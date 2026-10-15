#include "SLFlythroughMetrics.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLFlythroughHDRMetricsTest, "SL.Flythrough.HDRMetrics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLFlythroughHDRMetricsTest::RunTest(const FString& Parameters)
{
	// ---- relMSE / SMAPE (hand-computed, eps = 0.01) ----
	{
		const TArray<FLinearColor> Ref = { FLinearColor(1, 1, 1, 1), FLinearColor(0, 0, 0, 1) };
		const FSLHDRErrors Same = FSLFlythroughMetrics::ComputeHDRErrors(Ref, Ref);
		TestEqual(TEXT("relMSE of identical images"), Same.RelMSE, 0.0);
		TestEqual(TEXT("SMAPE of identical images"), Same.SMAPE, 0.0);

		const TArray<FLinearColor> Test = { FLinearColor(2, 2, 2, 1), FLinearColor(0, 0, 0, 1) };
		const FSLHDRErrors E = FSLFlythroughMetrics::ComputeHDRErrors(Test, Ref);
		// Pixel 0: (2-1)^2 / (1 + 0.01) per channel; pixel 1 contributes 0. Mean over 2 pixels.
		TestEqual(TEXT("relMSE"), E.RelMSE, (1.0 / 1.01) / 2.0, 1e-9);
		TestEqual(TEXT("SMAPE"), E.SMAPE, (1.0 / 3.01) / 2.0, 1e-9);

		const FSLHDRErrors Mismatch = FSLFlythroughMetrics::ComputeHDRErrors(Test, { FLinearColor::Black });
		TestEqual(TEXT("Size mismatch yields zero"), Mismatch.RelMSE, 0.0);
	}

	// ---- Display transform: Reinhard + gamma 2.2, negatives clamp to 0 ----
	{
		TArray<float> D;
		FSLFlythroughMetrics::ToDisplay({ FLinearColor(1, 0, -1, 1) }, D);
		TestEqual(TEXT("Display RGB count"), D.Num(), 3);
		TestEqual(TEXT("Display of 1.0"), D[0], (float)FMath::Pow(0.5, 1.0 / 2.2), 1e-6f);
		TestEqual(TEXT("Display of 0"), D[1], 0.f);
		TestEqual(TEXT("Display of negative"), D[2], 0.f);
	}

	// ---- Temporal flicker: |dTest - dRef| ----
	{
		TestEqual(TEXT("Flicker beyond reference change"),
			FSLFlythroughMetrics::ComputeTemporalFlicker({ 0.f, 0.f }, { 0.5f, 0.1f }, { 0.f, 0.f }, { 0.2f, 0.1f }),
			0.15, 1e-6);
		TestEqual(TEXT("Output that tracks the reference has no flicker"),
			FSLFlythroughMetrics::ComputeTemporalFlicker({ 0.1f }, { 0.4f }, { 0.1f }, { 0.4f }), 0.0, 1e-9);
	}

	// ---- EXR round trip, HDR manifest and completeness ----
	const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("SLFlythroughTests"), FGuid::NewGuid().ToString());
	{
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Dir, TEXT("frames_hdr")), true);
		const TArray<FFloat16Color> Half = { FFloat16Color(FLinearColor(3.5f, 1000.5f, 0.25f, 1.f)), FFloat16Color(FLinearColor(0.f, 0.f, 0.f, 1.f)) };
		for (int32 i = 0; i < 2; ++i)
		{
			const FString Path = FPaths::Combine(Dir, TEXT("frames_hdr"), FString::Printf(TEXT("frame_%04d.exr"), i));
			TestTrue(TEXT("Save EXR"), FImageUtils::SaveImageByExtension(*Path, FImageView(Half.GetData(), 2, 1)));
		}

		TArray<FLinearColor> Loaded;
		int32 W = 0, H = 0;
		TestTrue(TEXT("Load EXR"), FSLFlythroughMetrics::LoadEXR(FPaths::Combine(Dir, TEXT("frames_hdr"), TEXT("frame_0000.exr")), Loaded, W, H));
		TestEqual(TEXT("EXR width"), W, 2);
		TestEqual(TEXT("EXR height"), H, 1);
		if (Loaded.Num() == 2)
		{
			TestEqual(TEXT("EXR keeps values above 1 (R)"), Loaded[0].R, 3.5f);
			TestEqual(TEXT("EXR keeps values above 1 (G)"), Loaded[0].G, 1000.5f);
			TestEqual(TEXT("EXR keeps small values"), Loaded[0].B, 0.25f);
		}

		TestFalse(TEXT("Run without manifest is not HDR-complete"), FSLFlythroughMetrics::IsHDRRunComplete(Dir, 2));
		TestTrue(TEXT("Write manifest"), FSLFlythroughMetrics::WriteHDRManifest(Dir, { 1.f, 1.f }));
		TestTrue(TEXT("Complete with manifest and frames"), FSLFlythroughMetrics::IsHDRRunComplete(Dir, 2));
		TestFalse(TEXT("Frame count must match"), FSLFlythroughMetrics::IsHDRRunComplete(Dir, 3));
	}

	// ---- Summary CSV: unset HDR values stay unset; legacy 7-column files still parse ----
	{
		FSLMethodSummary WithHDR;
		WithHDR.Method = TEXT("1SPP_Noisy");
		WithHDR.RunId = TEXT("run_001");
		WithHDR.AvgRelMSE = 0.125;
		WithHDR.AvgSMAPE = 0.25;
		WithHDR.AvgTemporalFlicker = 0.5;
		WithHDR.Reference = TEXT("64SPP_GT/run_001");
		FSLMethodSummary WithoutHDR;
		WithoutHDR.Method = TEXT("1SPP_Gaussian");
		WithoutHDR.RunId = TEXT("run_002");

		TestTrue(TEXT("Write summary"), FSLFlythroughMetrics::WriteSummaryCSV(Dir, { WithHDR, WithoutHDR }));
		TArray<FSLMethodSummary> Read;
		TestTrue(TEXT("Read summary"), FSLFlythroughMetrics::ReadSummaryCSV(Dir, Read));
		if (TestEqual(TEXT("Summary rows"), Read.Num(), 2))
		{
			TestTrue(TEXT("relMSE round trip"), Read[0].AvgRelMSE.IsSet() && FMath::IsNearlyEqual(Read[0].AvgRelMSE.GetValue(), 0.125));
			TestTrue(TEXT("Flicker round trip"), Read[0].AvgTemporalFlicker.IsSet() && FMath::IsNearlyEqual(Read[0].AvgTemporalFlicker.GetValue(), 0.5));
			TestEqual(TEXT("Reference round trip"), Read[0].Reference, WithHDR.Reference);
			TestFalse(TEXT("Missing HDR stays unset (not zero)"), Read[1].AvgRelMSE.IsSet());
			TestFalse(TEXT("Missing flicker stays unset"), Read[1].AvgTemporalFlicker.IsSet());
		}

		FFileHelper::SaveStringToFile(TEXT("method,run_id,avg_render_ms,avg_mse,avg_psnr,avg_ssim,avg_cnr\nOld,run_001,1,2,3,0.5,4\n"),
			*FPaths::Combine(Dir, TEXT("summary_metrics.csv")));
		TestTrue(TEXT("Read legacy summary"), FSLFlythroughMetrics::ReadSummaryCSV(Dir, Read));
		if (TestEqual(TEXT("Legacy rows"), Read.Num(), 1))
		{
			TestEqual(TEXT("Legacy CNR"), Read[0].AvgCNR, 4.0);
			TestFalse(TEXT("Legacy rows have no HDR"), Read[0].AvgRelMSE.IsSet());
		}
	}

	// ---- Per-frame averages: flicker skips frame 0, partial HDR data never averages in ----
	{
		TArray<FSLFrameMetrics> Frames;
		Frames.AddDefaulted(3);
		for (int32 i = 0; i < 3; ++i) { Frames[i].RelMSE = 0.1 * (i + 1); Frames[i].SMAPE = 0.2; }
		Frames[1].TemporalFlicker = 0.3;
		Frames[2].TemporalFlicker = 0.5;
		FSLMethodSummary S;
		TestTrue(TEXT("Summary has HDR"), FSLFlythroughMetrics::ComputeSummaryHDR(Frames, S));
		TestTrue(TEXT("relMSE average"), S.AvgRelMSE.IsSet() && FMath::IsNearlyEqual(S.AvgRelMSE.GetValue(), 0.2));
		TestTrue(TEXT("Flicker averages frames 1..N-1"), S.AvgTemporalFlicker.IsSet() && FMath::IsNearlyEqual(S.AvgTemporalFlicker.GetValue(), 0.4));

		Frames[2].RelMSE.Reset();
		FSLMethodSummary Partial;
		TestFalse(TEXT("Partial HDR data is not averaged"), FSLFlythroughMetrics::ComputeSummaryHDR(Frames, Partial));
	}

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

#endif

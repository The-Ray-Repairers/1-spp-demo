#include "SLFlythroughMetrics.h"
#include "SL_Test.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

// -----------------------------------------------------------------------
// PNG Loading
// -----------------------------------------------------------------------

bool FSLFlythroughMetrics::LoadPNG(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *Path))
	{
		UE_LOG(LogSL_Test, Warning, TEXT("FlythroughMetrics: Failed to load file %s"), *Path);
		return false;
	}

	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(FileData.GetData(), FileData.Num()))
	{
		UE_LOG(LogSL_Test, Warning, TEXT("FlythroughMetrics: Failed to decompress PNG %s"), *Path);
		return false;
	}

	OutWidth = Wrapper->GetWidth();
	OutHeight = Wrapper->GetHeight();

	TArray<uint8> RawData;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
	{
		UE_LOG(LogSL_Test, Warning, TEXT("FlythroughMetrics: Failed to get raw pixels from %s"), *Path);
		return false;
	}

	const int64 PixelCount = int64(OutWidth) * OutHeight;
	if (OutWidth <= 0 || OutHeight <= 0 || PixelCount > MAX_int32
		|| RawData.Num() != PixelCount * sizeof(FColor)) return false;
	const int32 NumPixels = int32(PixelCount);
	OutPixels.SetNumUninitialized(NumPixels);
	FMemory::Memcpy(OutPixels.GetData(), RawData.GetData(), NumPixels * sizeof(FColor));
	return true;
}

// -----------------------------------------------------------------------
// Luminance
// -----------------------------------------------------------------------

double FSLFlythroughMetrics::ToLuminance(const FColor& C)
{
	// BT.709 luminance in [0, 255] from sRGB components.
	return 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B;
}

// -----------------------------------------------------------------------
// Per-frame Metrics
// -----------------------------------------------------------------------

FSLFrameMetrics FSLFlythroughMetrics::ComputeFrameMetrics(
	int32 FrameIndex,
	const TArray<FColor>& TestPixels,
	const TArray<FColor>& GTPixels,
	int32 Width, int32 Height)
{
	FSLFrameMetrics Result;
	Result.FrameIndex = FrameIndex;

	const int64 PixelCount = int64(Width) * Height;
	if (Width <= 0 || Height <= 0 || PixelCount > MAX_int32
		|| TestPixels.Num() != PixelCount || GTPixels.Num() != PixelCount)
	{
		UE_LOG(LogSL_Test, Warning, TEXT("FlythroughMetrics: Dimension mismatch for frame %d"), FrameIndex);
		return Result;
	}
	const int32 N = int32(PixelCount);

	// ---- MSE (per-channel, [0,255] space) ----
	double SumSqErr = 0.0;
	TArray<double> TestLum, GTLum;
	TestLum.SetNumUninitialized(N);
	GTLum.SetNumUninitialized(N);

	for (int32 i = 0; i < N; ++i)
	{
		const FColor& T = TestPixels[i];
		const FColor& G = GTPixels[i];
		const double dR = (double)T.R - (double)G.R;
		const double dG = (double)T.G - (double)G.G;
		const double dB = (double)T.B - (double)G.B;
		SumSqErr += (dR * dR + dG * dG + dB * dB) / 3.0; // average across channels
		TestLum[i] = ToLuminance(T);
		GTLum[i] = ToLuminance(G);
	}
	Result.MSE = SumSqErr / (double)N;

	// ---- PSNR ----
	constexpr double MaxVal = 255.0;
	if (Result.MSE > 0.0)
		Result.PSNR = 10.0 * FMath::LogX(10.0, (MaxVal * MaxVal) / Result.MSE);
	else
		Result.PSNR = 100.0; // perfect match cap

	// ---- SSIM ----
	Result.SSIM = ComputeSSIM(TestLum, GTLum, Width, Height);

	// ---- CNR (full-reference, linear ratio) ----
	// σ_G = spatial std-dev of GT luminance
	// σ_R = spatial std-dev of residual luminance (Test - GT)
	// CNR = σ_G / σ_R; floor a zero residual to keep CSV/chart values finite.
	{
		constexpr double Epsilon = 1e-8;
		double MeanGT = 0.0, MeanRes = 0.0;
		for (int32 i = 0; i < N; ++i)
		{
			MeanGT += GTLum[i];
			MeanRes += (TestLum[i] - GTLum[i]);
		}
		MeanGT /= N;
		MeanRes /= N;

		double VarGT = 0.0, VarRes = 0.0;
		for (int32 i = 0; i < N; ++i)
		{
			const double dGT = GTLum[i] - MeanGT;
			const double dRes = (TestLum[i] - GTLum[i]) - MeanRes;
			VarGT += dGT * dGT;
			VarRes += dRes * dRes;
		}
		VarGT /= N;
		VarRes /= N;

		const double SigmaGT = FMath::Sqrt(VarGT);
		const double SigmaRes = FMath::Sqrt(VarRes);
		Result.CNR = SigmaGT / FMath::Max(SigmaRes, Epsilon);
	}

	return Result;
}

// -----------------------------------------------------------------------
// SSIM (Wang et al. 2004, 11×11 Gaussian-weighted window)
// -----------------------------------------------------------------------

double FSLFlythroughMetrics::ComputeSSIM(
	const TArray<double>& TestLum,
	const TArray<double>& GTLum,
	int32 Width, int32 Height)
{
	// Constants for 8-bit dynamic range
	constexpr double C1 = 6.5025;   // (0.01 * 255)^2
	constexpr double C2 = 58.5225;  // (0.03 * 255)^2
	const int32 WindowSize = ((FMath::Min(11, FMath::Min(Width, Height)) - 1) / 2) * 2 + 1;
	const int32 HalfW = WindowSize / 2;

	// Build a 1D Gaussian kernel (sigma = 1.5)
	double Kernel[11];
	constexpr double Sigma = 1.5;
	double KernelSum = 0.0;
	for (int32 i = 0; i < WindowSize; ++i)
	{
		const double d = (double)(i - HalfW);
		Kernel[i] = FMath::Exp(-0.5 * d * d / (Sigma * Sigma));
		KernelSum += Kernel[i];
	}
	for (int32 i = 0; i < WindowSize; ++i) Kernel[i] /= KernelSum;

	// 2D separable Gaussian weights
	double Weights[11][11];
	for (int32 y = 0; y < WindowSize; ++y)
		for (int32 x = 0; x < WindowSize; ++x)
			Weights[y][x] = Kernel[y] * Kernel[x];

	double TotalSSIM = 0.0;
	int32 WindowCount = 0;

	// Sample valid window positions with stride 4.
	for (int32 cy = HalfW; cy < Height - HalfW; cy += 4) // stride 4 for performance
	{
		for (int32 cx = HalfW; cx < Width - HalfW; cx += 4)
		{
			double MuX = 0, MuY = 0;
			for (int32 wy = 0; wy < WindowSize; ++wy)
			{
				const int32 py = cy - HalfW + wy;
				for (int32 wx = 0; wx < WindowSize; ++wx)
				{
					const int32 px = cx - HalfW + wx;
					const int32 idx = py * Width + px;
					const double w = Weights[wy][wx];
					MuX += w * TestLum[idx];
					MuY += w * GTLum[idx];
				}
			}

			double SigmaXX = 0, SigmaYY = 0, SigmaXY = 0;
			for (int32 wy = 0; wy < WindowSize; ++wy)
			{
				const int32 py = cy - HalfW + wy;
				for (int32 wx = 0; wx < WindowSize; ++wx)
				{
					const int32 px = cx - HalfW + wx;
					const int32 idx = py * Width + px;
					const double w = Weights[wy][wx];
					const double dx = TestLum[idx] - MuX;
					const double dy = GTLum[idx] - MuY;
					SigmaXX += w * dx * dx;
					SigmaYY += w * dy * dy;
					SigmaXY += w * dx * dy;
				}
			}

			const double Num = (2.0 * MuX * MuY + C1) * (2.0 * SigmaXY + C2);
			const double Den = (MuX * MuX + MuY * MuY + C1) * (SigmaXX + SigmaYY + C2);
			TotalSSIM += Num / Den;
			++WindowCount;
		}
	}

	return WindowCount > 0 ? TotalSSIM / WindowCount : 1.0;
}

// -----------------------------------------------------------------------
// CSV I/O
// -----------------------------------------------------------------------

bool FSLFlythroughMetrics::WriteTimingsCSV(const FString& Path, const TArray<FSLFrameTiming>& Timings)
{
	FString CSV = TEXT("frame_idx,render_time_ms\n");
	double Total = 0.0;
	for (const auto& T : Timings)
	{
		CSV += FString::Printf(TEXT("%d,%.3f\n"), T.FrameIndex, T.RenderTimeMs);
		Total += T.RenderTimeMs;
	}
	const double Avg = Timings.Num() > 0 ? Total / Timings.Num() : 0.0;
	CSV += FString::Printf(TEXT("avg,%.3f\n"), Avg);
	return FFileHelper::SaveStringToFile(CSV, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FSLFlythroughMetrics::WriteMetricsCSV(const FString& Path, const TArray<FSLFrameMetrics>& Metrics)
{
	FString CSV = TEXT("frame_idx,mse,psnr,ssim,cnr\n");
	double TotalMSE = 0, TotalPSNR = 0, TotalSSIM = 0, TotalCNR = 0;
	for (const auto& M : Metrics)
	{
		CSV += FString::Printf(TEXT("%d,%.6f,%.4f,%.6f,%.4f\n"), M.FrameIndex, M.MSE, M.PSNR, M.SSIM, M.CNR);
		TotalMSE += M.MSE;
		TotalPSNR += M.PSNR;
		TotalSSIM += M.SSIM;
		TotalCNR += M.CNR;
	}
	const int32 N = Metrics.Num();
	if (N > 0)
		CSV += FString::Printf(TEXT("avg,%.6f,%.4f,%.6f,%.4f\n"), TotalMSE / N, TotalPSNR / N, TotalSSIM / N, TotalCNR / N);
	return FFileHelper::SaveStringToFile(CSV, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FSLFlythroughMetrics::WriteSummaryCSV(const FString& RendersRoot, const TArray<FSLMethodSummary>& Summaries)
{
	FString CSV = TEXT("method,run_id,avg_render_ms,avg_mse,avg_psnr,avg_ssim,avg_cnr\n");
	for (const auto& S : Summaries)
	{
		CSV += FString::Printf(TEXT("%s,%s,%.3f,%.6f,%.4f,%.6f,%.4f\n"),
			*S.Method, *S.RunId, S.AvgRenderMs, S.AvgMSE, S.AvgPSNR, S.AvgSSIM, S.AvgCNR);
	}
	return FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(RendersRoot, TEXT("summary_metrics.csv")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FSLFlythroughMetrics::ReadSummaryCSV(const FString& RendersRoot, TArray<FSLMethodSummary>& OutSummaries)
{
	OutSummaries.Reset();
	FString CSV;
	if (!FFileHelper::LoadFileToString(CSV, *FPaths::Combine(RendersRoot, TEXT("summary_metrics.csv"))))
		return false;

	TArray<FString> Lines;
	CSV.ParseIntoArrayLines(Lines);
	// Skip header line
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		TArray<FString> Fields;
		Lines[i].ParseIntoArray(Fields, TEXT(","));
		if (Fields.Num() < 7) continue;

		FSLMethodSummary S;
		S.Method = Fields[0];
		S.RunId = Fields[1];
		S.AvgRenderMs = FCString::Atod(*Fields[2]);
		S.AvgMSE = FCString::Atod(*Fields[3]);
		S.AvgPSNR = FCString::Atod(*Fields[4]);
		S.AvgSSIM = FCString::Atod(*Fields[5]);
		S.AvgCNR = FCString::Atod(*Fields[6]);
		OutSummaries.Add(MoveTemp(S));
	}
	return true;
}

double FSLFlythroughMetrics::ReadAvgRenderTime(const FString& TimingsPath)
{
	FString CSV;
	if (!FFileHelper::LoadFileToString(CSV, *TimingsPath)) return 0.0;

	TArray<FString> Lines;
	CSV.ParseIntoArrayLines(Lines);
	// Last line should be "avg,<value>"
	for (int32 i = Lines.Num() - 1; i >= 0; --i)
	{
		if (Lines[i].StartsWith(TEXT("avg")))
		{
			TArray<FString> Fields;
			Lines[i].ParseIntoArray(Fields, TEXT(","));
			if (Fields.Num() >= 2) return FCString::Atod(*Fields[1]);
		}
	}
	return 0.0;
}

// -----------------------------------------------------------------------
// Run Directory Helpers
// -----------------------------------------------------------------------

FString FSLFlythroughMetrics::FindLatestRun(const FString& MethodDir)
{
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *FPaths::Combine(MethodDir, TEXT("*")), false, true);

	FString Latest;
	int32 LatestNum = -1;
	for (const FString& Dir : Dirs)
	{
		if (Dir.StartsWith(TEXT("run_")) && IsRunComplete(FPaths::Combine(MethodDir, Dir)))
		{
			const int32 Num = FCString::Atoi(*Dir.Mid(4));
			if (Num > LatestNum)
			{
				LatestNum = Num;
				Latest = Dir;
			}
		}
	}
	return Latest.IsEmpty() ? FString() : FPaths::Combine(MethodDir, Latest);
}

FString FSLFlythroughMetrics::GetNextRunId(const FString& MethodDir)
{
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *FPaths::Combine(MethodDir, TEXT("*")), false, true);

	int32 MaxNum = 0;
	for (const FString& Dir : Dirs)
	{
		if (Dir.StartsWith(TEXT("run_")))
		{
			MaxNum = FMath::Max(MaxNum, FCString::Atoi(*Dir.Mid(4)));
		}
	}
	return FString::Printf(TEXT("run_%03d"), MaxNum + 1);
}

bool FSLFlythroughMetrics::IsRunComplete(const FString& RunDir)
{
	// timings.csv is written only after capture finishes. Verify every recorded frame.
	if (!IFileManager::Get().DirectoryExists(*RunDir)) return false;
	FString CSV;
	if (!FFileHelper::LoadFileToString(CSV, *FPaths::Combine(RunDir, TEXT("timings.csv")))) return false;
	TArray<FString> Lines;
	CSV.ParseIntoArrayLines(Lines);
	if (Lines.Num() < 3 || Lines[0] != TEXT("frame_idx,render_time_ms") || !Lines.Last().StartsWith(TEXT("avg,"))) return false;

	TArray<FString> Frames;
	IFileManager::Get().FindFiles(Frames, *FPaths::Combine(RunDir, TEXT("frames"), TEXT("*.png")), true, false);
	if (Frames.Num() != Lines.Num() - 2) return false;
	Frames.Sort();
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		if (!Lines[Index + 1].StartsWith(FString::Printf(TEXT("%d,"), Index))
			|| Frames[Index] != FString::Printf(TEXT("frame_%04d.png"), Index)
			|| IFileManager::Get().FileSize(*FPaths::Combine(RunDir, TEXT("frames"), Frames[Index])) <= 0) return false;
	}
	return true;
}

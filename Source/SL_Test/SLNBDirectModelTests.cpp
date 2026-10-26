// Copyright Epic Games, Inc. All Rights Reserved.
//
// Checks /Game/ML/nb_direct the way SL.Denoiser.Method 6 runs it: on NNERuntimeORTDml through
// RDG, at several view sizes (the model has dynamic H, W), against NNERuntimeORTCpu on the same
// input. Needs a D3D12 RHI; under -nullrhi the DirectML half is skipped with a warning.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Math/RandomStream.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "NNERuntimeRDG.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

namespace SLNBDirectTest
{
	constexpr int32 RawChannels = 10;
	constexpr int32 OutChannels = 3;

	/** ue_buffers in the NoiseBase encoding SLRayRepairPreprocess.usf writes for method 6. */
	TArray<float> MakeInput(int32 W, int32 H, int32 Seed)
	{
		FRandomStream Rng(Seed);
		const int32 Plane = W * H;
		TArray<float> X;
		X.SetNumZeroed(RawChannels * Plane);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const int32 I = y * W + x;
				const float Light = 0.2f + 2.f * FMath::Abs(FMath::Sin(x * 0.02f) * FMath::Cos(y * 0.03f));
				const FVector3f Albedo(0.2f + 0.6f * (x % 64) / 64.f, 0.5f, 0.3f + 0.5f * (y % 32) / 32.f);
				for (int32 c = 0; c < 3; ++c)
				{
					// 1 spp-like noise with occasional fireflies on top of smooth lighting.
					const float Noise = Rng.FRand() < 0.002f ? 200.f : -FMath::Loge(FMath::Max(Rng.FRand(), 1e-4f));
					X[c * Plane + I] = Albedo[c] * Light * Noise;
					X[(3 + c) * Plane + I] = Albedo[c];
				}
				const FVector3f N = FVector3f(-1.f, 0.3f * FMath::Sin(x * 0.01f), 0.3f * FMath::Cos(y * 0.01f)).GetSafeNormal();
				X[6 * Plane + I] = N.X;   // n . forward (faces the camera)
				X[7 * Plane + I] = N.Y;   // n . right
				X[8 * Plane + I] = N.Z;   // n . up
				X[9 * Plane + I] = y < H / 8 ? 1e8f : 200.f + 4000.f * x / W;   // sky band, then a floor ramp (cm)
			}
		}
		return X;
	}

	UE::NNE::FTensorShape Shape(int32 C, int32 W, int32 H)
	{
		return UE::NNE::FTensorShape::Make({ 1u, (uint32)C, (uint32)H, (uint32)W });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLNBDirectModelTest, "SL.Denoiser.NBDirectModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLNBDirectModelTest::RunTest(const FString& Parameters)
{
	using namespace SLNBDirectTest;

	UNNEModelData* ModelData = LoadObject<UNNEModelData>(nullptr, TEXT("/Game/ML/nb_direct.nb_direct"));
	if (!TestNotNull(TEXT("/Game/ML/nb_direct loads"), ModelData)) return false;

	// ---- CPU reference (also checks the asset's graph and dynamic shapes) ----
	TWeakInterfacePtr<INNERuntimeCPU> CpuRuntime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
	if (!TestTrue(TEXT("NNERuntimeORTCpu available"), CpuRuntime.IsValid())) return false;
	TSharedPtr<UE::NNE::IModelCPU> CpuModel = CpuRuntime->CreateModelCPU(ModelData);
	TSharedPtr<UE::NNE::IModelInstanceCPU> Cpu = CpuModel.IsValid() ? CpuModel->CreateModelInstanceCPU() : nullptr;
	if (!TestTrue(TEXT("CPU model instance"), Cpu.IsValid())) return false;

	const TConstArrayView<UE::NNE::FTensorDesc> InDescs = Cpu->GetInputTensorDescs();
	if (!TestEqual(TEXT("One input"), InDescs.Num(), 1)) return false;
	const TConstArrayView<int32> Dims = InDescs[0].GetShape().GetData();
	TestTrue(TEXT("Input is [1, 10, H, W] with dynamic H, W"),
		Dims.Num() == 4 && Dims[1] == RawChannels && Dims[2] < 0 && Dims[3] < 0);

	// ---- DirectML through RDG, as FSLDenoiserRayRepair runs it ----
	TWeakInterfacePtr<INNERuntimeRDG> DmlRuntime = UE::NNE::GetRuntime<INNERuntimeRDG>(TEXT("NNERuntimeORTDml"));
	TSharedPtr<UE::NNE::IModelInstanceRDG> Dml;
	if (DmlRuntime.IsValid())
	{
		TSharedPtr<UE::NNE::IModelRDG> DmlModel = DmlRuntime->CreateModelRDG(ModelData);
		Dml = DmlModel.IsValid() ? DmlModel->CreateModelInstanceRDG() : nullptr;
		TestTrue(TEXT("DirectML model instance"), Dml.IsValid());
	}
	else
	{
		AddWarning(TEXT("NNERuntimeORTDml unavailable (no D3D12 RHI?); only the CPU runtime was checked"));
	}

	const FIntPoint Sizes[] = { FIntPoint(384, 256), FIntPoint(481, 270), FIntPoint(1920, 1080) };
	for (const FIntPoint& Size : Sizes)
	{
		const int32 W = Size.X, H = Size.Y;
		const TArray<float> In = MakeInput(W, H, W + H);
		TArray<float> Ref;
		Ref.SetNumZeroed(OutChannels * W * H);

		const FString Tag = FString::Printf(TEXT("%dx%d"), W, H);
		TArray<UE::NNE::FTensorShape> InShapes = { Shape(RawChannels, W, H) };
		if (!TestTrue(Tag + TEXT(" CPU SetInputTensorShapes"), Cpu->SetInputTensorShapes(InShapes) == UE::NNE::EResultStatus::Ok)) continue;
		UE::NNE::FTensorBindingCPU InBinding{ const_cast<float*>(In.GetData()), (uint64)In.Num() * sizeof(float) };
		UE::NNE::FTensorBindingCPU OutBinding{ Ref.GetData(), (uint64)Ref.Num() * sizeof(float) };
		if (!TestTrue(Tag + TEXT(" CPU RunSync"), Cpu->RunSync({ InBinding }, { OutBinding }) == UE::NNE::IModelInstanceCPU::ERunSyncStatus::Ok)) continue;

		float RefMax = 0.f;
		bool bRefValid = true;
		for (float V : Ref) { bRefValid &= FMath::IsFinite(V) && V >= 0.f; RefMax = FMath::Max(RefMax, V); }
		TestTrue(Tag + TEXT(" CPU output finite and nonnegative"), bRefValid);
		TestTrue(Tag + TEXT(" CPU output not all zero"), RefMax > 0.f);

		if (!Dml.IsValid()) continue;

		TArray<float> Out;
		Out.SetNumZeroed(Ref.Num());
		bool bShapesOk = false, bEnqueueOk = false;
		ENQUEUE_RENDER_COMMAND(SLNBDirectTestRun)([&](FRHICommandListImmediate& RHICmdList)
		{
			bShapesOk = Dml->SetInputTensorShapes(InShapes) == UE::NNE::EResultStatus::Ok;
			if (!bShapesOk) return;

			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef InBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), In.Num()), TEXT("NBDirectTest.In"));
			FRDGBufferRef OutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), Out.Num()), TEXT("NBDirectTest.Out"));
			GraphBuilder.QueueBufferUpload(InBuffer, In.GetData(), In.Num() * sizeof(float));

			UE::NNE::FTensorBindingRDG InRDG; InRDG.Buffer = InBuffer;
			UE::NNE::FTensorBindingRDG OutRDG; OutRDG.Buffer = OutBuffer;
			bEnqueueOk = Dml->EnqueueRDG(GraphBuilder, { InRDG }, { OutRDG }) == UE::NNE::EResultStatus::Ok;

			FRHIGPUBufferReadback Readback(TEXT("NBDirectTest.Readback"));
			const uint32 NumBytes = Out.Num() * sizeof(float);
			AddEnqueueCopyPass(GraphBuilder, &Readback, OutBuffer, NumBytes);
			GraphBuilder.Execute();

			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (bEnqueueOk && Readback.IsReady())
			{
				FMemory::Memcpy(Out.GetData(), Readback.Lock(NumBytes), NumBytes);
				Readback.Unlock();
			}
			else
			{
				bEnqueueOk = false;
			}
		});
		FlushRenderingCommands();

		if (!TestTrue(Tag + TEXT(" DirectML SetInputTensorShapes"), bShapesOk)) continue;
		if (!TestTrue(Tag + TEXT(" DirectML EnqueueRDG + readback"), bEnqueueOk)) continue;

		double MaxDiff = 0.0, SumDiff = 0.0;
		bool bValid = true;
		for (int32 i = 0; i < Out.Num(); ++i)
		{
			bValid &= FMath::IsFinite(Out[i]) && Out[i] >= 0.f;
			const double D = FMath::Abs((double)Out[i] - Ref[i]);
			MaxDiff = FMath::Max(MaxDiff, D);
			SumDiff += D;
		}
		const double RelMax = MaxDiff / FMath::Max((double)RefMax, 1e-6);
		AddInfo(FString::Printf(TEXT("%s: DirectML vs CPU max abs diff %.3g (%.2e of output max %.3g), mean %.3g"),
			*Tag, MaxDiff, RelMax, RefMax, SumDiff / Out.Num()));
		TestTrue(Tag + TEXT(" DirectML output finite and nonnegative"), bValid);
		TestTrue(Tag + TEXT(" DirectML matches CPU (max diff < 1% of output range)"), RelMax < 1e-2);
	}
	return true;
}

#endif

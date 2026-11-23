// Copyright Epic Games, Inc. All Rights Reserved.
//
// Tests for the recurrent, motion-aware Ray Repair model (SL.Denoiser.Method 7).
//
// SL.Denoiser.RayRepairMotion.Model
//   Editor context. Checks the asset's signature against neural-denoiser docs/UE_INTEGRATION.md,
//   then runs three recurrent frames on NNERuntimeORTDml through RDG and on NNERuntimeORTCpu, feeding
//   each frame's hidden_out back as the next frame's hidden_in, and compares the two runtimes.
//   Needs a D3D12 RHI (for example -RenderOffscreen -dx12); under -nullrhi only the CPU half runs.
//
// SL.Denoiser.RayRepairMotion.InGame
//   Client context, in a 1920x1080 game. Switches the live renderer to method 7 while the camera
//   turns, checks that the model runs every frame and carries its hidden state, and logs the
//   wall-clock frame time with method 0 and method 7. Then takes an SL.Denoiser.RayRepair.Dump and
//   checks the files it writes (Saved/RayRepairDump). Waits for shader compilation first; add
//   -dpcvars=r.RayTracing.NonBlockingPipelineCreation=0 so the path tracer does not render its black
//   fallback while ray tracing pipelines compile. For example (no window with -RenderOffscreen):
//     UnrealEditor.exe SL_Test.uproject /Game/Levels/LVL_RT_Room -game -RenderOffscreen -dx12 -windowed -ResX=1920 -ResY=1080 -ForceRes
//       -dpcvars=r.RayTracing.NonBlockingPipelineCreation=0 -ExecCmds="r.SetRes 1920x1080w,Automation RunTests SL.Denoiser.RayRepairMotion.InGame"
//       -TestExit="Automation Test Queue Empty"

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SLDenoiser.h"
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
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

namespace SLRayRepairMotionTest
{
	constexpr int32 W = 1920;
	constexpr int32 H = 1080;

	/** The input names and shapes the 1-spp-demo integration expects (UE_INTEGRATION.md, 3x model). */
	struct FExpected { const TCHAR* Name; TArray<int32> Shape; };
	const TArray<FExpected>& ExpectedInputs()
	{
		static const TArray<FExpected> E = {
			{ TEXT("ue_buffers"), { 1, 10, H, W } },
			{ TEXT("motion_px"),  { 1, 2, H, W } },
			{ TEXT("hidden_in0"), { 1, 32, 368, 640 } },
			{ TEXT("hidden_in1"), { 1, 64, 184, 320 } },
			{ TEXT("hidden_in2"), { 1, 128, 92, 160 } },
			{ TEXT("hidden_in3"), { 1, 256, 46, 80 } },
			{ TEXT("hidden_in4"), { 1, 256, 23, 40 } },
		};
		return E;
	}

	/** ue_buffers in the NoiseBase encoding SLRayRepairPreprocess.usf writes for methods 6 and 7. Frame shifts the image. */
	TArray<float> MakeBuffers(int32 Frame)
	{
		FRandomStream Rng(1000 + Frame);
		const int32 Plane = W * H;
		TArray<float> X;
		X.SetNumZeroed(10 * Plane);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const int32 I = y * W + x;
				const int32 sx = x + 2 * Frame;   // content moves 2 px left per frame
				const float Light = 0.2f + 2.f * FMath::Abs(FMath::Sin(sx * 0.02f) * FMath::Cos(y * 0.03f));
				const FVector3f Albedo(0.2f + 0.6f * (sx % 64) / 64.f, 0.5f, 0.3f + 0.5f * (y % 32) / 32.f);
				for (int32 c = 0; c < 3; ++c)
				{
					const float Noise = Rng.FRand() < 0.002f ? 200.f : -FMath::Loge(FMath::Max(Rng.FRand(), 1e-4f));
					X[c * Plane + I] = Albedo[c] * Light * Noise;
					X[(3 + c) * Plane + I] = Albedo[c];
				}
				const FVector3f N = FVector3f(-1.f, 0.3f * FMath::Sin(sx * 0.01f), 0.3f * FMath::Cos(y * 0.01f)).GetSafeNormal();
				X[6 * Plane + I] = N.X;
				X[7 * Plane + I] = N.Y;
				X[8 * Plane + I] = N.Z;
				X[9 * Plane + I] = y < H / 8 ? 1e8f : 200.f + 4000.f * x / W;
			}
		}
		return X;
	}

	/**
	 * Previous minus current pixel position. Frames 0-1: content moved 2 px left, so it was 2 px to the
	 * right. Frame 2: random motion up to +-5000 px, which sends most samples far off-screen (camera cuts,
	 * the sky, fast turns) and would make an out-of-bounds read in the warp page-fault.
	 */
	TArray<float> MakeMotion(int32 Frame)
	{
		TArray<float> M;
		M.SetNumZeroed(2 * W * H);
		if (Frame < 2)
		{
			for (int32 i = 0; i < W * H; ++i) M[i] = 2.f;
			return M;
		}
		FRandomStream Rng(77);
		for (float& V : M) V = (Rng.FRand() - 0.5f) * 10000.f;
		return M;
	}

	FString Shape(TConstArrayView<int32> Dims)
	{
		return TEXT("[") + FString::JoinBy(Dims, TEXT(", "), [](int32 D) { return FString::FromInt(D); }) + TEXT("]");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRayRepairMotionModelTest, "SL.Denoiser.RayRepairMotion.Model",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRayRepairMotionModelTest::RunTest(const FString& Parameters)
{
	using namespace SLRayRepairMotionTest;

	const IConsoleVariable* ModelVar = IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Denoiser.RayRepairMotion.Model"));
	const FString Path = ModelVar ? ModelVar->GetString() : FString();
	UNNEModelData* ModelData = LoadObject<UNNEModelData>(nullptr, *(Path + TEXT(".") + FPaths::GetBaseFilename(Path)));
	if (!TestNotNull(FString::Printf(TEXT("%s loads"), *Path), ModelData)) return false;

	// ---- Signature, on the CPU runtime ----
	TWeakInterfacePtr<INNERuntimeCPU> CpuRuntime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
	if (!TestTrue(TEXT("NNERuntimeORTCpu available"), CpuRuntime.IsValid())) return false;
	TSharedPtr<UE::NNE::IModelCPU> CpuModel = CpuRuntime->CreateModelCPU(ModelData);
	TSharedPtr<UE::NNE::IModelInstanceCPU> Cpu = CpuModel.IsValid() ? CpuModel->CreateModelInstanceCPU() : nullptr;
	if (!TestTrue(TEXT("CPU model instance"), Cpu.IsValid())) return false;

	const TConstArrayView<UE::NNE::FTensorDesc> InDescs = Cpu->GetInputTensorDescs();
	const TConstArrayView<UE::NNE::FTensorDesc> OutDescs = Cpu->GetOutputTensorDescs();
	if (!TestEqual(TEXT("Input count"), InDescs.Num(), ExpectedInputs().Num())) return false;
	if (!TestEqual(TEXT("Output count (denoised_rgb + 5 hidden)"), OutDescs.Num(), 6)) return false;
	TArray<UE::NNE::FTensorShape> InShapes;
	for (int32 i = 0; i < InDescs.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("Input %d name"), i), InDescs[i].GetName(), FString(ExpectedInputs()[i].Name));
		TestEqual(FString::Printf(TEXT("%s shape"), *InDescs[i].GetName()),
			Shape(InDescs[i].GetShape().GetData()), Shape(ExpectedInputs()[i].Shape));
		InShapes.Add(UE::NNE::FTensorShape::MakeFromSymbolic(InDescs[i].GetShape()));
	}
	TestEqual(TEXT("Output 0 name"), OutDescs[0].GetName(), FString(TEXT("denoised_rgb")));
	for (int32 h = 0; h < 5; ++h)
	{
		TestEqual(FString::Printf(TEXT("Output %d name"), h + 1), OutDescs[h + 1].GetName(), FString::Printf(TEXT("hidden_out%d"), h));
	}
	if (HasAnyErrors()) return false;
	if (!TestTrue(TEXT("CPU SetInputTensorShapes"), Cpu->SetInputTensorShapes(InShapes) == UE::NNE::EResultStatus::Ok)) return false;

	TArray<uint64> InFloats, OutFloats;
	for (const UE::NNE::FTensorShape& S : InShapes) InFloats.Add(S.Volume());
	OutFloats.Add(3ull * W * H);
	for (int32 h = 0; h < 5; ++h) OutFloats.Add(InFloats[2 + h]);

	// ---- DirectML instance, as FSLDenoiserRayRepair runs it ----
	TWeakInterfacePtr<INNERuntimeRDG> DmlRuntime = UE::NNE::GetRuntime<INNERuntimeRDG>(TEXT("NNERuntimeORTDml"));
	TSharedPtr<UE::NNE::IModelInstanceRDG> Dml;
	if (DmlRuntime.IsValid())
	{
		TSharedPtr<UE::NNE::IModelRDG> DmlModel = DmlRuntime->CreateModelRDG(ModelData);
		Dml = DmlModel.IsValid() ? DmlModel->CreateModelInstanceRDG() : nullptr;
		TestTrue(TEXT("DirectML model instance"), Dml.IsValid());
		if (Dml.IsValid()) TestTrue(TEXT("DirectML SetInputTensorShapes"), Dml->SetInputTensorShapes(InShapes) == UE::NNE::EResultStatus::Ok);
	}
	else
	{
		AddWarning(TEXT("NNERuntimeORTDml unavailable (no D3D12 RHI?); only the CPU runtime was checked"));
	}

	// ---- Three recurrent frames. Both runtimes get the same inputs, including the CPU's hidden state. ----
	TArray<TArray<float>> HiddenIn;
	for (int32 h = 0; h < 5; ++h) HiddenIn.Emplace_GetRef().SetNumZeroed(InFloats[2 + h]);
	TArray<float> PreviousDenoised;

	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		const FString Tag = FString::Printf(TEXT("Frame %d"), Frame);
		const TArray<float> Buffers = MakeBuffers(Frame);
		const TArray<float> Motion = MakeMotion(Frame);
		TArray<const TArray<float>*> Inputs = { &Buffers, &Motion };
		for (const TArray<float>& Hd : HiddenIn) Inputs.Add(&Hd);

		TArray<TArray<float>> Ref;
		for (uint64 N : OutFloats) Ref.Emplace_GetRef().SetNumZeroed(N);
		TArray<UE::NNE::FTensorBindingCPU> CpuIn, CpuOut;
		for (const TArray<float>* In : Inputs) CpuIn.Add({ const_cast<float*>(In->GetData()), (uint64)In->Num() * sizeof(float) });
		for (TArray<float>& Out : Ref) CpuOut.Add({ Out.GetData(), (uint64)Out.Num() * sizeof(float) });
		const double CpuStart = FPlatformTime::Seconds();
		if (!TestTrue(Tag + TEXT(" CPU RunSync"), Cpu->RunSync(CpuIn, CpuOut) == UE::NNE::IModelInstanceCPU::ERunSyncStatus::Ok)) return false;
		AddInfo(FString::Printf(TEXT("%s: CPU inference %.0f ms"), *Tag, (FPlatformTime::Seconds() - CpuStart) * 1000.0));

		float RefMax = 0.f;
		bool bRefValid = true;
		for (float V : Ref[0]) { bRefValid &= FMath::IsFinite(V) && V >= 0.f; RefMax = FMath::Max(RefMax, V); }
		TestTrue(Tag + TEXT(" CPU denoised_rgb finite and nonnegative"), bRefValid);
		TestTrue(Tag + TEXT(" CPU denoised_rgb not all zero"), RefMax > 0.f);
		if (Frame > 0)
		{
			double HiddenEnergy = 0.0;
			for (float V : HiddenIn[0]) HiddenEnergy += FMath::Abs(V);
			TestTrue(Tag + TEXT(" receives a non-zero hidden state"), HiddenEnergy > 0.0);
		}

		if (Dml.IsValid())
		{
			TArray<TArray<float>> Out;
			for (uint64 N : OutFloats) Out.Emplace_GetRef().SetNumZeroed(N);
			bool bEnqueueOk = false;
			ENQUEUE_RENDER_COMMAND(SLRayRepairMotionTestRun)([&](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				TArray<UE::NNE::FTensorBindingRDG> InRDG, OutRDG;
				TArray<FRDGBufferRef> OutBuffers;
				// Exact-size buffers, as the denoiser allocates them: a read past a tensor's end page-faults.
				for (const TArray<float>* In : Inputs)
				{
					FRDGBufferRef B = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), In->Num()), TEXT("RayRepairMotionTest.In"));
					GraphBuilder.QueueBufferUpload(B, In->GetData(), In->Num() * sizeof(float), ERDGInitialDataFlags::NoCopy);
					InRDG.Emplace_GetRef().Buffer = B;
				}
				for (const TArray<float>& O : Out)
				{
					FRDGBufferRef B = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), O.Num()), TEXT("RayRepairMotionTest.Out"));
					OutRDG.Emplace_GetRef().Buffer = B;
					OutBuffers.Add(B);
				}
				bEnqueueOk = Dml->EnqueueRDG(GraphBuilder, InRDG, OutRDG) == UE::NNE::EResultStatus::Ok;

				TArray<TUniquePtr<FRHIGPUBufferReadback>> Readbacks;
				for (int32 o = 0; o < OutBuffers.Num(); ++o)
				{
					Readbacks.Add(MakeUnique<FRHIGPUBufferReadback>(TEXT("RayRepairMotionTest.Readback")));
					AddEnqueueCopyPass(GraphBuilder, Readbacks.Last().Get(), OutBuffers[o], Out[o].Num() * sizeof(float));
				}
				GraphBuilder.Execute();
				RHICmdList.SubmitAndBlockUntilGPUIdle();
				for (int32 o = 0; o < Out.Num() && bEnqueueOk; ++o)
				{
					const uint32 NumBytes = Out[o].Num() * sizeof(float);
					if (!Readbacks[o]->IsReady()) { bEnqueueOk = false; break; }
					FMemory::Memcpy(Out[o].GetData(), Readbacks[o]->Lock(NumBytes), NumBytes);
					Readbacks[o]->Unlock();
				}
			});
			FlushRenderingCommands();

			if (TestTrue(Tag + TEXT(" DirectML EnqueueRDG + readback"), bEnqueueOk))
			{
				for (int32 o = 0; o < Out.Num(); ++o)
				{
					double MaxDiff = 0.0, MaxRef = 0.0;
					bool bValid = true;
					for (int32 i = 0; i < Out[o].Num(); ++i)
					{
						bValid &= FMath::IsFinite(Out[o][i]);
						MaxDiff = FMath::Max(MaxDiff, (double)FMath::Abs(Out[o][i] - Ref[o][i]));
						MaxRef = FMath::Max(MaxRef, (double)FMath::Abs(Ref[o][i]));
					}
					const FString Name = OutDescs[o].GetName();
					const double Rel = MaxDiff / FMath::Max(MaxRef, 1e-6);
					AddInfo(FString::Printf(TEXT("%s %s: DirectML vs CPU max abs diff %.3g (%.2e of max %.3g)"), *Tag, *Name, MaxDiff, Rel, MaxRef));
					TestTrue(Tag + TEXT(" DirectML ") + Name + TEXT(" finite"), bValid);
					TestTrue(Tag + TEXT(" DirectML ") + Name + TEXT(" matches CPU (max diff < 1% of range)"), Rel < 1e-2);
				}
			}
		}

		if (Frame > 0)
		{
			double Change = 0.0, Scale = 0.0;
			for (int32 i = 0; i < Ref[0].Num(); ++i) { Change += FMath::Abs(Ref[0][i] - PreviousDenoised[i]); Scale += FMath::Abs(Ref[0][i]); }
			AddInfo(FString::Printf(TEXT("%s: mean |change| from previous frame %.3g (mean output %.3g)"), *Tag, Change / Ref[0].Num(), Scale / Ref[0].Num()));
		}
		PreviousDenoised = MoveTemp(Ref[0]);
		for (int32 h = 0; h < 5; ++h) HiddenIn[h] = MoveTemp(Ref[1 + h]);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// In-game check
// ---------------------------------------------------------------------------------------------

namespace SLRayRepairMotionTest
{
	struct FGameState
	{
		FAutomationTestBase* Test = nullptr;
		TWeakObjectPtr<APlayerController> Player;
		int32 Stage = 0;
		int32 StageFrames = 0;
		uint32 RunAtStart = 0;
		uint32 HistoryAtStart = 0;
		double MeasureStart = 0.0;
		double FrameMs[2] = { 0.0, 0.0 };       // mean wall-clock time per frame, method 0 and method 7
		FString DumpBefore;                     // last dump folder before this test asked for one
		int32 DumpWaitFrames = 0;
		double Started = FPlatformTime::Seconds();
	};

	FSLDenoiserRayRepair* MotionPass()
	{
		return static_cast<FSLDenoiserRayRepair*>(GetDenoiserForMethod(ESLDenoiserMethod::RayRepairMotion));
	}

	void SetMethod(int32 Method)
	{
		IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Denoiser.Method"))->Set(Method, ECVF_SetByConsole);
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FSLRayRepairMotionInGameStep, TSharedPtr<SLRayRepairMotionTest::FGameState>, State);
bool FSLRayRepairMotionInGameStep::Update()
{
	using namespace SLRayRepairMotionTest;
	FAutomationTestBase* Test = State->Test;
	if (FPlatformTime::Seconds() - State->Started > 600.0)
	{
		Test->AddError(TEXT("Timed out"));
		SetMethod(0);
		return true;
	}
	// Turn the camera slowly so motion_px and the history warp are exercised.
	if (APlayerController* PC = State->Player.Get())
	{
		FRotator R = PC->GetControlRotation();
		R.Yaw += 0.15f;
		PC->SetControlRotation(R);
	}

	// Stage 4: wait for the tensor dump requested at the end of stage 3, then check it.
	if (State->Stage == 4)
	{
		const FString Folder = FSLDenoiserRayRepair::GetLastDumpFolder();
		if (Folder.IsEmpty() || Folder == State->DumpBefore)
		{
			if (++State->DumpWaitFrames < 600) return false;
			Test->AddError(TEXT("SL.Denoiser.RayRepair.Dump wrote nothing within 600 frames"));
			SetMethod(0);
			return true;
		}
		static const TCHAR* Files[] = { TEXT("ue_buffers.npy"), TEXT("motion_px.npy"), TEXT("hidden_in0.npy"),
			TEXT("hidden_in4.npy"), TEXT("denoised_rgb.npy"), TEXT("meta.json") };
		for (const TCHAR* File : Files)
		{
			Test->TestTrue(FString::Printf(TEXT("Dump has %s"), File), IFileManager::Get().FileSize(*(Folder / File)) > 0);
		}
		// ue_buffers is [1, 10, 1080, 1920] float32 after a 128-byte .npy header.
		Test->TestEqual(TEXT("ue_buffers.npy size"), IFileManager::Get().FileSize(*(Folder / TEXT("ue_buffers.npy"))),
			int64(128) + int64(10) * W * H * 4);
		FString Meta;
		FFileHelper::LoadFileToString(Meta, *(Folder / TEXT("meta.json")));
		Test->TestTrue(TEXT("meta.json records the pre-exposure"), Meta.Contains(TEXT("\"pre_exposure\": ")) && !Meta.Contains(TEXT("\"pre_exposure\": null")));
		Test->TestTrue(TEXT("meta.json records a continued recurrent state"), Meta.Contains(TEXT("\"state_reset_this_frame\": false")));
		const int32 Denoised = Meta.Find(TEXT("\"denoised_rgb\": {"));
		const FString DenoisedAll = Denoised == INDEX_NONE ? FString() : Meta.Mid(Denoised, Meta.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Meta.Find(TEXT("\"all\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Denoised)) - Denoised + 1);
		Test->AddInfo(TEXT("denoised_rgb ") + DenoisedAll);
		Test->TestTrue(TEXT("denoised_rgb has no NaN or Inf"), DenoisedAll.EndsWith(TEXT("\"nan\": 0, \"inf\": 0}")));
		Test->TestFalse(TEXT("denoised_rgb is not all zero"), DenoisedAll.Contains(TEXT("\"max\": 0.0,")));
		Test->AddInfo(TEXT("Tensor dump: ") + Folder);
		SetMethod(0);
		return true;
	}

	// Stages, each ending after its frame count:
	//   0 settle on method 0   1 measure method 0   2 switch to 7 and settle   3 measure method 7, check, restore 0
	constexpr int32 SettleFrames = 30, MeasureFrames = 90;
	static const int32 StageFrames[] = { SettleFrames, MeasureFrames, SettleFrames, MeasureFrames };
	if (++State->StageFrames < StageFrames[FMath::Min(State->Stage, 3)]) return false;
#if WITH_EDITOR
	// Frames rendered while shaders compile are not representative (the path tracer may show its fallback).
	if (State->Stage == 0 && GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0) return false;
#endif
	State->StageFrames = 0;
	const double Now = FPlatformTime::Seconds();

	switch (State->Stage)
	{
	case 0:
		State->MeasureStart = Now;
		break;
	case 1:
		State->FrameMs[0] = (Now - State->MeasureStart) * 1000.0 / MeasureFrames;
		SetMethod(7);
		break;
	case 2:
		State->RunAtStart = MotionPass()->GetFramesRun();
		State->HistoryAtStart = MotionPass()->GetFramesWithHistory();
		State->MeasureStart = Now;
		break;
	default:
	{
		State->FrameMs[1] = (Now - State->MeasureStart) * 1000.0 / MeasureFrames;
		const uint32 Run = MotionPass()->GetFramesRun() - State->RunAtStart;
		const uint32 WithHistory = MotionPass()->GetFramesWithHistory() - State->HistoryAtStart;
		Test->AddInfo(FString::Printf(TEXT("Method 7 ran on %u frames, %u of them continuing the hidden state, over %d game frames"),
			Run, WithHistory, MeasureFrames));
		Test->TestTrue(TEXT("Method 7 ran the model (view rect must be 1920x1080)"), Run >= uint32(MeasureFrames / 2));
		Test->TestTrue(TEXT("Hidden state carries from frame to frame"), WithHistory + 2 >= Run && Run > 0);
		Test->AddInfo(FString::Printf(TEXT("Wall-clock frame time: method 0 %.1f ms, method 7 %.1f ms (mean of %d frames each)"),
			State->FrameMs[0], State->FrameMs[1], MeasureFrames));
		State->DumpBefore = FSLDenoiserRayRepair::GetLastDumpFolder();
		GEngine->Exec(nullptr, TEXT("SL.Denoiser.RayRepair.Dump"));
		break;
	}
	}
	++State->Stage;
	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRayRepairMotionInGameTest, "SL.Denoiser.RayRepairMotion.InGame",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FSLRayRepairMotionInGameTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; }
	if (!TestNotNull(TEXT("Run in a DX12 game instance"), World)) return false;

	auto State = MakeShared<SLRayRepairMotionTest::FGameState>();
	State->Test = this;
	State->Player = GEngine->GetFirstLocalPlayerController(World);
	SLRayRepairMotionTest::SetMethod(0);
	ADD_LATENT_AUTOMATION_COMMAND(FSLRayRepairMotionInGameStep(State));
	return true;
}

#endif

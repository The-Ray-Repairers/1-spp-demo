#if WITH_DEV_AUTOMATION_TESTS
#include "SLFlythroughSubsystem.h"
#include "SLComparisonView.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLFlythroughRendererTest, "SL.Flythrough.RendererRouting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLFlythroughRendererTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    auto* Capture = NewObject<USLFlythroughSubsystem>(World);
    Capture->RegisterMethods();
    TestEqual(TEXT("Seven methods retained"), Capture->Methods.Num(), 7);
    const auto& SVGF = Capture->Methods[2];
    const auto& DLSS = Capture->Methods[3];
    TestTrue(TEXT("SVGF uses the guide backend"), SVGF.Backend == ESLGMRenderBackend::SVGF);
    TestTrue(TEXT("DLSS uses the Lumen backend"), DLSS.Backend == ESLGMRenderBackend::LumenDLSSRR);
    TestTrue(TEXT("Integrated methods have no implementation blocker"), SVGF.UnavailableReason.IsEmpty() && DLSS.UnavailableReason.IsEmpty());
    TestEqual(TEXT("SVGF warm-up"), SVGF.WarmUpFrames, 32);
    TestEqual(TEXT("DLSS warm-up"), DLSS.WarmUpFrames, 32);
    TestFalse(TEXT("DLSS must not wait for path tracer accumulation"), DLSS.bAccumulate);
    const auto& OIDN = Capture->Methods[6];
    TestTrue(TEXT("OIDN uses the engine denoiser backend"), OIDN.Backend == ESLGMRenderBackend::OpenImageDenoise);
    TestTrue(TEXT("OIDN has no implementation blocker"), OIDN.UnavailableReason.IsEmpty());
    TestEqual(TEXT("Spatial OIDN needs no temporal settling"), OIDN.WarmUpFrames, 1);
    TestEqual(TEXT("Ours routes to RayRepair"), Capture->Methods[4].DenoiserMethod, 5);
    TestEqual(TEXT("KPN/Direct routes to NB Direct"), Capture->Methods[5].DenoiserMethod, 6);
    FText Reason;
    TestFalse(TEXT("Unknown method remains blocked"), Capture->IsMethodAvailable(INDEX_NONE, Reason));
    TestFalse(TEXT("Unavailable reason supplied"), Reason.IsEmpty());
    Capture->SelectMethod(INDEX_NONE);
    TestEqual(TEXT("Blocked selection leaves current method alone"), Capture->SelectedMethod, 0);
    Capture->AccumulationView = FSceneViewExtensions::NewExtension<FSLComparisonView>(World);
    Capture->AccumulationView->RenderedFrames.Store(99);
    Capture->AccumulationView->PathTracingSeed = 99;
    Capture->ResetFrameHistory();
    TestEqual(TEXT("Resume/new pose restarts warm-up"), Capture->AccumulationView->RenderedFrames.Load(), uint32(0));
    TestTrue(TEXT("Resume/new pose requests camera cut"), Capture->AccumulationView->bResetHistory.Load());
    TestTrue(TEXT("Path tracing also resets"), Capture->AccumulationView->bResetAccumulation.Load());
    TestEqual(TEXT("SVGF sample sequence restarts with each pose"), Capture->AccumulationView->PathTracingSeed, 0);
    Capture->AccumulationView.Reset();
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLFlythroughRendererManifestTest, "SL.Flythrough.RendererManifest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLFlythroughRendererManifestTest::RunTest(const FString& Parameters)
{
    FSLFlythroughRunManifest GT;
    GT.Method = TEXT("512SPP_GT"); GT.SPP = 512; GT.DenoiserMethod = 0;
    GT.Map = TEXT("/Game/Test"); GT.CameraPath = TEXT("recorded:test");
    GT.CameraParameters = TEXT("same_camera"); GT.RenderSettings = TEXT("capture_protocol=2");
    GT.MethodSettings = TEXT("backend=0;warmup=1");
    GT.Width = 16; GT.Height = 16; GT.FrameCount = 1; GT.TargetFPS = 30;
    GT.DurationSeconds = 1.0 / 30.0;
    auto DLSS = GT;
    DLSS.Method = TEXT("DLSS"); DLSS.SPP = 1;
    DLSS.MethodSettings = TEXT("backend=2;warmup=32;dlss_rr=1");
    TestTrue(TEXT("Different backend may be scored against GT"), DLSS.IsComparableTo(GT));
    TestFalse(TEXT("Different backend may not resume a GT run"), DLSS.IsSameCaptureAs(GT));
    auto Changed = DLSS;
    Changed.MethodSettings += TEXT(";quality=changed");
    TestFalse(TEXT("Backend tuning change invalidates resume"), DLSS.IsSameCaptureAs(Changed));
    Changed = DLSS;
    Changed.RenderSettings = TEXT("capture_protocol=old");
    TestFalse(TEXT("Old capture protocol requires a new GT"), DLSS.IsComparableTo(Changed));
    const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Folder, true);
    TestTrue(TEXT("Write renderer identity"), FSLFlythroughMetrics::WriteRunManifest(Folder, DLSS));
    FSLFlythroughRunManifest Loaded;
    TestTrue(TEXT("Read renderer identity"), FSLFlythroughMetrics::ReadRunManifest(Folder, Loaded));
    TestTrue(TEXT("Backend identity survives round trip"), DLSS.IsSameCaptureAs(Loaded));
    IFileManager::Get().DeleteDirectory(*Folder, false, true);
    return true;
}
#endif

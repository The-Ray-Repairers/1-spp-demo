#include "SLGMToolConfig.h"
#include "SLRenderingMethods.h"

const FSLGMToolEntry* USLGMToolConfig::FindProfile(FName Id) const
{
    if (Id.IsNone()) return nullptr;
    return Entries.FindByPredicate([Id](const FSLGMToolEntry& E) {
        return E.Id == Id && E.Control == ESLGMControlType::RenderingAction
            && E.Action != ESLComparisonAction::Restore && E.Action != ESLComparisonAction::OpenResults;
    });
}

void USLGMToolConfig::PostLoad()
{
    Super::PostLoad();
    if (RenderingProfileVersion != 0) { StandardizeRenderingMethods(); return; }
    // One-time compatibility migration. Runtime selection never interprets these
    // legacy mode names; newly authored presets use ApplyProfile and Profile.
    for (int32 Index = 0; Index < Entries.Num(); ++Index)
    {
        auto& Entry = Entries[Index];
        if (Entry.Control != ESLGMControlType::RenderingAction) continue;
        if (Entry.Id.IsNone()) Entry.Id = FName(*FString::Printf(TEXT("RenderingProfile_%d"), Index));
        switch (Entry.Action)
        {
        case ESLComparisonAction::Raw:
            BaselineProfileId = DefaultPreviewProfileId = Entry.Id;
            Entry.Profile.ComparisonSource = ESLGMComparisonSource::SharedInput;
            Entry.Profile.LabelOptionVariable = TEXT("SL.Denoiser.Method");
            break;
        case ESLComparisonAction::SVGF:
            Entry.Profile.Backend = ESLGMRenderBackend::SVGF;
            Entry.Profile.ComparisonSource = ESLGMComparisonSource::SharedInput;
            break;
        case ESLComparisonAction::RR:
            Entry.Profile.Backend = ESLGMRenderBackend::LumenDLSSRR;
            break;
        case ESLComparisonAction::Reference:
            Entry.Profile.bAccumulate = Entry.Profile.bSaveImage = true;
            Entry.Profile.SamplesVariable = ReferenceSamplesVariable;
            Entry.Profile.PreviewLabel = NSLOCTEXT("SLComparison", "ReferencePreview", "Reference ({Samples} SPP)");
            break;
        default: break;
        }
    }
    for (auto& Entry : Entries)
        if (Entry.Control == ESLGMControlType::RadioGroup && Entry.ConsoleVariable == TEXT("SL.Denoiser.Method"))
            Entry.PreviewProfileId = BaselineProfileId;
    RenderingProfileVersion = 1;
    StandardizeRenderingMethods();
}

#if WITH_EDITOR
void USLGMToolConfig::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    RenderingProfileVersion = 1;
    Super::PostEditChangeProperty(Event);
}
#endif

void USLGMToolConfig::StandardizeRenderingMethods()
{
    TArray<FSLGMToolEntry> Profiles;
    for (const auto& Method : SLRenderingMethods::Get())
    {
        const auto* Existing = Entries.FindByPredicate([&](const FSLGMToolEntry& E) {
            return E.Control == ESLGMControlType::RenderingAction
                && (E.Id == FName(Method.Id) || (E.Action == Method.Action && Method.Action != ESLComparisonAction::ApplyProfile));
        });
        // OIDN uses the same 1-sample path tracing setup as the raw profile.
        if (SLRenderingMethods::GetBackend(Method) == ESLGMRenderBackend::OpenImageDenoise
            && (!Existing || Existing->Profile.Backend != ESLGMRenderBackend::OpenImageDenoise))
            Existing = Entries.FindByPredicate([](const FSLGMToolEntry& E) {
                return E.Control == ESLGMControlType::RenderingAction && E.Action == ESLComparisonAction::Raw;
            });
        FSLGMToolEntry Entry = Existing ? *Existing : FSLGMToolEntry();
        Entry.Id = FName(Method.Id);
        Entry.Group = FText::FromString(TEXT("Rendering Comparisons"));
        Entry.Label = FText::FromString(Method.Label);
        Entry.Control = ESLGMControlType::RenderingAction;
        Entry.Action = Method.Action;
        Entry.Profile.PreviewLabel = Entry.Label;
        Entry.Profile.LabelOptionVariable.Empty();
        Entry.Profile.SamplesVariable.Empty();
        Entry.Profile.SamplesPerPixel = Method.Action == ESLComparisonAction::Reference ? 512 : 1;
        Entry.Profile.bAccumulate = Entry.Profile.bSaveImage = Method.Action == ESLComparisonAction::Reference;
        Entry.Profile.Backend = SLRenderingMethods::GetBackend(Method);
        Entry.Profile.ComparisonSource = (Entry.Profile.bAccumulate || Method.Action == ESLComparisonAction::RR
            || Entry.Profile.Backend == ESLGMRenderBackend::OpenImageDenoise)
            ? ESLGMComparisonSource::IndependentRaw : ESLGMComparisonSource::SharedInput;
        Entry.RenderSettings.RemoveAll([](const FSLGMRenderSetting& S) { return S.Name == TEXT("SL.Denoiser.Method"); });
        FSLGMRenderSetting Denoiser;
        Denoiser.Name = TEXT("SL.Denoiser.Method");
        Denoiser.Value = TEXT("0");
        Entry.RenderSettings.Add(Denoiser);
        Profiles.Add(MoveTemp(Entry));
    }
    const int32 FirstProfile = Entries.IndexOfByPredicate([](const FSLGMToolEntry& E) { return E.Control == ESLGMControlType::RenderingAction; });
    Entries.RemoveAll([](const FSLGMToolEntry& E) {
        return E.Control == ESLGMControlType::RenderingAction && E.Action != ESLComparisonAction::Restore && E.Action != ESLComparisonAction::OpenResults;
    });
    Entries.Insert(Profiles, FMath::Clamp(FirstProfile, 0, Entries.Num()));
    BaselineProfileId = DefaultPreviewProfileId = FName(SLRenderingMethods::Get()[0].Id);
    // This legacy CVar control is presented as a profile selector by the GM panel.
    // Keep its internal link valid for callers of SelectPreviewMethod.
    for (auto& Entry : Entries)
        if (Entry.Control == ESLGMControlType::RadioGroup && Entry.ConsoleVariable == TEXT("SL.Denoiser.Method"))
            Entry.PreviewProfileId = BaselineProfileId;
}

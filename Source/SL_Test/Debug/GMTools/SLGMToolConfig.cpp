#include "SLGMToolConfig.h"

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
    if (RenderingProfileVersion != 0) return;
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
}

#if WITH_EDITOR
void USLGMToolConfig::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    RenderingProfileVersion = 1;
    Super::PostEditChangeProperty(Event);
}
#endif

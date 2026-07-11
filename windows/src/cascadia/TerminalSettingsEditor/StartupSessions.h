// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// StartupSessions — axan D19. The global "Startup sessions" page: a row list over the global
// GlobalAppSettings.StartupSessions tree. Each row picks a WT profile (which shell) and edits its
// directory/command/name/icon/color; rows reorder, nest (indent/outdent), duplicate, and import
// from a portable .toml. Replaces the per-profile Startup page (M14/D17).

#pragma once

#include "StartupSessions.g.h"
#include "StartupSessionsViewModel.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct StartupSessions : public HasScrollViewer<StartupSessions>, StartupSessionsT<StartupSessions>
    {
    public:
        StartupSessions();

        void OnNavigatedTo(const Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        void AddEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void DeleteEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void DuplicateEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void IndentEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void OutdentEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void MoveEntryUp_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void MoveEntryDown_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        safe_void_coroutine Import_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        // axan #14: the "Save current as startup" confirm-flyout's Replace button.
        void CaptureConfirm_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

        // The per-row profile ComboBox holds Model::Profile objects but the row stores a GUID
        // string; these reconcile the two (select-on-load, write-guid-on-change).
        void ProfileCombo_Loaded(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void ProfileCombo_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);

        // axan #438: per-row icon picker flyout (builtin glyphs + browse + free text). Content
        // is rebuilt on each Opening so it always reflects the row it's anchored to.
        void IconFlyout_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);

        // axan #13: per-row session color picker flyout (the shared palette swatches +
        // the icon/text/both apply-to choice). Rebuilt on each Opening, like the icon flyout.
        void ColorFlyout_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);

        til::property_changed_event PropertyChanged;
        WINRT_PROPERTY(Editor::StartupSessionsViewModel, ViewModel, nullptr);

    private:
        Editor::LaunchEntryViewModel _entryFromSender(const Windows::Foundation::IInspectable& sender);
        safe_void_coroutine _PickIconImage(Editor::LaunchEntryViewModel vm, winrt::weak_ref<Windows::UI::Xaml::Controls::Flyout> weakFlyout);

        // True while ProfileCombo_Loaded programmatically selects the row's current
        // profile; SelectionChanged skips the commit then (#435 item 1 — without this,
        // visiting the page pins follow-the-default entries to the current default's
        // GUID). Set/cleared synchronously around the SelectedItem assignment, so one
        // flag serves every row's combo.
        bool _profileComboLoading{ false };
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(StartupSessions);
}

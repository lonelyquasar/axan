// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// StartupSessions — axan D19 / #38. The global "Startup sessions" page over the global
// GlobalAppSettings.StartupSessions tree: a list of the rows (structure only) with a
// toolbar + keyboard accelerators acting on the selected row, and a detail panel editing
// that row (Identity via the shared Axan::Ui::IdentityEditor, Launch, or the separator
// fields). Replaces the per-row column grid and, before it, the per-profile Startup page.

#pragma once

#include "StartupSessions.g.h"
#include "StartupSessionsViewModel.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

#include <memory>

namespace Axan::Ui::IdentityEditor
{
    class Editor;
}

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct StartupSessions : public HasScrollViewer<StartupSessions>, StartupSessionsT<StartupSessions>
    {
    public:
        StartupSessions();

        void OnNavigatedTo(const Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        // Page commands.
        void AddEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        // axan #3: the "Add separator" button — appends a divider row.
        void AddSeparator_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        safe_void_coroutine Import_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        // axan #14: the "Save current as startup" confirm-flyout's Replace button.
        void CaptureConfirm_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

        // axan #38: the selection toolbar; each acts on ViewModel.SelectedEntry.
        void DeleteEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void DuplicateEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void IndentEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void OutdentEntry_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void MoveEntryUp_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void MoveEntryDown_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

        // axan #38: the same ops from the keyboard, scoped to the list (ScopeOwner).
        void MoveUpAccelerator_Invoked(const Windows::UI::Xaml::Input::KeyboardAccelerator& sender, const Windows::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs& args);
        void MoveDownAccelerator_Invoked(const Windows::UI::Xaml::Input::KeyboardAccelerator& sender, const Windows::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs& args);
        void OutdentAccelerator_Invoked(const Windows::UI::Xaml::Input::KeyboardAccelerator& sender, const Windows::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs& args);
        void IndentAccelerator_Invoked(const Windows::UI::Xaml::Input::KeyboardAccelerator& sender, const Windows::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs& args);
        void DuplicateAccelerator_Invoked(const Windows::UI::Xaml::Input::KeyboardAccelerator& sender, const Windows::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs& args);
        void DeleteAccelerator_Invoked(const Windows::UI::Xaml::Input::KeyboardAccelerator& sender, const Windows::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs& args);

        // axan #38: list <-> ViewModel.SelectedEntry.
        void EntriesList_Loaded(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void EntriesList_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        // axan #38: built-in drag reorder, bracketed by the view-model's BeginDrag /
        // NormalizeAfterDrag.
        void EntriesList_DragItemsStarting(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::DragItemsStartingEventArgs& args);
        void EntriesList_DragItemsCompleted(const Windows::UI::Xaml::Controls::ListViewBase& sender, const Windows::UI::Xaml::Controls::DragItemsCompletedEventArgs& args);

        // Detail panel handlers.
        void ProfileCombo_SelectionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);
        safe_void_coroutine BrowseDirectory_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void IconTextBox_TextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::TextChangedEventArgs& args);

        // axan #38: row-template formatting (see the IDL).
        static hstring RowLabel(const hstring& name, bool isSeparator, const hstring& separatorStyle, double height);
        static double RowLabelOpacity(const hstring& name);
        static Windows::UI::Text::FontWeight RowLabelWeight(const hstring& name, bool isSeparator);
        static hstring SeparatorChevron(const hstring& name, bool collapsed);
        static Windows::UI::Xaml::Visibility SeparatorRuleVisibility(const hstring& separatorStyle);
        static hstring RowAccessibleName(const hstring& name, bool isSeparator, int32_t depth);
        static Windows::UI::Xaml::Automation::Peers::AutomationHeadingLevel RowHeadingLevel(bool isSeparator);

        til::property_changed_event PropertyChanged;
        WINRT_PROPERTY(Editor::StartupSessionsViewModel, ViewModel, nullptr);

    private:
        // Runs a structural op with list-selection echo suppressed (the op's in-place
        // RemoveAt/InsertAt clears the ListView's selection, which must not be pushed back
        // into the view-model as "nothing selected"), then re-syncs the list. Returns
        // whatever the op returns.
        template<typename F>
        auto _runOp(F&& op);
        // The selected row, or null.
        Editor::LaunchEntryViewModel _selected() const;

        void _onViewModelPropertyChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Data::PropertyChangedEventArgs& args);
        // ViewModel.SelectedEntry -> the ListView's selection (+ scroll it into view).
        void _syncListSelection();
        // After a keyboard op, put focus back on the selected row's container (the op's
        // RemoveAt/InsertAt recycles the focused container).
        void _focusSelectedRow();
        // Rebuild the detail panel's code-built and code-synced parts for the selected
        // row: the identity editor, the icon text box, the profile combo.
        void _rebuildDetail();
        void _syncProfileCombo();
        void _syncIconText();
        safe_void_coroutine _PickIconImage(Editor::LaunchEntryViewModel vm);

        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _vmChangedRevoker;

        // True while a structural op runs or the page itself sets the ListView's selection;
        // EntriesList_SelectionChanged ignores the list's selection changes then.
        bool _ignoreListSelection{ false };
        // The row being dragged, between DragItemsStarting and DragItemsCompleted (null
        // otherwise). _ignoreListSelection is held for the whole drag: the reorder's
        // RemoveAt/InsertAt clears the list's selection, which must not blank the detail
        // panel.
        Editor::LaunchEntryViewModel _dragHead{ nullptr };
        // The row the detail panel was last built for; the identity editor is rebuilt only
        // when the selected row is a different one.
        Editor::LaunchEntryViewModel _detailFor{ nullptr };
        // The shared identity editor for _detailFor. The page owns it: the editor's XAML
        // handlers hold only a weak_ptr, so dropping this (rebuild / page teardown) leaves
        // the old controls inert.
        std::shared_ptr<Axan::Ui::IdentityEditor::Editor> _identity;
        // True while _syncIconText writes the icon text box, so its TextChanged isn't
        // taken as the user typing an icon.
        bool _iconTextSyncing{ false };

        // True while _syncProfileCombo programmatically selects the row's current profile;
        // SelectionChanged skips the commit then (#435 item 1 — without this, selecting a
        // follow-the-default entry would pin it to the current default's GUID).
        bool _profileComboLoading{ false };
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(StartupSessions);
}

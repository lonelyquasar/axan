// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

#include "StartupSessionsViewModel.g.h"
#include "ProfileViewModel.h" // LaunchEntryViewModel
#include "ViewModelHelpers.h"
#include "Utils.h"

#include <AxanEntryTreeOps.h> // axan #38: shared tree rules (src/inc)

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct StartupSessionsViewModel : StartupSessionsViewModelT<StartupSessionsViewModel>, ViewModelHelper<StartupSessionsViewModel>
    {
    public:
        StartupSessionsViewModel(const Model::CascadiaSettings& settings);

        Windows::Foundation::Collections::IObservableVector<Model::Profile> AvailableProfiles() const;
        hstring DefaultProfileGuid() const;

        void AddEntry();
        void AddSeparator();
        void DeleteEntry(const Editor::LaunchEntryViewModel& vm);
        void DuplicateEntry(const Editor::LaunchEntryViewModel& vm);
        void IndentEntry(const Editor::LaunchEntryViewModel& vm);
        void OutdentEntry(const Editor::LaunchEntryViewModel& vm);
        void MoveEntryUp(const Editor::LaunchEntryViewModel& vm);
        void MoveEntryDown(const Editor::LaunchEntryViewModel& vm);
        bool ImportFromToml(hstring path);

        // axan #38: selection model (see the IDL).
        Editor::LaunchEntryViewModel SelectedEntry() const { return _selectedEntry; }
        void SelectedEntry(const Editor::LaunchEntryViewModel& value);
        bool HasSelection() const { return _selectedEntry != nullptr; }
        bool CanMoveUp() const;
        bool CanMoveDown() const;
        bool CanIndent() const;
        bool CanOutdent() const;

        uint32_t CaptureLiveStartup();
        // axan #14: impl-only — MainPage attaches TerminalApp's live-tree snapshot
        // provider at construction (via get_self, same component; not projected).
        void LiveEntriesProvider(const Editor::LiveStartupEntriesProvider& provider) { _liveEntriesProvider = provider; }

        // The startup tree rows. Set wholesale on (re)build; the macro getter feeds the ItemsControl.
        VIEW_MODEL_OBSERVABLE_PROPERTY(Windows::Foundation::Collections::IObservableVector<Editor::LaunchEntryViewModel>, Entries, nullptr);

    private:
        Model::CascadiaSettings _settings{ nullptr };
        // Guards _Commit while a structural edit mutates several rows at once.
        bool _suspendCommit{ false };
        // axan #14: see LiveEntriesProvider above.
        Editor::LiveStartupEntriesProvider _liveEntriesProvider{ nullptr };

        void _rebuildFromSettings();
        // axan #3: one row from a model entry, carrying every field incl. the separator
        // ones — shared by the settings rebuild and the live capture (#14).
        // axan #19: the ONLY LaunchEntryViewModel construction site; _modelFromRow is its
        // inverse (used by _commit and DuplicateEntry). Together they own the field list.
        static Editor::LaunchEntryViewModel _rowFromModel(const Model::LaunchEntry& e);
        static Model::LaunchEntry _modelFromRow(const Editor::LaunchEntryViewModel& vm);
        void _hookEntry(const Editor::LaunchEntryViewModel& vm);
        void _recomputeDepths();
        void _commit();
        void _moveEntry(const Editor::LaunchEntryViewModel& vm, bool up);

        // axan #38: the row the detail panel edits (see SelectedEntry).
        Editor::LaunchEntryViewModel _selectedEntry{ nullptr };
        // Set the selection and raise SelectedEntry plus every derived flag
        // unconditionally: structural ops call this even when the row is unchanged.
        void _select(const Editor::LaunchEntryViewModel& vm);
        void _notifySelectionState();
        // The rows as AxanEntryTreeOps nodes, and a row's index (none when absent).
        Axan::EntryTree::Nodes _nodes() const;
        std::optional<uint32_t> _indexOf(const Editor::LaunchEntryViewModel& vm) const;
        void _applyBlockMove(const Axan::EntryTree::BlockMove& move);
        hstring _resolveImportedProfile(const std::string& bareGuid, const std::string& name);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(StartupSessionsViewModel);
}

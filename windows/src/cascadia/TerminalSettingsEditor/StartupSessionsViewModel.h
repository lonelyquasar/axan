// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

#include "StartupSessionsViewModel.g.h"
#include "ProfileViewModel.h" // LaunchEntryViewModel
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct StartupSessionsViewModel : StartupSessionsViewModelT<StartupSessionsViewModel>, ViewModelHelper<StartupSessionsViewModel>
    {
    public:
        StartupSessionsViewModel(const Model::CascadiaSettings& settings);

        Windows::Foundation::Collections::IObservableVector<Model::Profile> AvailableProfiles() const;
        hstring DefaultProfileGuid() const;

        void AddEntry();
        void DeleteEntry(const Editor::LaunchEntryViewModel& vm);
        void DuplicateEntry(const Editor::LaunchEntryViewModel& vm);
        void IndentEntry(const Editor::LaunchEntryViewModel& vm);
        void OutdentEntry(const Editor::LaunchEntryViewModel& vm);
        bool ImportFromToml(hstring path);

        // The startup tree rows. Set wholesale on (re)build; the macro getter feeds the ItemsControl.
        VIEW_MODEL_OBSERVABLE_PROPERTY(Windows::Foundation::Collections::IObservableVector<Editor::LaunchEntryViewModel>, Entries, nullptr);

    private:
        Model::CascadiaSettings _settings{ nullptr };
        // Guards _Commit while a structural edit mutates several rows at once.
        bool _suspendCommit{ false };

        void _rebuildFromSettings();
        void _hookEntry(const Editor::LaunchEntryViewModel& vm);
        void _recomputeDepths();
        void _commit();
        hstring _resolveImportedProfile(const std::string& bareGuid, const std::string& name);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(StartupSessionsViewModel);
}

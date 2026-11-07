// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// axan #38: Settings > Sessions > Sidebar (see SidebarViewModel.idl). The members moved
// here verbatim from GlobalAppearanceViewModel.

#pragma once

#include "SidebarViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct SidebarViewModel : SidebarViewModelT<SidebarViewModel>, ViewModelHelper<SidebarViewModel>
    {
    public:
        SidebarViewModel(Model::GlobalAppSettings globalSettings);

        // DON'T ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper.
        using ViewModelHelper<SidebarViewModel>::PropertyChanged;

        GETSET_BINDABLE_ENUM_SETTING(SidebarStatusAfterSeen, Model::SidebarStatusAfterSeen, _GlobalSettings.SidebarStatusAfterSeen);

    public:
        // sidebar.stateGlyphs (see the idl)
        winrt::hstring SidebarStateGlyphText(const winrt::hstring& key) const;
        void SetSidebarStateGlyph(const winrt::hstring& key, const winrt::hstring& nativeIcon);
        void ResetSidebarStateGlyph(const winrt::hstring& key);
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarIdleGlyphPreview() const { return _StateGlyphPreview(L"idle"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarWorkingGlyphPreview() const { return _StateGlyphPreview(L"working"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarBlockedGlyphPreview() const { return _StateGlyphPreview(L"blocked"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarDoneGlyphPreview() const { return _StateGlyphPreview(L"done"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarErrorGlyphPreview() const { return _StateGlyphPreview(L"error"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarRunningGlyphPreview() const { return _StateGlyphPreview(L"running"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarPermissionGlyphPreview() const { return _StateGlyphPreview(L"permission"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarQuestionGlyphPreview() const { return _StateGlyphPreview(L"question"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarAuthGlyphPreview() const { return _StateGlyphPreview(L"auth"); }
        winrt::hstring SidebarIdleGlyphLabel() const { return _StateGlyphLabel(L"idle"); }
        winrt::hstring SidebarWorkingGlyphLabel() const { return _StateGlyphLabel(L"working"); }
        winrt::hstring SidebarBlockedGlyphLabel() const { return _StateGlyphLabel(L"blocked"); }
        winrt::hstring SidebarDoneGlyphLabel() const { return _StateGlyphLabel(L"done"); }
        winrt::hstring SidebarErrorGlyphLabel() const { return _StateGlyphLabel(L"error"); }
        winrt::hstring SidebarRunningGlyphLabel() const { return _StateGlyphLabel(L"running"); }
        winrt::hstring SidebarPermissionGlyphLabel() const { return _StateGlyphLabel(L"permission"); }
        winrt::hstring SidebarQuestionGlyphLabel() const { return _StateGlyphLabel(L"question"); }
        winrt::hstring SidebarAuthGlyphLabel() const { return _StateGlyphLabel(L"auth"); }

        // axan #34: sidebar.hoistStates (see the idl)
        bool SidebarHoistStateEnabled(const winrt::hstring& name) const;
        void SetSidebarHoistStateEnabled(const winrt::hstring& name, bool enabled);
        void ResetSidebarHoistStates();
        winrt::hstring SidebarHoistStatesSummary() const;

        // axan: sidebar.idleEffects (see the idl)
        bool SidebarIdleEffectEnabled(const winrt::hstring& name) const;
        void SetSidebarIdleEffectEnabled(const winrt::hstring& name, bool enabled);
        void ResetSidebarIdleEffects();
        winrt::hstring SidebarIdleEffectsSummary() const;
        // The localized checkbox label / help for an effect name ("" for an unknown one).
        static winrt::hstring IdleEffectLabel(std::wstring_view name);
        static winrt::hstring IdleEffectHelp(std::wstring_view name);

        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarMatchesProfileTransparency);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarIdleAfterMinutes); // axan #31
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarNeedsYouGroup); // axan #34
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarStateLabels);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarShowAgentName);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarLabelFontSize);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarStateLineFontSize);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarIconSize);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarStateGlyphSize);

    private:
        Model::GlobalAppSettings _GlobalSettings;

        // sidebar.stateGlyphs helpers. nullopt = the key is absent (default glyph).
        std::optional<std::wstring> _StateGlyphConfigured(std::wstring_view key) const;
        winrt::Windows::UI::Xaml::Controls::IconElement _StateGlyphPreview(std::wstring_view key) const;
        winrt::hstring _StateGlyphLabel(std::wstring_view key) const;
        void _StoreStateGlyphs(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring>& map);
    };
};

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(SidebarViewModel);
}

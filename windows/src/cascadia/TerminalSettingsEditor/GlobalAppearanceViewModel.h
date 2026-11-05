// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GlobalAppearanceViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct GlobalAppearanceViewModel : GlobalAppearanceViewModelT<GlobalAppearanceViewModel>, ViewModelHelper<GlobalAppearanceViewModel>
    {
    public:
        GlobalAppearanceViewModel(Model::GlobalAppSettings globalSettings);

        // DON'T YOU DARE ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper. Do this instead:
        using ViewModelHelper<GlobalAppearanceViewModel>::PropertyChanged;

        WINRT_PROPERTY(Windows::Foundation::Collections::IObservableVector<Model::Theme>, ThemeList, nullptr);
        GETSET_BINDABLE_ENUM_SETTING(NewTabPosition, Model::NewTabPosition, _GlobalSettings.NewTabPosition);
        GETSET_BINDABLE_ENUM_SETTING(TabWidthMode, winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode, _GlobalSettings.TabWidthMode);
        GETSET_BINDABLE_ENUM_SETTING(SidebarStatusAfterSeen, Model::SidebarStatusAfterSeen, _GlobalSettings.SidebarStatusAfterSeen); // axan

    public:
        winrt::Windows::Foundation::IInspectable CurrentTheme();
        void CurrentTheme(const winrt::Windows::Foundation::IInspectable& tag);
        static winrt::hstring ThemeNameConverter(const Model::Theme& theme);

        // axan: sidebar.stateGlyphs (see the idl)
        winrt::hstring SidebarStateGlyphText(const winrt::hstring& key) const;
        void SetSidebarStateGlyph(const winrt::hstring& key, const winrt::hstring& nativeIcon);
        void ResetSidebarStateGlyph(const winrt::hstring& key);
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarIdleGlyphPreview() const { return _StateGlyphPreview(L"idle"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarWorkingGlyphPreview() const { return _StateGlyphPreview(L"working"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarBlockedGlyphPreview() const { return _StateGlyphPreview(L"blocked"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarDoneGlyphPreview() const { return _StateGlyphPreview(L"done"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarErrorGlyphPreview() const { return _StateGlyphPreview(L"error"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarRunningGlyphPreview() const { return _StateGlyphPreview(L"running"); }
        winrt::hstring SidebarIdleGlyphLabel() const { return _StateGlyphLabel(L"idle"); }
        winrt::hstring SidebarWorkingGlyphLabel() const { return _StateGlyphLabel(L"working"); }
        winrt::hstring SidebarBlockedGlyphLabel() const { return _StateGlyphLabel(L"blocked"); }
        winrt::hstring SidebarDoneGlyphLabel() const { return _StateGlyphLabel(L"done"); }
        winrt::hstring SidebarErrorGlyphLabel() const { return _StateGlyphLabel(L"error"); }
        winrt::hstring SidebarRunningGlyphLabel() const { return _StateGlyphLabel(L"running"); }

        bool InvertedDisableAnimations();
        void InvertedDisableAnimations(bool value);

        void ShowTitlebarToggled(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& args);

        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, AlwaysShowTabs);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, ShowTabsFullscreen);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, ShowTabsInTitlebar);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, UseAcrylicInTabRow);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarMatchesProfileTransparency);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarIdleAfterMinutes); // axan #31
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, ShowTitleInTitlebar);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, AlwaysOnTop);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, AutoHideWindow);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, AlwaysShowNotificationIcon);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, MinimizeToNotificationArea);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, ShowAdminShield);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, EnableUnfocusedAcrylic);

    private:
        Model::GlobalAppSettings _GlobalSettings;
        winrt::Windows::Foundation::IInspectable _currentTheme;

        void _UpdateThemeList();
        // axan: sidebar.stateGlyphs helpers. nullopt = the key is absent (default glyph).
        std::optional<std::wstring> _StateGlyphConfigured(std::wstring_view key) const;
        winrt::Windows::UI::Xaml::Controls::IconElement _StateGlyphPreview(std::wstring_view key) const;
        winrt::hstring _StateGlyphLabel(std::wstring_view key) const;
        void _StoreStateGlyphs(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring>& map);
    };
};

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(GlobalAppearanceViewModel);
}

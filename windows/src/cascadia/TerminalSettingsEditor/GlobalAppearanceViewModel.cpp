// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GlobalAppearanceViewModel.h"
#include "GlobalAppearanceViewModel.g.cpp"
#include "EnumEntry.h"

#include <LibraryResources.h>
#include <AxanStateGlyphs.h> // axan: sidebar.stateGlyphs keys / defaults / resolution

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    constexpr std::wstring_view systemThemeName{ L"system" };
    constexpr std::wstring_view darkThemeName{ L"dark" };
    constexpr std::wstring_view lightThemeName{ L"light" };
    constexpr std::wstring_view legacySystemThemeName{ L"legacySystem" };
    constexpr std::wstring_view legacyDarkThemeName{ L"legacyDark" };
    constexpr std::wstring_view legacyLightThemeName{ L"legacyLight" };

    GlobalAppearanceViewModel::GlobalAppearanceViewModel(Model::GlobalAppSettings globalSettings) :
        _GlobalSettings{ globalSettings },
        _ThemeList{ single_threaded_observable_vector<Model::Theme>() }
    {
        INITIALIZE_BINDABLE_ENUM_SETTING(NewTabPosition, NewTabPosition, NewTabPosition, L"Globals_NewTabPosition", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(TabWidthMode, TabViewWidthMode, winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode, L"Globals_TabWidthMode", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(SidebarStatusAfterSeen, SidebarStatusAfterSeen, SidebarStatusAfterSeen, L"Globals_SidebarStatusAfterSeen", L"Content"); // axan
        _UpdateThemeList();
    }

    // ---- axan: sidebar.stateGlyphs ----
    // The setting is a string map; only the keys the user changed are present. The editor
    // writes the portable form (a builtin glyph as builtin:NAME) so settings.json stays
    // readable; the app resolves either form (AxanStateGlyphs.h).

    std::optional<std::wstring> GlobalAppearanceViewModel::_StateGlyphConfigured(std::wstring_view key) const
    {
        const auto map = _GlobalSettings.SidebarStateGlyphs();
        const winrt::hstring k{ key };
        if (map && map.HasKey(k))
        {
            return std::wstring{ map.Lookup(k) };
        }
        return std::nullopt;
    }

    winrt::hstring GlobalAppearanceViewModel::SidebarStateGlyphText(const winrt::hstring& key) const
    {
        const auto configured = _StateGlyphConfigured(key);
        if (!configured)
        {
            return {};
        }
        bool mapped = false;
        return winrt::hstring{ Axan::IconRegistry::NativeToPortable(*configured, mapped) };
    }

    // The resolved icon at 16px; "show nothing" previews as a faint Cancel glyph so the button
    // isn't blank.
    winrt::Windows::UI::Xaml::Controls::IconElement GlobalAppearanceViewModel::_StateGlyphPreview(std::wstring_view key) const
    {
        const auto native = Axan::StateGlyphs::Resolve(_StateGlyphConfigured(key), Axan::StateGlyphs::DefaultFor(key));
        const bool none = native.empty();
        const auto icon = Microsoft::Terminal::UI::IconPathConverter::IconWUX(none ? winrt::hstring{ L"\uE711" } : winrt::hstring{ native });
        if (!icon)
        {
            return nullptr;
        }
        Controls::FontIconSource font{ nullptr };
        if (const auto ise = icon.try_as<Controls::IconSourceElement>())
        {
            font = ise.IconSource().try_as<Controls::FontIconSource>();
        }
        if (font)
        {
            font.FontSize(16);
        }
        else
        {
            icon.Width(16);
            icon.Height(16);
        }
        if (none)
        {
            icon.Opacity(0.4);
        }
        return icon;
    }

    // What the row says beside its button: Default / None / the configured value (an image
    // path as its file name).
    winrt::hstring GlobalAppearanceViewModel::_StateGlyphLabel(std::wstring_view key) const
    {
        const auto configured = _StateGlyphConfigured(key);
        if (!configured)
        {
            return Axan::StateGlyphs::DefaultFor(key).empty() ? RS_(L"Globals_SidebarStateGlyphDefaultNone") : RS_(L"Globals_SidebarStateGlyphDefault");
        }
        const auto text = SidebarStateGlyphText(winrt::hstring{ key });
        if (Axan::StateGlyphs::Resolve(configured, {}).empty())
        {
            return RS_(L"Globals_SidebarStateGlyphNone");
        }
        const std::wstring_view t{ text };
        if (const auto slash = t.find_last_of(L"\\/"); slash != std::wstring_view::npos && slash + 1 < t.size())
        {
            return winrt::hstring{ t.substr(slash + 1) };
        }
        return text;
    }

    void GlobalAppearanceViewModel::_StoreStateGlyphs(const IMap<winrt::hstring, winrt::hstring>& map)
    {
        if (map.Size() == 0)
        {
            _GlobalSettings.ClearSidebarStateGlyphs();
        }
        else
        {
            _GlobalSettings.SidebarStateGlyphs(map);
        }
        _NotifyChanges(L"SidebarIdleGlyphPreview", L"SidebarIdleGlyphLabel", L"SidebarWorkingGlyphPreview", L"SidebarWorkingGlyphLabel", L"SidebarBlockedGlyphPreview", L"SidebarBlockedGlyphLabel", L"SidebarDoneGlyphPreview", L"SidebarDoneGlyphLabel", L"SidebarErrorGlyphPreview", L"SidebarErrorGlyphLabel", L"SidebarRunningGlyphPreview", L"SidebarRunningGlyphLabel");
    }

    // Always a fresh map: the model's may be shared with (or inherited from) another settings
    // object, so it is never mutated in place.
    static IMap<winrt::hstring, winrt::hstring> _copyStateGlyphs(const IMap<winrt::hstring, winrt::hstring>& source)
    {
        auto copy = single_threaded_map<winrt::hstring, winrt::hstring>();
        if (source)
        {
            for (const auto& kv : source)
            {
                copy.Insert(kv.Key(), kv.Value());
            }
        }
        return copy;
    }

    void GlobalAppearanceViewModel::SetSidebarStateGlyph(const winrt::hstring& key, const winrt::hstring& nativeIcon)
    {
        auto map = _copyStateGlyphs(_GlobalSettings.SidebarStateGlyphs());
        bool mapped = false;
        map.Insert(key, winrt::hstring{ Axan::IconRegistry::NativeToPortable(nativeIcon, mapped) });
        _StoreStateGlyphs(map);
    }

    void GlobalAppearanceViewModel::ResetSidebarStateGlyph(const winrt::hstring& key)
    {
        auto map = _copyStateGlyphs(_GlobalSettings.SidebarStateGlyphs());
        if (map.HasKey(key))
        {
            map.Remove(key);
        }
        _StoreStateGlyphs(map);
    }

    // Function Description:
    // - Updates the list of all themes available to choose from.
    void GlobalAppearanceViewModel::_UpdateThemeList()
    {
        // Surprisingly, though this is called every time we navigate to the page,
        // the list does not keep growing on each navigation.
        const auto& ThemeMap{ _GlobalSettings.Themes() };
        for (const auto& pair : ThemeMap)
        {
            _ThemeList.Append(pair.Value());
        }
    }

    winrt::Windows::Foundation::IInspectable GlobalAppearanceViewModel::CurrentTheme()
    {
        return _GlobalSettings.CurrentTheme();
    }

    // Get the name out of the newly selected item, stash that as the Theme name
    // set for the globals. That controls which theme is actually the current
    // theme.
    void GlobalAppearanceViewModel::CurrentTheme(const winrt::Windows::Foundation::IInspectable& tag)
    {
        if (const auto& theme{ tag.try_as<Model::Theme>() })
        {
            _GlobalSettings.Theme(Model::ThemePair{ theme.Name() });
        }
    }

    // Method Description:
    // - Convert the names of the inbox themes to some more descriptive,
    //   well-known values. If the passed in theme isn't an inbox one, then just
    //   return its set Name.
    //    - "light" becomes "Light"
    //    - "dark" becomes "Dark"
    //    - "system" becomes "Use Windows theme"
    // - These values are all localized based on the app language.
    // Arguments:
    // - theme: the theme to get the display name for.
    // Return Value:
    // - the potentially localized name to use for this Theme.
    winrt::hstring GlobalAppearanceViewModel::ThemeNameConverter(const Model::Theme& theme)
    {
        if (theme.Name() == darkThemeName)
        {
            return RS_(L"Globals_ThemeDark/Content");
        }
        else if (theme.Name() == lightThemeName)
        {
            return RS_(L"Globals_ThemeLight/Content");
        }
        else if (theme.Name() == systemThemeName)
        {
            return RS_(L"Globals_ThemeSystem/Content");
        }
        else if (theme.Name() == legacyDarkThemeName)
        {
            return RS_(L"Globals_ThemeDarkLegacy/Content");
        }
        else if (theme.Name() == legacyLightThemeName)
        {
            return RS_(L"Globals_ThemeLightLegacy/Content");
        }
        else if (theme.Name() == legacySystemThemeName)
        {
            return RS_(L"Globals_ThemeSystemLegacy/Content");
        }
        return theme.Name();
    }

    bool GlobalAppearanceViewModel::InvertedDisableAnimations()
    {
        return !_GlobalSettings.DisableAnimations();
    }

    void GlobalAppearanceViewModel::InvertedDisableAnimations(bool value)
    {
        _GlobalSettings.DisableAnimations(!value);
    }

    void GlobalAppearanceViewModel::ShowTitlebarToggled(const winrt::Windows::Foundation::IInspectable& /* sender */, const RoutedEventArgs& /* args */)
    {
        // Set AlwaysShowTabs to true if ShowTabsInTitlebar was toggled OFF -> ON
        if (!ShowTabsInTitlebar())
        {
            AlwaysShowTabs(true);
        }
    }
}

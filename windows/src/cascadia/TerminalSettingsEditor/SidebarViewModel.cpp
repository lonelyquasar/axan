// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// axan #38: Settings > Sessions > Sidebar. Moved verbatim from GlobalAppearanceViewModel.

#include "pch.h"
#include "SidebarViewModel.h"
#include "SidebarViewModel.g.cpp"
#include "EnumEntry.h"

#include <LibraryResources.h>
#include <AxanStateGlyphs.h> // sidebar.stateGlyphs keys / defaults / resolution
#include <AxanHoistRule.h> // axan #34: sidebar.hoistStates

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    SidebarViewModel::SidebarViewModel(Model::GlobalAppSettings globalSettings) :
        _GlobalSettings{ globalSettings }
    {
        INITIALIZE_BINDABLE_ENUM_SETTING(SidebarStatusAfterSeen, SidebarStatusAfterSeen, SidebarStatusAfterSeen, L"Globals_SidebarStatusAfterSeen", L"Content");
    }

    // ---- axan #34: sidebar.hoistStates ----
    // Stored as an array of names; unset means the default list. The page writes the names
    // in vocabulary order; unknown names a hand edit added are dropped on the first change.

    static Axan::HoistRule::Mask _hoistMask(const Model::GlobalAppSettings& globals)
    {
        if (!globals.HasSidebarHoistStates() || !globals.SidebarHoistStates())
        {
            return Axan::HoistRule::kDefault;
        }
        return Axan::HoistRule::MaskFromNames(globals.SidebarHoistStates());
    }

    bool SidebarViewModel::SidebarHoistStateEnabled(const winrt::hstring& name) const
    {
        return Axan::HoistRule::Has(_hoistMask(_GlobalSettings), name);
    }

    void SidebarViewModel::SetSidebarHoistStateEnabled(const winrt::hstring& name, bool enabled)
    {
        const auto before = _hoistMask(_GlobalSettings);
        const auto after = Axan::HoistRule::With(before, name, enabled);
        if (after == before && _GlobalSettings.HasSidebarHoistStates())
        {
            return;
        }
        std::vector<winrt::hstring> names;
        for (const auto n : Axan::HoistRule::NamesFromMask(after))
        {
            names.emplace_back(n);
        }
        _GlobalSettings.SidebarHoistStates(winrt::single_threaded_vector<winrt::hstring>(std::move(names)));
        _NotifyChanges(L"SidebarHoistStatesSummary");
    }

    void SidebarViewModel::ResetSidebarHoistStates()
    {
        _GlobalSettings.ClearSidebarHoistStates();
        _NotifyChanges(L"SidebarHoistStatesSummary");
    }

    winrt::hstring SidebarViewModel::SidebarHoistStatesSummary() const
    {
        return winrt::hstring{ Axan::HoistRule::Describe(_hoistMask(_GlobalSettings)) };
    }

    // ---- axan: sidebar.stateGlyphs ----
    // The setting is a string map; only the keys the user changed are present. The editor
    // writes the portable form (a builtin glyph as builtin:NAME) so settings.json stays
    // readable; the app resolves either form (AxanStateGlyphs.h).

    std::optional<std::wstring> SidebarViewModel::_StateGlyphConfigured(std::wstring_view key) const
    {
        const auto map = _GlobalSettings.SidebarStateGlyphs();
        const winrt::hstring k{ key };
        if (map && map.HasKey(k))
        {
            return std::wstring{ map.Lookup(k) };
        }
        return std::nullopt;
    }

    winrt::hstring SidebarViewModel::SidebarStateGlyphText(const winrt::hstring& key) const
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
    winrt::Windows::UI::Xaml::Controls::IconElement SidebarViewModel::_StateGlyphPreview(std::wstring_view key) const
    {
        // A kind key (permission / question / auth) falls back to blocked, as in the app.
        const auto lookup = [this](std::wstring_view k) { return _StateGlyphConfigured(k); };
        const auto native = Axan::StateGlyphs::IsKindKey(key) ? Axan::StateGlyphs::GlyphFor(L"blocked", key, lookup) : Axan::StateGlyphs::GlyphFor(key, lookup);
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
    winrt::hstring SidebarViewModel::_StateGlyphLabel(std::wstring_view key) const
    {
        const auto configured = _StateGlyphConfigured(key);
        if (!configured)
        {
            if (Axan::StateGlyphs::IsKindKey(key))
            {
                return RS_(L"Globals_SidebarStateGlyphDefaultBlocked");
            }
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

    void SidebarViewModel::_StoreStateGlyphs(const IMap<winrt::hstring, winrt::hstring>& map)
    {
        if (map.Size() == 0)
        {
            _GlobalSettings.ClearSidebarStateGlyphs();
        }
        else
        {
            _GlobalSettings.SidebarStateGlyphs(map);
        }
        _NotifyChanges(L"SidebarIdleGlyphPreview", L"SidebarIdleGlyphLabel", L"SidebarWorkingGlyphPreview", L"SidebarWorkingGlyphLabel", L"SidebarBlockedGlyphPreview", L"SidebarBlockedGlyphLabel", L"SidebarDoneGlyphPreview", L"SidebarDoneGlyphLabel", L"SidebarErrorGlyphPreview", L"SidebarErrorGlyphLabel", L"SidebarRunningGlyphPreview", L"SidebarRunningGlyphLabel", L"SidebarPermissionGlyphPreview", L"SidebarPermissionGlyphLabel", L"SidebarQuestionGlyphPreview", L"SidebarQuestionGlyphLabel", L"SidebarAuthGlyphPreview", L"SidebarAuthGlyphLabel");
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

    void SidebarViewModel::SetSidebarStateGlyph(const winrt::hstring& key, const winrt::hstring& nativeIcon)
    {
        auto map = _copyStateGlyphs(_GlobalSettings.SidebarStateGlyphs());
        bool mapped = false;
        map.Insert(key, winrt::hstring{ Axan::IconRegistry::NativeToPortable(nativeIcon, mapped) });
        _StoreStateGlyphs(map);
    }

    void SidebarViewModel::ResetSidebarStateGlyph(const winrt::hstring& key)
    {
        auto map = _copyStateGlyphs(_GlobalSettings.SidebarStateGlyphs());
        if (map.HasKey(key))
        {
            map.Remove(key);
        }
        _StoreStateGlyphs(map);
    }
}

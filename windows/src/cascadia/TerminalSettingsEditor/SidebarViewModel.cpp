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
#include <AxanIdleEffects.h> // axan: sidebar.idleEffects
#include <AxanGlyphScale.h> // axan: sidebar.iconFontScale defaults, icon -> font family

#include <optional>
#include <set>

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

    // ---- axan: sidebar.idleEffects ----
    // Same storage shape as sidebar.hoistStates: an array of names, unset = the default.

    static Axan::IdleEffects::Mask _idleEffectsMask(const Model::GlobalAppSettings& globals)
    {
        if (!globals.HasSidebarIdleEffects() || !globals.SidebarIdleEffects())
        {
            return Axan::IdleEffects::kDefault;
        }
        return Axan::IdleEffects::MaskFromNames(globals.SidebarIdleEffects());
    }

    winrt::hstring SidebarViewModel::IdleEffectLabel(std::wstring_view name)
    {
        if (name == L"dim")
        {
            return RS_(L"Globals_SidebarIdleEffectDim");
        }
        if (name == L"smaller")
        {
            return RS_(L"Globals_SidebarIdleEffectSmaller");
        }
        if (name == L"gray")
        {
            return RS_(L"Globals_SidebarIdleEffectGray");
        }
        if (name == L"collapse")
        {
            return RS_(L"Globals_SidebarIdleEffectCollapse");
        }
        return {};
    }

    winrt::hstring SidebarViewModel::IdleEffectHelp(std::wstring_view name)
    {
        if (name == L"dim")
        {
            return RS_(L"Globals_SidebarIdleEffectDimHelp");
        }
        if (name == L"smaller")
        {
            return RS_(L"Globals_SidebarIdleEffectSmallerHelp");
        }
        if (name == L"gray")
        {
            return RS_(L"Globals_SidebarIdleEffectGrayHelp");
        }
        if (name == L"collapse")
        {
            return RS_(L"Globals_SidebarIdleEffectCollapseHelp");
        }
        return {};
    }

    bool SidebarViewModel::SidebarIdleEffectEnabled(const winrt::hstring& name) const
    {
        const auto bit = Axan::IdleEffects::Bit(name);
        return bit != 0 && (_idleEffectsMask(_GlobalSettings) & bit) != 0;
    }

    void SidebarViewModel::SetSidebarIdleEffectEnabled(const winrt::hstring& name, bool enabled)
    {
        const auto bit = Axan::IdleEffects::Bit(name);
        if (bit == 0)
        {
            return;
        }
        const auto before = _idleEffectsMask(_GlobalSettings);
        const auto after = static_cast<Axan::IdleEffects::Mask>(enabled ? (before | bit) : (before & ~bit));
        if (after == before && _GlobalSettings.HasSidebarIdleEffects())
        {
            return;
        }
        std::vector<winrt::hstring> names;
        for (const auto n : Axan::IdleEffects::NamesFromMask(after))
        {
            names.emplace_back(n);
        }
        _GlobalSettings.SidebarIdleEffects(winrt::single_threaded_vector<winrt::hstring>(std::move(names)));
        _NotifyChanges(L"SidebarIdleEffectsSummary");
    }

    void SidebarViewModel::ResetSidebarIdleEffects()
    {
        _GlobalSettings.ClearSidebarIdleEffects();
        _NotifyChanges(L"SidebarIdleEffectsSummary");
    }

    winrt::hstring SidebarViewModel::SidebarIdleEffectsSummary() const
    {
        std::wstring out;
        for (const auto n : Axan::IdleEffects::NamesFromMask(_idleEffectsMask(_GlobalSettings)))
        {
            if (!out.empty())
            {
                out += L", ";
            }
            out += IdleEffectLabel(n);
        }
        return out.empty() ? RS_(L"Globals_SidebarIdleEffectsNone") : winrt::hstring{ out };
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
    template<typename V>
    static IMap<winrt::hstring, V> _copyMap(const IMap<winrt::hstring, V>& source)
    {
        auto copy = single_threaded_map<winrt::hstring, V>();
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
        auto map = _copyMap(_GlobalSettings.SidebarStateGlyphs());
        bool mapped = false;
        map.Insert(key, winrt::hstring{ Axan::IconRegistry::NativeToPortable(nativeIcon, mapped) });
        _StoreStateGlyphs(map);
    }

    // ---- axan: sidebar.iconFontScale ----
    //
    // Family names compare case-insensitively everywhere (AxanGlyphScale::SameFamily), as the
    // icon converter's lookup does: "segoe ui historic" is the Segoe UI Historic row, not a
    // second one. A value this page writes is stored under the row's spelling (the kFonts
    // spelling for a built-in family) and replaces any other spelling of the same family.

    // The key the map stores for a family in some spelling, if any.
    static std::optional<winrt::hstring> _findFamilyKey(const IMap<winrt::hstring, double>& map, std::wstring_view family)
    {
        if (map)
        {
            for (const auto& kv : map)
            {
                if (Axan::GlyphScale::SameFamily(kv.Key(), family))
                {
                    return kv.Key();
                }
            }
        }
        return std::nullopt;
    }

    // Drops every spelling of a family from the map.
    static void _removeFamily(const IMap<winrt::hstring, double>& map, std::wstring_view family)
    {
        while (const auto key = _findFamilyKey(map, family))
        {
            map.Remove(*key);
        }
    }

    IVectorView<winrt::hstring> SidebarViewModel::SidebarIconFontFamilies() const
    {
        std::vector<winrt::hstring> builtIn;
        for (const auto& f : Axan::GlyphScale::kFonts)
        {
            builtIn.emplace_back(f.font);
        }
        std::sort(builtIn.begin(), builtIn.end());
        // Case-insensitive, so one family written two ways is one row (the first spelling
        // seen; a built-in family always shows its kFonts spelling).
        std::set<std::wstring, Axan::GlyphScale::FamilyLess> others;
        const auto addIcon = [&](const winrt::hstring& icon) {
            if (auto family = Axan::GlyphScale::FamilyForIcon(icon); !family.empty())
            {
                others.emplace(std::move(family));
            }
        };
        if (const auto map = _GlobalSettings.SidebarIconFontScale())
        {
            for (const auto& kv : map)
            {
                if (!kv.Key().empty())
                {
                    others.emplace(kv.Key().c_str());
                }
            }
        }
        if (const auto entries = _GlobalSettings.StartupSessions())
        {
            for (const auto& entry : entries)
            {
                addIcon(entry.Icon());
            }
        }
        for (const auto& icon : _extraIconSources)
        {
            addIcon(icon);
        }
        auto families = single_threaded_vector<winrt::hstring>(std::move(builtIn));
        for (const auto& family : others)
        {
            if (Axan::GlyphScale::KnownFamily(family).empty())
            {
                families.Append(winrt::hstring{ family });
            }
        }
        return families.GetView();
    }

    double SidebarViewModel::SidebarIconFontScaleDefault(const winrt::hstring& family) const
    {
        return Axan::GlyphScale::FontScaleFor(family);
    }

    bool SidebarViewModel::IsSidebarIconFontScaleSet(const winrt::hstring& family) const
    {
        return _findFamilyKey(_GlobalSettings.SidebarIconFontScale(), family).has_value();
    }

    double SidebarViewModel::SidebarIconFontScale(const winrt::hstring& family) const
    {
        const auto map = _GlobalSettings.SidebarIconFontScale();
        return Axan::GlyphScale::FontScaleFor(family, [&](std::wstring_view) -> std::optional<double> {
            if (const auto key = _findFamilyKey(map, family))
            {
                return map.Lookup(*key);
            }
            return std::nullopt;
        });
    }

    void SidebarViewModel::_StoreIconFontScales(const IMap<winrt::hstring, double>& map)
    {
        if (map.Size() == 0)
        {
            _GlobalSettings.ClearSidebarIconFontScale();
        }
        else
        {
            _GlobalSettings.SidebarIconFontScale(map);
        }
    }

    void SidebarViewModel::SetSidebarIconFontScale(const winrt::hstring& family, double value)
    {
        if (family.empty() || !(value == value))
        {
            return;
        }
        auto map = _copyMap(_GlobalSettings.SidebarIconFontScale());
        const auto clamped = Axan::GlyphScale::ClampScale(value);
        _removeFamily(map, family);
        // The built-in value is stored as "no override", so the map only lists real changes.
        if (std::abs(clamped - Axan::GlyphScale::FontScaleFor(family)) >= 1e-9)
        {
            map.Insert(winrt::hstring{ Axan::GlyphScale::CanonicalFamily(family) }, clamped);
        }
        _StoreIconFontScales(map);
    }

    void SidebarViewModel::ResetSidebarIconFontScale(const winrt::hstring& family)
    {
        auto map = _copyMap(_GlobalSettings.SidebarIconFontScale());
        _removeFamily(map, family);
        _StoreIconFontScales(map);
    }

    void SidebarViewModel::ResetSidebarStateGlyph(const winrt::hstring& key)
    {
        auto map = _copyMap(_GlobalSettings.SidebarStateGlyphs());
        if (map.HasKey(key))
        {
            map.Remove(key);
        }
        _StoreStateGlyphs(map);
    }
}

// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "LaunchEntry.h"
#include "LaunchEntry.g.cpp"

#include <AxanLaunchEntryWire.h> // the shared wire-format key definitions (#431 item 6)
#include <AxanHoistRule.h> // axan #34

using namespace Microsoft::Terminal::Settings::Model;

// JSON keys for one entry object inside the global "startupSessions" array (D19).
// Single-sourced from AxanLaunchEntryWire.h so the TOML exporter (which reads
// settings.json raw) can never drift from this (de)serializer. "parent" (not
// "parentId") is the historical wire spelling; the projected property is ParentId.
static constexpr std::string_view IdKey{ Axan::LaunchEntryWire::JsonKey::Id };
static constexpr std::string_view ParentKey{ Axan::LaunchEntryWire::JsonKey::Parent };
// axan D19: the referenced WT profile GUID (which shell this entry spawns).
static constexpr std::string_view ProfileKey{ Axan::LaunchEntryWire::JsonKey::Profile };
static constexpr std::string_view NameKey{ Axan::LaunchEntryWire::JsonKey::Name };
static constexpr std::string_view DirectoryKey{ Axan::LaunchEntryWire::JsonKey::Directory };
static constexpr std::string_view CommandKey{ Axan::LaunchEntryWire::JsonKey::Command };
static constexpr std::string_view IconKey{ Axan::LaunchEntryWire::JsonKey::Icon };
// axan #422: per-node recolor token + target. "colorTarget" follows WT's camelCase compound-key
// convention (cf. "startingDirectory"); the value, not the key, is what stays portable with Linux.
static constexpr std::string_view ColorKey{ Axan::LaunchEntryWire::JsonKey::Color };
static constexpr std::string_view ColorTargetKey{ Axan::LaunchEntryWire::JsonKey::ColorTarget };
// axan #3: separator rows — kind discriminator + the separator-only fields.
static constexpr std::string_view KindKey{ Axan::LaunchEntryWire::JsonKey::Kind };
static constexpr std::string_view StyleKey{ Axan::LaunchEntryWire::JsonKey::Style };
static constexpr std::string_view HeightKey{ Axan::LaunchEntryWire::JsonKey::Height };
static constexpr std::string_view PlacementKey{ Axan::LaunchEntryWire::JsonKey::Placement };
static constexpr std::string_view CollapsedKey{ Axan::LaunchEntryWire::JsonKey::Collapsed }; // axan #33
static constexpr std::string_view HoistKey{ Axan::LaunchEntryWire::JsonKey::Hoist }; // axan #34

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    winrt::com_ptr<LaunchEntry> LaunchEntry::FromJson(const Json::Value& json)
    {
        auto result = winrt::make_self<LaunchEntry>();
        JsonUtils::GetValueForKey(json, IdKey, result->_Id);
        JsonUtils::GetValueForKey(json, ParentKey, result->_ParentId);
        JsonUtils::GetValueForKey(json, ProfileKey, result->_Profile);
        JsonUtils::GetValueForKey(json, NameKey, result->_Name);
        JsonUtils::GetValueForKey(json, DirectoryKey, result->_Directory);
        JsonUtils::GetValueForKey(json, CommandKey, result->_Command);
        JsonUtils::GetValueForKey(json, IconKey, result->_Icon);
        JsonUtils::GetValueForKey(json, ColorKey, result->_Color);
        JsonUtils::GetValueForKey(json, ColorTargetKey, result->_ColorTarget);
        // axan #3: separator rows. All sparse on disk; a session row carries none of them.
        JsonUtils::GetValueForKey(json, KindKey, result->_Kind);
        JsonUtils::GetValueForKey(json, StyleKey, result->_SeparatorStyle);
        JsonUtils::GetValueForKey(json, HeightKey, result->_Height);
        JsonUtils::GetValueForKey(json, PlacementKey, result->_Placement);
        JsonUtils::GetValueForKey(json, CollapsedKey, result->_Collapsed); // axan #33
        // axan #34: "hoist" is "never" (or "default") or an array of state names; stored in
        // the canonical string form (unknown names dropped, see AxanHoistRule.h).
        if (const auto& hoist = json[JsonKey(HoistKey)]; hoist.isString())
        {
            result->_Hoist = winrt::hstring{ Axan::HoistRule::Normalize(winrt::to_hstring(hoist.asString())) };
        }
        else if (hoist.isArray())
        {
            std::vector<std::wstring> names;
            for (const auto& v : hoist)
            {
                if (v.isString())
                {
                    names.emplace_back(winrt::to_hstring(v.asString()));
                }
            }
            result->_Hoist = winrt::hstring{ Axan::HoistRule::Format(Axan::HoistRule::FromMask(Axan::HoistRule::MaskFromNames(names))) };
        }
        if (result->IsSeparator())
        {
            result->_Height = Axan::LaunchEntryWire::NormalizeSeparatorHeight(result->_Height);
        }
        return result;
    }

    bool LaunchEntry::IsSeparator() const noexcept
    {
        return _Kind == Axan::LaunchEntryWire::KindSeparatorW;
    }

    Json::Value LaunchEntry::ToJson() const
    {
        Json::Value json{ Json::objectValue };
        JsonUtils::SetValueForKey(json, IdKey, _Id);
        JsonUtils::SetValueForKey(json, ParentKey, _ParentId);
        // axan #3: a separator writes only its own keys (see the block at the end) — the
        // session recipe keys would all be null noise on it.
        if (IsSeparator())
        {
            JsonUtils::SetValueForKey(json, KindKey, _Kind);
            if (!_SeparatorStyle.empty() && _SeparatorStyle != Axan::LaunchEntryWire::StyleLineW)
            {
                JsonUtils::SetValueForKey(json, StyleKey, _SeparatorStyle);
            }
            const auto height = Axan::LaunchEntryWire::NormalizeSeparatorHeight(_Height);
            if (height != 1.0)
            {
                JsonUtils::SetValueForKey(json, HeightKey, height);
            }
            if (!_Placement.empty() && _Placement != Axan::LaunchEntryWire::PlacementInlineW)
            {
                JsonUtils::SetValueForKey(json, PlacementKey, _Placement);
            }
            // axan #33: the header label (in "name") and collapsed state, both sparse.
            if (!_Name.empty())
            {
                JsonUtils::SetValueForKey(json, NameKey, _Name);
            }
            if (_Collapsed)
            {
                JsonUtils::SetValueForKey(json, CollapsedKey, _Collapsed);
            }
            return json;
        }
        JsonUtils::SetValueForKey(json, ProfileKey, _Profile);
        JsonUtils::SetValueForKey(json, NameKey, _Name);
        JsonUtils::SetValueForKey(json, DirectoryKey, _Directory);
        JsonUtils::SetValueForKey(json, CommandKey, _Command);
        JsonUtils::SetValueForKey(json, IconKey, _Icon);
        // Sparse, matching the Linux writer (profile-toml.cc): omit color when empty, and omit
        // colorTarget when empty or the "both" default — keeps a no-recolor entry clean on disk.
        if (!_Color.empty())
        {
            JsonUtils::SetValueForKey(json, ColorKey, _Color);
        }
        if (!_ColorTarget.empty() && _ColorTarget != L"both")
        {
            JsonUtils::SetValueForKey(json, ColorTargetKey, _ColorTarget);
        }
        // axan #34: sparse; "never" as a string, a custom list as an array of names.
        if (const auto hoist = Axan::HoistRule::Parse(_Hoist); hoist.mode == Axan::HoistRule::Mode::Never)
        {
            json[JsonKey(HoistKey)] = std::string{ "never" };
        }
        else if (hoist.mode == Axan::HoistRule::Mode::Custom)
        {
            Json::Value names{ Json::arrayValue };
            for (const auto n : Axan::HoistRule::NamesFromMask(hoist.mask))
            {
                names.append(winrt::to_string(n));
            }
            json[JsonKey(HoistKey)] = names;
        }
        // axan #3: a session row never writes kind/style/height/placement, so its on-disk
        // shape is byte-identical to before separators existed.
        return json;
    }

    // Deep copy used when the settings tree is duplicated (GlobalAppSettings::Copy),
    // so an edited entry in one settings clone never bleeds into another.
    Model::LaunchEntry LaunchEntry::Copy() const
    {
        auto entry = winrt::make_self<LaunchEntry>();
        entry->_Id = _Id;
        entry->_ParentId = _ParentId;
        entry->_Profile = _Profile;
        entry->_Name = _Name;
        entry->_Directory = _Directory;
        entry->_Command = _Command;
        entry->_Icon = _Icon;
        entry->_Color = _Color;
        entry->_ColorTarget = _ColorTarget;
        entry->_Kind = _Kind;
        entry->_SeparatorStyle = _SeparatorStyle;
        entry->_Height = _Height;
        entry->_Placement = _Placement;
        entry->_Collapsed = _Collapsed;
        entry->_Hoist = _Hoist;
        return *entry;
    }
}

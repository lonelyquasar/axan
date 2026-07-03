// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// LaunchEntry — axan M14 / D19. One curated startup session, an element of the global
// GlobalAppSettings.StartupSessions tree (D19). Each entry references a WT profile (which
// shell) via Profile and carries its own recipe (directory, command, name, icon, color).
// The Windows superset of the Linux default-launch-entries tuple. Self-(de)serializing via
// FromJson/ToJson (keys single-sourced in src/inc/AxanLaunchEntryWire.h), plumbed through
// the IVector<LaunchEntry> setting by the ConversionTrait below (mirrors NewTabMenuEntry).

#pragma once

#include "LaunchEntry.g.h"
#include "JsonUtils.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct LaunchEntry : LaunchEntryT<LaunchEntry>
    {
    public:
        LaunchEntry() = default;

        static com_ptr<LaunchEntry> FromJson(const Json::Value& json);
        Json::Value ToJson() const;
        Model::LaunchEntry Copy() const;

        WINRT_PROPERTY(hstring, Id);
        WINRT_PROPERTY(hstring, ParentId);
        // axan D19: the GUID of the WT profile this entry spawns (which shell — cmd / pwsh /
        // a WSL distro / ...). "" -> the global default profile. Locally a stable UUID; the
        // portable TOML also records the profile name so it re-resolves across machines.
        WINRT_PROPERTY(hstring, Profile);
        WINRT_PROPERTY(hstring, Name);
        WINRT_PROPERTY(hstring, Directory);
        WINRT_PROPERTY(hstring, Command);
        WINRT_PROPERTY(hstring, Icon);
        // axan #422: per-node recolor token (palette name like "blue", or literal "#RRGGBB";
        // "" -> theme foreground) and its target ("icon"/"text"/"both"; "" -> "both"). Mirror
        // the Linux launch-entries color/color_target fields so the value round-trips per theme.
        WINRT_PROPERTY(hstring, Color);
        WINRT_PROPERTY(hstring, ColorTarget);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(LaunchEntry);
}

namespace Microsoft::Terminal::Settings::Model::JsonUtils
{
    using namespace winrt::Microsoft::Terminal::Settings::Model;

    template<>
    struct ConversionTrait<LaunchEntry>
    {
        LaunchEntry FromJson(const Json::Value& json)
        {
            return *implementation::LaunchEntry::FromJson(json);
        }

        bool CanConvert(const Json::Value& json) const
        {
            return json.isObject();
        }

        Json::Value ToJson(const LaunchEntry& val)
        {
            return winrt::get_self<implementation::LaunchEntry>(val)->ToJson();
        }

        std::string TypeDescription() const
        {
            return "LaunchEntry";
        }
    };
}

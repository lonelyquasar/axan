// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanLaunchEntryWire — the single source of truth for the LaunchEntry wire formats
// (D18/D19, issue #431 item 6).
//
// A LaunchEntry crosses two serialization boundaries:
//   * settings.json (native, camelCase keys) — read/written by the Settings Model
//     (LaunchEntry::FromJson/ToJson) and read raw by the TOML exporter
//     (TerminalApp/AxanProfileToml.cpp, which works on the JSON file directly).
//   * the portable TOML interchange (canonical kebab keys; spec: root README, Configuration) —
//     written by the exporter and read by the Startup-sessions editor importer
//     (TerminalSettingsEditor/StartupSessionsViewModel.cpp).
// Before this header existed the key spellings, icon mapping, and sparse color rules were
// re-typed in three places, which is exactly how the exporter/importer `profile`-key
// asymmetry (#431 headline) happened. Header-only in the shared src/inc (the
// AxanIconRegistry.h precedent) so TerminalApp, the Settings Model, and the Settings
// Editor all compile against the one definition without a cross-lib symbol.
//
// ---------------------------------------------------------------------------------------
// The portable startup-sessions TOML schema (D19 retarget, "Phase 3" of #425/#426)
// ---------------------------------------------------------------------------------------
// Post-D19 the curated tree is GLOBAL (GlobalAppSettings.StartupSessions, settings.json
// key "startupSessions"), not per-profile, and each entry references the WT profile it
// spawns. The portable file is therefore its own top-level array-of-tables — kebab-cased
// like every key axan designs (D18 "congeal the portable core to one case") and named
// after the native global key:
//
//   [meta]
//   axan-toml-version = 1            # interchange schema version; newer is rejected
//   exported-by       = "windows"
//
//   [[startup-sessions]]
//   id           = "..."             # the entry's own minted UUID (stable; bare form)
//   parent-id    = ""                # parent entry's id; "" = root; forward-reference only
//   profile      = "..."             # referenced WT profile GUID, bare (no braces, like
//                                    #   [profile].uuid); OMITTED = follow the default profile
//   profile-name = "PowerShell"      # the referenced profile's display name — the D19
//                                    #   cross-machine fallback; import resolves the GUID
//                                    #   first, then re-matches by this name; omitted with
//                                    #   `profile`
//   name         = "..."             # label template; "" = OSC-title fallback chain
//   directory    = "..."             # working dir; "~" = home; "" = launch dir
//   command      = "..."             # raw command; "" = the profile's shell; wrap-at-spawn
//   icon         = "builtin:..."     # portable icon vocabulary (AxanIconRegistry); native
//                                    #   (non-portable) icons carry verbatim
//   color        = "blue"            # recolor token; SPARSE — omitted when empty
//   color-target = "icon"            # "icon"|"text"|"both"; SPARSE — omitted when ""/both
//
// Why not the old `[[profile.default-launch-entries]]` form: that was the per-profile
// D17/D18 shape; D19 deleted the per-profile setting, and pre-public policy is no compat
// shims, so the importer reads only this schema. The Linux build is unaffected — it keeps
// its per-profile `[[profile.default-launch-entries]]` (D19 "Linux: unaffected"), and a
// Linux import of this file fails cleanly on its missing [profile] table.

#pragma once

#include <string>
#include <string_view>

#include <AxanIconRegistry.h>

namespace Axan::LaunchEntryWire
{
    // The interchange schema version this build writes and accepts
    // (root README, Configuration). Files declaring a newer version are rejected.
    inline constexpr int kTomlVersion = 1;

    // ---- settings.json keys (native WT camelCase; LaunchEntry::FromJson/ToJson and the
    // raw-JSON exporter agree through these). "parent" (not "parentId") is the historical
    // wire spelling; the projected property is ParentId. ----
    namespace JsonKey
    {
        inline constexpr std::string_view Id{ "id" };
        inline constexpr std::string_view Parent{ "parent" };
        inline constexpr std::string_view Profile{ "profile" };
        inline constexpr std::string_view Name{ "name" };
        inline constexpr std::string_view Directory{ "directory" };
        inline constexpr std::string_view Command{ "command" };
        inline constexpr std::string_view Icon{ "icon" };
        inline constexpr std::string_view Color{ "color" };
        inline constexpr std::string_view ColorTarget{ "colorTarget" };
        // The global tree's key on the settings.json root object (GlobalAppSettings).
        inline constexpr std::string_view StartupSessions{ "startupSessions" };
    }

    // ---- portable TOML keys (canonical kebab; spec: root README, Configuration). ----
    namespace TomlKey
    {
        inline constexpr std::string_view Table{ "startup-sessions" };
        inline constexpr std::string_view Id{ "id" };
        inline constexpr std::string_view ParentId{ "parent-id" };
        inline constexpr std::string_view Profile{ "profile" };
        inline constexpr std::string_view ProfileName{ "profile-name" };
        inline constexpr std::string_view Name{ "name" };
        inline constexpr std::string_view Directory{ "directory" };
        inline constexpr std::string_view Command{ "command" };
        inline constexpr std::string_view Icon{ "icon" };
        inline constexpr std::string_view Color{ "color" };
        inline constexpr std::string_view ColorTarget{ "color-target" };
    }

    // One startup-session entry in portable (TOML-side) form: UTF-8 strings, bare profile
    // GUID, icon in the PORTABLE vocabulary (builtin:NAME or a verbatim native carry).
    struct Entry
    {
        std::string id;
        std::string parentId;
        std::string profile; // bare GUID; "" = follow the default profile
        std::string profileName; // display-name fallback, exported beside the GUID
        std::string name;
        std::string directory;
        std::string command;
        std::string icon; // portable form
        std::string color; // sparse
        std::string colorTarget; // sparse
    };

    // Quote a UTF-8 string as a TOML basic string. Escapes the TOML-mandated set,
    // including \uXXXX for the bare control chars U+0000–U+001F (#431 item 4 — an
    // unescaped control char produces TOML that fails to re-parse).
    inline std::string TomlQuote(std::string_view s)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out = "\"";
        for (const char c : s)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                }
                else
                {
                    out.push_back(c);
                }
                break;
            }
        }
        out.push_back('"');
        return out;
    }

    // Emit one entry as a [[startup-sessions]] block (trailing blank line included).
    // Sparse rules: profile/profile-name omitted when the entry follows the default
    // profile; color omitted when empty; color-target omitted when empty or "both" —
    // matching LaunchEntry::ToJson and the Linux writer.
    inline std::string ToTomlBlock(const Entry& e)
    {
        const auto line = [](std::string_view key, const std::string& value) {
            std::string out{ key };
            out.append(key.size() < 12 ? 12 - key.size() : 0, ' ');
            out += " = ";
            out += TomlQuote(value);
            out += '\n';
            return out;
        };

        std::string out = "[[";
        out += TomlKey::Table;
        out += "]]\n";
        out += line(TomlKey::Id, e.id);
        out += line(TomlKey::ParentId, e.parentId);
        if (!e.profile.empty())
        {
            out += line(TomlKey::Profile, e.profile);
            if (!e.profileName.empty())
            {
                out += line(TomlKey::ProfileName, e.profileName);
            }
        }
        out += line(TomlKey::Name, e.name);
        out += line(TomlKey::Directory, e.directory);
        out += line(TomlKey::Command, e.command);
        out += line(TomlKey::Icon, e.icon);
        if (!e.color.empty())
        {
            out += line(TomlKey::Color, e.color);
        }
        if (!e.colorTarget.empty() && e.colorTarget != "both")
        {
            out += line(TomlKey::ColorTarget, e.colorTarget);
        }
        out += '\n';
        return out;
    }

    // Read one [[startup-sessions]] table into an Entry. Templated on the toml++ table
    // type so this header doesn't itself include <toml.hpp> (the Settings Model uses the
    // JSON-key half of this header and has no toml++ on its include path); instantiate
    // from a TU that includes toml.hpp.
    template<typename TomlTable>
    Entry FromTomlTable(const TomlTable& t)
    {
        const auto field = [&t](std::string_view key) -> std::string {
            return t[key].template value<std::string>().value_or("");
        };
        Entry e;
        e.id = field(TomlKey::Id);
        e.parentId = field(TomlKey::ParentId);
        e.profile = field(TomlKey::Profile);
        e.profileName = field(TomlKey::ProfileName);
        e.name = field(TomlKey::Name);
        e.directory = field(TomlKey::Directory);
        e.command = field(TomlKey::Command);
        e.icon = field(TomlKey::Icon);
        e.color = field(TomlKey::Color);
        e.colorTarget = field(TomlKey::ColorTarget);
        return e;
    }

    // Import-direction icon mapping (portable -> native M13), honoring the registry's
    // `mapped` flag per D18 (#431 item 7): an UNKNOWN `builtin:` name falls back to the
    // empty icon (default placeholder) instead of storing the raw token, and
    // `unknownBuiltin` is set so the caller can log the warning D18 requires. Anything
    // not builtin:-prefixed carries verbatim (a native icon from this platform).
    inline std::wstring ImportIcon(std::wstring_view portableIcon, bool& unknownBuiltin)
    {
        unknownBuiltin = false;
        bool mapped = false;
        auto native = Axan::IconRegistry::PortableToNative(portableIcon, mapped);
        if (!mapped &&
            portableIcon.size() >= Axan::IconRegistry::kBuiltinPrefix.size() &&
            portableIcon.substr(0, Axan::IconRegistry::kBuiltinPrefix.size()) == Axan::IconRegistry::kBuiltinPrefix)
        {
            unknownBuiltin = true;
            return {};
        }
        return native;
    }

    // Export-direction icon mapping (native M13 -> portable): a known Segoe glyph becomes
    // builtin:NAME; anything else (emoji, path, unmapped glyph) carries verbatim, which is
    // the D18 v1 contract for non-portable icons.
    inline std::wstring ExportIcon(std::wstring_view nativeIcon)
    {
        bool mapped = false;
        return Axan::IconRegistry::NativeToPortable(nativeIcon, mapped);
    }
}

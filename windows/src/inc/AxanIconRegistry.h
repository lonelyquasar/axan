// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanIconRegistry — the `builtin:NAME` <-> Segoe Fluent glyph bridge (D18).
//
// Header-only and placed in the shared src/inc so BOTH the app-side exporter
// (TerminalApp/AxanProfileToml) and the settings-editor importer (TerminalSettingsEditor)
// link the same one glyph table without a WinRT projection or cross-lib symbol — there is a
// single source of truth for the icon vocabulary, as D18 requires.
//
// Icons are the one divergent surface between the Linux and Windows builds: Linux stores
// curated symbolic icons as `builtin:NAME` (11 names; linux/src/terminal-sidebar-icons.cc),
// Windows stores M13-language icons (a Segoe Fluent glyph / emoji / image path). The portable
// TOML interchange (spec: root README, Configuration) makes `builtin:NAME` the shared icon
// *vocabulary*; this is the Windows translation at the import/export boundary only — the
// native LaunchEntry.Icon store keeps speaking M13.
//   * import:  builtin:NAME -> the mapped glyph (unknown name -> kept verbatim)
//   * export:  a known glyph -> builtin:NAME for portability (anything else -> verbatim)

#pragma once

#include <array>
#include <string>
#include <string_view>

namespace Axan::IconRegistry
{
    // The portable vocabulary prefix. An icon of the form "builtin:NAME" is the only
    // cross-platform-portable icon form besides the empty string.
    inline constexpr std::wstring_view kBuiltinPrefix{ L"builtin:" };

    namespace details
    {
        struct BuiltinGlyph
        {
            std::wstring_view name; // the portable builtin: name (no prefix)
            wchar_t glyph; // the Segoe Fluent / MDL2 codepoint
        };

        // The 11 curated Linux builtin names (stable, append-only order;
        // linux/src/terminal-sidebar-icons.cc) mapped to a Segoe Fluent / MDL2 glyph.
        //
        // PROVISIONAL GLYPHS: the NAME is the cross-platform contract; the codepoint is a
        // tunable Windows rendering detail. Best-effort from the Segoe Fluent Icons / Segoe
        // MDL2 Assets set — confirm visually in the running app (a wrong codepoint shows a
        // tofu box but never breaks the round-trip). Edit the codepoints freely; do not rename
        // the keys. Numeric (PUA 0xE000-0xF8FF), not \u escapes, to stay ASCII-robust.
        inline constexpr std::array<BuiltinGlyph, 11> kBuiltins{ {
            { L"terminal", 0xE756 }, // CommandPrompt
            { L"ssh", 0xE8CE }, // Remote / link             (verify)
            { L"server", 0xE968 }, // Server                 (verify)
            { L"database", 0xE964 }, // Storage              (verify)
            { L"package", 0xE7B8 }, // Package               (verify)
            { L"container", 0xEDA2 }, // box / drive          (verify)
            { L"git", 0xE8AB }, // Branch / Switch            (verify)
            { L"editor", 0xE943 }, // Code
            { L"logs", 0xE7C3 }, // Page / document           (verify)
            { L"build", 0xE90F }, // Repair / wrench. NOT 0xE15E: IconPathConverter only uses the
            //                                symbol font for U+E700..U+F8FF, so the sidebar/tab
            //                                drew the legacy Segoe UI Symbol codepoint as tofu.
            { L"test", 0xE73E }, // Completed / checkmark       (verify)
        } };
    }

    // The builtin vocabulary itself, exposed for callers that enumerate the whole set —
    // the sidebar's auto-assign glyph pool and the node editor's icon picker (#436 item 1)
    // — so every pickable/auto-assigned icon is one that NativeToPortable round-trips to a
    // portable `builtin:NAME` token.
    using BuiltinGlyph = details::BuiltinGlyph;
    inline constexpr const std::array<BuiltinGlyph, 11>& Builtins() noexcept
    {
        return details::kBuiltins;
    }

    // Map a portable icon token to the Windows-native (M13) icon string.
    //   "builtin:terminal" -> the Segoe glyph; "" -> ""; any non-builtin string -> itself.
    // `mapped` is set true iff the input was a builtin: token that resolved to a glyph; an
    // unknown builtin: name returns the input unchanged with mapped=false (the caller may warn;
    // the raw token is harmlessly inert on Windows).
    inline std::wstring PortableToNative(std::wstring_view portableIcon, bool& mapped)
    {
        mapped = false;
        if (portableIcon.size() <= kBuiltinPrefix.size() ||
            portableIcon.substr(0, kBuiltinPrefix.size()) != kBuiltinPrefix)
        {
            return std::wstring{ portableIcon };
        }
        const auto name = portableIcon.substr(kBuiltinPrefix.size());
        for (const auto& [builtinName, glyph] : details::kBuiltins)
        {
            if (name == builtinName)
            {
                mapped = true;
                return std::wstring(1, glyph);
            }
        }
        return std::wstring{ portableIcon };
    }

    // Map a Windows-native (M13) icon string back to the portable token for export.
    //   a known Segoe glyph -> "builtin:NAME"; anything else (emoji, path, unmapped glyph,
    //   already-"builtin:..." string) -> itself unchanged. `mapped` true iff a glyph matched.
    inline std::wstring NativeToPortable(std::wstring_view nativeIcon, bool& mapped)
    {
        mapped = false;
        if (nativeIcon.size() == 1)
        {
            for (const auto& [builtinName, glyph] : details::kBuiltins)
            {
                if (nativeIcon.front() == glyph)
                {
                    mapped = true;
                    return std::wstring{ kBuiltinPrefix } + std::wstring{ builtinName };
                }
            }
        }
        return std::wstring{ nativeIcon };
    }
}

// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan: setting `sidebar.stateGlyphs`, the glyph in the right-edge slot of a sidebar row.
//
//   "sidebar.stateGlyphs": { "idle": "...", "working": "...", "blocked": "...",
//                            "done": "...", "error": "...", "running": "..." }
//
// Keyed by the slot state the derivation exposes (TerminalApp/AxanSessionState.h Slot /
// SlotKey): the OSC 7501 protocol's own states, plus `running` for an OSC 133 shell command
// in a pane with no 7501 records. `idle` is an agent at rest (a 7501 record exists and
// nothing is happening, including a done/error report already seen); a plain shell at its
// prompt has no slot state and shows nothing.
//
// Each key is optional. A missing key keeps that state's default (idle = Robot, working =
// lightning, running = play, blocked / done / error = nothing: the attention dot on the
// icon already carries those). An empty string shows nothing. Anything else uses the node
// icon vocabulary: a portable `builtin:NAME` token (AxanIconRegistry.h), a literal Segoe
// glyph or emoji, or an image path, rendered by IconPathConverter. Unknown keys are ignored.
//
// Header-only in the shared src/inc (like AxanIconRegistry.h) and pure (no WinRT), so the app
// (TerminalApp), the settings editor and UnitTests_TerminalCore share one definition.

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <AxanIconRegistry.h>

namespace Axan::StateGlyphs
{
    // Static Segoe Fluent / MDL2 glyphs, never animated.
    inline constexpr std::wstring_view kDefaultIdle{ L"\xE99A" }; // Robot: an agent lives here, at rest
    inline constexpr std::wstring_view kDefaultWorking{ L"\xE945" }; // Lightning: the agent is working
    inline constexpr std::wstring_view kDefaultRunning{ L"\xE768" }; // Play: a shell command is running

    // Every key, in the order the settings page lists them.
    inline constexpr std::array<std::wstring_view, 6> kKeys{ L"idle", L"working", L"blocked", L"done", L"error", L"running" };

    // The default for a key; "" for blocked / done / error and for unknown keys.
    constexpr std::wstring_view DefaultFor(std::wstring_view key) noexcept
    {
        if (key == L"idle")
        {
            return kDefaultIdle;
        }
        if (key == L"working")
        {
            return kDefaultWorking;
        }
        if (key == L"running")
        {
            return kDefaultRunning;
        }
        return {};
    }

    constexpr bool IsKey(std::wstring_view key) noexcept
    {
        for (const auto k : kKeys)
        {
            if (k == key)
            {
                return true;
            }
        }
        return false;
    }

    // One value. nullopt (key absent) -> the fallback; otherwise the configured value, trimmed
    // of surrounding whitespace, with a known `builtin:NAME` mapped to its glyph (an unknown
    // builtin: name stays verbatim and renders as nothing, like a node icon would).
    inline std::wstring Resolve(const std::optional<std::wstring>& configured, std::wstring_view fallback)
    {
        if (!configured)
        {
            return std::wstring{ fallback };
        }
        std::wstring_view value{ *configured };
        constexpr std::wstring_view ws{ L" \t\r\n" };
        const auto first = value.find_first_not_of(ws);
        if (first == std::wstring_view::npos)
        {
            return {};
        }
        value = value.substr(first, value.find_last_not_of(ws) - first + 1);
        bool mapped = false;
        return Axan::IconRegistry::PortableToNative(value, mapped);
    }

    // The glyph for a slot key ("" = no slot state -> nothing). `lookup(key)` returns a
    // std::optional<std::wstring>: the configured value, or nullopt when the key is absent
    // (or the setting isn't set at all).
    template<typename Lookup>
    std::wstring GlyphFor(std::wstring_view key, Lookup&& lookup)
    {
        if (!IsKey(key))
        {
            return {};
        }
        return Resolve(lookup(key), DefaultFor(key));
    }
}

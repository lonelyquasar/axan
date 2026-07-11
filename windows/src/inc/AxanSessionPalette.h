// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanSessionPalette — the six-name session color palette (#422).
//
// Header-only and placed in the shared src/inc (the AxanIconRegistry pattern) so BOTH the
// sidebar renderer (TerminalApp/SessionNodeViewModel) and the Startup sessions settings page
// (TerminalSettingsEditor, #13) resolve a stored color token against the same one table —
// previously the tables lived only in SessionNodeViewModel.cpp and growing a settings-page
// color control would have meant a third copy (Linux carries its own by design).
//
// What is STORED on a node/entry is the semantic NAME ("blue"), not a hex — so the same
// stored value resolves to a theme-legible hex at render time and stays portable across
// OSes. The hue sets are ported from the Linux "Axan Session Colors" sets
// (terminal-sidebar.cc node_edit_swatches_light/_dark), pushed for contrast against the
// worst-case surface per theme (#353535 dark / #f6f5f4 light); every hue clears AA (4.5:1)
// on its surface. Names are stable and append-only; edit hexes freely, do not rename.

#pragma once

#include <array>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string_view>

namespace Axan::SessionPalette
{
    struct Swatch
    {
        std::wstring_view name;
        std::wstring_view hex;
    };

    inline constexpr std::array<Swatch, 6> kLight{ {
        { L"red", L"#df0025" },
        { L"orange", L"#a35c00" },
        { L"yellow", L"#787101" },
        { L"green", L"#007f39" },
        { L"blue", L"#0277a4" },
        { L"purple", L"#7d04ff" } } };
    inline constexpr std::array<Swatch, 6> kDark{ {
        { L"red", L"#ff6f69" },
        { L"orange", L"#ff9405" },
        { L"yellow", L"#f8ea09" },
        { L"green", L"#09ff79" },
        { L"blue", L"#12bbff" },
        { L"purple", L"#b5a1ff" } } };

    namespace details
    {
        inline bool AsciiIEquals(std::wstring_view a, std::wstring_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::towlower(a[i]) != std::towlower(b[i]))
                {
                    return false;
                }
            }
            return true;
        }
    }

    // Resolve a stored color token to a paintable hex: "" -> ""; "#hex" -> verbatim (the
    // returned view aliases the input, so the token must outlive it); a palette name
    // (ASCII case-insensitive) -> the active theme set's hex; an unknown name -> "" (no
    // recolor, paint with the theme foreground). Mirrors Linux sidebar_resolve_color_token.
    inline std::wstring_view ResolveToken(std::wstring_view token, bool isDarkTheme)
    {
        if (token.empty())
        {
            return {};
        }
        if (token.front() == L'#')
        {
            return token;
        }
        const auto& pal = isDarkTheme ? kDark : kLight;
        for (const auto& sw : pal)
        {
            if (details::AsciiIEquals(token, sw.name))
            {
                return sw.hex;
            }
        }
        return {};
    }

    // Parse "#RRGGBB" / "#AARRGGBB" into channels (A defaults to 0xFF for the 6-digit
    // form). Returns nullopt for anything else — empty, named colors, malformed — so the
    // caller falls back to the theme foreground. Dependency-free on purpose: each consumer
    // wraps the channels in its own color/brush type.
    struct Rgba
    {
        uint8_t a, r, g, b;
    };
    inline std::optional<Rgba> ParseHexColor(std::wstring_view s)
    {
        if (s.empty() || s.front() != L'#')
        {
            return std::nullopt;
        }
        s.remove_prefix(1);
        if (s.size() != 6 && s.size() != 8)
        {
            return std::nullopt;
        }
        uint32_t v = 0;
        for (const auto c : s)
        {
            uint32_t nibble;
            if (c >= L'0' && c <= L'9')
            {
                nibble = static_cast<uint32_t>(c - L'0');
            }
            else if (c >= L'a' && c <= L'f')
            {
                nibble = static_cast<uint32_t>(c - L'a' + 10);
            }
            else if (c >= L'A' && c <= L'F')
            {
                nibble = static_cast<uint32_t>(c - L'A' + 10);
            }
            else
            {
                return std::nullopt;
            }
            v = (v << 4) | nibble;
        }
        Rgba color{};
        color.a = s.size() == 8 ? static_cast<uint8_t>((v >> 24) & 0xFF) : uint8_t{ 0xFF };
        color.r = static_cast<uint8_t>((v >> 16) & 0xFF);
        color.g = static_cast<uint8_t>((v >> 8) & 0xFF);
        color.b = static_cast<uint8_t>(v & 0xFF);
        return color;
    }
}

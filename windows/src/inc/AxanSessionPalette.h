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
#include <string>
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

    // axan #50: parse what a user types into the identity editor's custom colour box and
    // return the STORED form, lowercase "#rrggbb" (the palette's own hex spelling, and a
    // value ParseHexColor reads back unchanged). Accepted, after trimming outer whitespace:
    //   "#RRGGBB", "RRGGBB" (no hash), "#RGB" (each digit doubled), any hex case;
    //   "rgb(r, g, b)" (keyword ASCII case-insensitive, three decimal 0..255 components,
    //   optional spaces around each).
    // Anything else (empty, a palette name, "#AARRGGBB", a bare 3-digit "abc", percentages,
    // out-of-range or missing components) is nullopt; the caller keeps the previous value.
    inline std::optional<std::wstring> ParseCustomColor(std::wstring_view s)
    {
        const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t'; };
        while (!s.empty() && isSpace(s.front()))
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && isSpace(s.back()))
        {
            s.remove_suffix(1);
        }
        if (s.empty())
        {
            return std::nullopt;
        }

        const auto hexVal = [](wchar_t c) -> int {
            if (c >= L'0' && c <= L'9')
            {
                return c - L'0';
            }
            if (c >= L'a' && c <= L'f')
            {
                return c - L'a' + 10;
            }
            if (c >= L'A' && c <= L'F')
            {
                return c - L'A' + 10;
            }
            return -1;
        };
        const auto format = [](unsigned r, unsigned g, unsigned b) {
            static constexpr wchar_t kDigits[] = L"0123456789abcdef";
            std::wstring out{ L"#" };
            for (const auto ch : { r, g, b })
            {
                out += kDigits[(ch >> 4) & 0xF];
                out += kDigits[ch & 0xF];
            }
            return out;
        };

        // rgb(r, g, b)
        if (s.size() >= 4 && details::AsciiIEquals(s.substr(0, 4), L"rgb("))
        {
            if (s.back() != L')')
            {
                return std::nullopt;
            }
            auto body = s.substr(4, s.size() - 5);
            unsigned ch[3]{};
            for (int i = 0; i < 3; ++i)
            {
                while (!body.empty() && isSpace(body.front()))
                {
                    body.remove_prefix(1);
                }
                size_t n = 0;
                unsigned v = 0;
                while (n < body.size() && body[n] >= L'0' && body[n] <= L'9')
                {
                    if (n >= 3)
                    {
                        return std::nullopt; // more than three digits is out of range
                    }
                    v = v * 10 + static_cast<unsigned>(body[n] - L'0');
                    ++n;
                }
                if (n == 0 || v > 255)
                {
                    return std::nullopt;
                }
                ch[i] = v;
                body.remove_prefix(n);
                while (!body.empty() && isSpace(body.front()))
                {
                    body.remove_prefix(1);
                }
                if (i < 2)
                {
                    if (body.empty() || body.front() != L',')
                    {
                        return std::nullopt;
                    }
                    body.remove_prefix(1);
                }
            }
            if (!body.empty())
            {
                return std::nullopt; // a fourth component or trailing junk
            }
            return format(ch[0], ch[1], ch[2]);
        }

        // Hex: "#RGB" / "#RRGGBB" / bare "RRGGBB".
        const bool hash = s.front() == L'#';
        if (hash)
        {
            s.remove_prefix(1);
        }
        if (!(s.size() == 6 || (hash && s.size() == 3)))
        {
            return std::nullopt;
        }
        unsigned d[6]{};
        for (size_t i = 0; i < s.size(); ++i)
        {
            const auto v = hexVal(s[i]);
            if (v < 0)
            {
                return std::nullopt;
            }
            d[i] = static_cast<unsigned>(v);
        }
        if (s.size() == 3)
        {
            return format(d[0] * 17, d[1] * 17, d[2] * 17);
        }
        return format(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5]);
    }
}

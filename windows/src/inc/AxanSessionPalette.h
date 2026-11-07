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
#include <vector>

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

    // axan #50: the outcome of parsing the custom colour box. color = the STORED form,
    // lowercase "#rrggbb" (the palette's own hex spelling, which ParseHexColor reads back
    // unchanged), or nullopt with error = a short hint for the inline error line, e.g.
    // "blue cannot be 400". error is empty on success.
    struct CustomColorParse
    {
        std::optional<std::wstring> color;
        std::wstring error;
    };

    namespace details
    {
        inline constexpr std::wstring_view kCustomGenericError{ L"Use #RRGGBB or r, g, b" };
        inline constexpr std::wstring_view kCustomCountError{ L"Use three numbers: r, g, b" };
        inline constexpr std::wstring_view kCustomHexError{ L"Hex colors are #RRGGBB" };
        inline constexpr std::array<std::wstring_view, 3> kChannelNames{ { L"red", L"green", L"blue" } };

        inline bool IsCustomSpace(wchar_t c) noexcept
        {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\x00A0';
        }

        inline void TrimCustom(std::wstring_view& v) noexcept
        {
            while (!v.empty() && IsCustomSpace(v.front()))
            {
                v.remove_prefix(1);
            }
            while (!v.empty() && IsCustomSpace(v.back()))
            {
                v.remove_suffix(1);
            }
        }

        inline int HexDigit(wchar_t c) noexcept
        {
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
        }

        inline bool IsDigit(wchar_t c) noexcept
        {
            return c >= L'0' && c <= L'9';
        }

        inline std::wstring FormatHex(unsigned r, unsigned g, unsigned b)
        {
            static constexpr wchar_t kDigits[] = L"0123456789abcdef";
            std::wstring out{ L"#" };
            for (const auto ch : { r, g, b })
            {
                out += kDigits[(ch >> 4) & 0xF];
                out += kDigits[ch & 0xF];
            }
            return out;
        }

        inline CustomColorParse CustomFail(std::wstring error)
        {
            return CustomColorParse{ std::nullopt, std::move(error) };
        }

        inline CustomColorParse CustomOk(std::wstring color)
        {
            return CustomColorParse{ std::move(color), std::wstring{} };
        }

        // An alpha value to ignore: digits with at most one '.', optionally ending in '%'.
        inline bool IsAlphaToken(std::wstring_view t) noexcept
        {
            if (!t.empty() && t.back() == L'%')
            {
                t.remove_suffix(1);
            }
            bool digit = false;
            bool dot = false;
            for (const auto c : t)
            {
                if (IsDigit(c))
                {
                    digit = true;
                }
                else if (c == L'.' && !dot)
                {
                    dot = true;
                }
                else
                {
                    return false;
                }
            }
            return digit;
        }

        // Splits a triplet body on commas (and, with slashOk, '/') and/or whitespace; spaces
        // are optional ("255,200,144"). A whitespace run with at most one comma/slash in it
        // is one separator; an empty component (",," or a leading/trailing comma) fails.
        inline std::optional<std::vector<std::wstring_view>> SplitCustomTriplet(std::wstring_view body, bool slashOk)
        {
            TrimCustom(body);
            std::vector<std::wstring_view> tokens;
            const auto isSep = [&](wchar_t c) { return c == L',' || (slashOk && c == L'/'); };
            while (!body.empty())
            {
                size_t n = 0;
                while (n < body.size() && !IsCustomSpace(body[n]) && !isSep(body[n]))
                {
                    ++n;
                }
                if (n == 0)
                {
                    return std::nullopt; // a separator where a component should be
                }
                tokens.push_back(body.substr(0, n));
                body.remove_prefix(n);
                if (body.empty())
                {
                    break;
                }
                size_t i = 0;
                while (i < body.size() && IsCustomSpace(body[i]))
                {
                    ++i;
                }
                if (i < body.size() && isSep(body[i]))
                {
                    ++i;
                    while (i < body.size() && IsCustomSpace(body[i]))
                    {
                        ++i;
                    }
                }
                body.remove_prefix(i);
                if (body.empty())
                {
                    return std::nullopt; // a trailing comma
                }
            }
            return tokens;
        }

        inline bool AllDigits(std::wstring_view v) noexcept
        {
            for (const auto c : v)
            {
                if (!IsDigit(c))
                {
                    return false;
                }
            }
            return true;
        }

        // r, g, b (+ an ignored alpha when allowAlpha). Count first, then each channel in
        // order, so the hint names the first bad one.
        inline CustomColorParse ParseCustomTriplet(std::wstring_view body, bool allowAlpha)
        {
            const auto tokens = SplitCustomTriplet(body, allowAlpha);
            if (!tokens)
            {
                return CustomFail(std::wstring{ kCustomGenericError });
            }
            if (tokens->size() != 3 && !(allowAlpha && tokens->size() == 4))
            {
                return CustomFail(std::wstring{ kCustomCountError });
            }
            unsigned ch[3]{};
            for (size_t i = 0; i < 3; ++i)
            {
                const auto t = (*tokens)[i];
                const std::wstring name{ kChannelNames[i] };
                auto digits = t;
                const bool negative = !digits.empty() && digits.front() == L'-';
                if (negative || (!digits.empty() && digits.front() == L'+'))
                {
                    digits.remove_prefix(1);
                }
                const auto dot = digits.find(L'.');
                const auto intPart = digits.substr(0, dot);
                const auto fracPart = dot == std::wstring_view::npos ? std::wstring_view{} : digits.substr(dot + 1);
                if (!AllDigits(intPart) || !AllDigits(fracPart) || (intPart.empty() && fracPart.empty()))
                {
                    return CustomFail(std::wstring{ kCustomGenericError }); // not a number at all
                }
                if (dot != std::wstring_view::npos)
                {
                    return CustomFail(name + L" must be a whole number");
                }
                unsigned v = 0;
                bool bad = negative && !intPart.empty() && intPart.find_first_not_of(L'0') != std::wstring_view::npos;
                for (const auto c : intPart)
                {
                    v = v * 10 + static_cast<unsigned>(c - L'0');
                    if (v > 255)
                    {
                        bad = true;
                        break;
                    }
                }
                if (bad)
                {
                    // Echo what was typed, capped so a pasted wall of digits stays short.
                    const auto shown = t.size() > 8 ? std::wstring{ t.substr(0, 8) } + L"\x2026" : std::wstring{ t };
                    return CustomFail(name + L" cannot be " + shown);
                }
                ch[i] = v;
            }
            if (tokens->size() == 4 && !IsAlphaToken((*tokens)[3]))
            {
                return CustomFail(std::wstring{ kCustomGenericError });
            }
            return CustomOk(FormatHex(ch[0], ch[1], ch[2]));
        }

        // "#RGB" / "#RRGGBB" / bare "RRGGBB" (the caller has already decided it's hex).
        inline CustomColorParse ParseCustomHex(std::wstring_view s)
        {
            const bool hash = !s.empty() && s.front() == L'#';
            if (hash)
            {
                s.remove_prefix(1);
            }
            if (!(s.size() == 6 || (hash && s.size() == 3)))
            {
                return CustomFail(std::wstring{ kCustomHexError });
            }
            unsigned d[6]{};
            for (size_t i = 0; i < s.size(); ++i)
            {
                const auto v = HexDigit(s[i]);
                if (v < 0)
                {
                    return CustomFail(std::wstring{ kCustomHexError });
                }
                d[i] = static_cast<unsigned>(v);
            }
            if (s.size() == 3)
            {
                return CustomOk(FormatHex(d[0] * 17, d[1] * 17, d[2] * 17));
            }
            return CustomOk(FormatHex(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5]));
        }
    }

    // axan #50: parse what a user types into the identity editor's custom colour box. Lenient
    // on purpose (UAT: people type "255,200,144" or paste "rgba(...)" from a browser).
    // Accepted, after trimming outer whitespace and ignoring one trailing ';' or '.':
    //   hex: "#RRGGBB", "RRGGBB" (no hash), "#RGB" (each digit doubled), any case;
    //   a decimal triplet r, g, b (each an integer 0..255) separated by commas and/or
    //   whitespace, spaces optional ("255,200,144", "254, 200, 144", "254 200 144"): bare,
    //   in parentheses ("(254, 200, 144)"), or as "rgb(...)" / "rgba(...)" (keyword any
    //   case, space before '(' allowed). The keyword forms also take an ignored fourth
    //   alpha component ("0.5", "50%", after ',', '/' or whitespace); the stored colour is
    //   always opaque.
    // Failures carry a hint: "<channel> cannot be <value>" (out of range or negative),
    // "<channel> must be a whole number", "Use three numbers: r, g, b" (wrong count),
    // "Hex colors are #RRGGBB" (bad hex length or digit), else "Use #RRGGBB or r, g, b".
    // A bare 3-char "abc" is rejected as hex (too easily a word); "#AARRGGBB" likewise.
    inline CustomColorParse ParseCustomColorDetailed(std::wstring_view s)
    {
        details::TrimCustom(s);
        if (!s.empty() && (s.back() == L';' || s.back() == L'.'))
        {
            s.remove_suffix(1);
            details::TrimCustom(s);
        }
        if (s.empty())
        {
            return details::CustomFail(std::wstring{ details::kCustomGenericError });
        }

        // "rgb(...)" / "rgba(...)" / "(...)": the triplet inside the parentheses.
        {
            auto rest = s;
            bool keyword = false;
            if (rest.size() >= 4 && details::AsciiIEquals(rest.substr(0, 4), L"rgba"))
            {
                rest.remove_prefix(4);
                keyword = true;
            }
            else if (rest.size() >= 3 && details::AsciiIEquals(rest.substr(0, 3), L"rgb"))
            {
                rest.remove_prefix(3);
                keyword = true;
            }
            if (keyword)
            {
                details::TrimCustom(rest);
            }
            if (!rest.empty() && rest.front() == L'(')
            {
                if (rest.size() < 2 || rest.back() != L')')
                {
                    return details::CustomFail(std::wstring{ details::kCustomGenericError });
                }
                return details::ParseCustomTriplet(rest.substr(1, rest.size() - 2), keyword);
            }
            if (keyword)
            {
                return details::CustomFail(std::wstring{ details::kCustomGenericError }); // "rgb" without parentheses
            }
        }

        if (s.front() == L'#')
        {
            return details::ParseCustomHex(s);
        }
        // A bare triplet: anything with a comma or inner whitespace.
        for (const auto c : s)
        {
            if (c == L',' || details::IsCustomSpace(c))
            {
                return details::ParseCustomTriplet(s, false);
            }
        }
        // One bare word: hex if it is all hex digits (a wrong length gets the hex hint),
        // otherwise not a colour we know ("blue", "-1").
        for (const auto c : s)
        {
            if (details::HexDigit(c) < 0)
            {
                return details::CustomFail(std::wstring{ details::kCustomGenericError });
            }
        }
        return details::ParseCustomHex(s);
    }

    // The canonical "#rrggbb" or nullopt; see ParseCustomColorDetailed for forms and hints.
    inline std::optional<std::wstring> ParseCustomColor(std::wstring_view s)
    {
        return ParseCustomColorDetailed(s).color;
    }
}

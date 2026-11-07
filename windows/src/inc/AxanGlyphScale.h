// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanGlyphScale — a per-font size multiplier for single-character icons.
//
// IconPathConverter draws a one-character icon (a literal glyph, an emoji, a code point picked
// in the icon picker) as a FontIcon in "Segoe UI", or "Segoe Fluent Icons" for the private-use
// range, and leaves anything Segoe UI lacks to DirectWrite's font fallback. Some fallback fonts
// draw their glyphs much smaller in the em than Segoe UI or Segoe UI Emoji do: Segoe UI
// Historic's Egyptian hieroglyphs and runes come out visibly undersized next to an emoji or a
// Fluent glyph at the same font size. ScaleFor gives the factor the FontIcon's size is
// multiplied by so they read at the same visual size; everything else is 1.0.
//
// Two tables, each one line per row:
//   * kFonts: font family -> multiplier. Add a font here to tune how big its glyphs draw.
//   * kBlocks: a Unicode range -> the font that draws it (the font the system falls back to on
//     Windows 11). Add a range here to route more characters to an existing font's scale.
// A font with no row is 1.0, and so is a character in no range.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanGlyphSets.h, so the icon
// converter and the unit tests share it.

#pragma once

#include <array>
#include <string_view>

namespace Axan::GlyphScale
{
    struct FontScale
    {
        std::wstring_view font;
        double scale;
    };

    // The multiplier per font family. Segoe Fluent Icons, Segoe UI Emoji and Segoe UI are the
    // reference sizes; Segoe UI Historic draws hieroglyphs and runes small.
    inline constexpr std::array<FontScale, 4> kFonts{ {
        { L"Segoe Fluent Icons", 1.0 },
        { L"Segoe UI Emoji", 1.0 },
        { L"Segoe UI", 1.0 },
        { L"Segoe UI Historic", 1.35 },
    } };

    struct Block
    {
        char32_t first;
        char32_t last;
        std::wstring_view font;
    };

    // Unicode ranges and the font that draws them.
    inline constexpr std::array<Block, 2> kBlocks{ {
        { 0x16A0, 0x16FF, L"Segoe UI Historic" }, // Runic
        { 0x13000, 0x1345F, L"Segoe UI Historic" }, // Egyptian Hieroglyphs + Format Controls
    } };

    constexpr double FontScaleFor(std::wstring_view font) noexcept
    {
        for (const auto& f : kFonts)
        {
            if (f.font == font)
            {
                return f.scale;
            }
        }
        return 1.0;
    }

    // The first code point of a UTF-16 string (a surrogate pair combined), 0 when empty or a
    // lone surrogate.
    constexpr char32_t FirstCodepoint(std::wstring_view text) noexcept
    {
        if (text.empty())
        {
            return 0;
        }
        const auto hi = static_cast<char32_t>(text[0]);
        if (hi >= 0xD800 && hi <= 0xDBFF)
        {
            if (text.size() < 2)
            {
                return 0;
            }
            const auto lo = static_cast<char32_t>(text[1]);
            if (lo < 0xDC00 || lo > 0xDFFF)
            {
                return 0;
            }
            return 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (hi >= 0xDC00 && hi <= 0xDFFF)
        {
            return 0;
        }
        return hi;
    }

    // The multiplier for a glyph icon: the scale of the font its first code point falls back
    // to, else 1.0. Only the first code point counts: a surrogate pair is one character, an
    // emoji sequence (a base plus variation selectors or joiners) starts with an emoji, and a
    // glyph name or a path starts with a letter, none of which any range lists.
    constexpr double ScaleFor(std::wstring_view glyph) noexcept
    {
        const auto cp = FirstCodepoint(glyph);
        if (cp == 0)
        {
            return 1.0;
        }
        for (const auto& b : kBlocks)
        {
            if (cp >= b.first && cp <= b.last)
            {
                return FontScaleFor(b.font);
            }
        }
        return 1.0;
    }
}

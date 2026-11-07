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
// multiplied by so they read at the same visual size.
//
// The tables here are the defaults, one line per row:
//   * kFonts: font family -> multiplier. Add a font here to tune how big its glyphs draw.
//   * kBlocks: a Unicode range -> the font that draws it (the font the system falls back to on
//     Windows 11). Add a range here to route more characters to an existing font's scale.
// A font with no row is 1.0. The user's sidebar.iconFontScale ({ "family": multiplier })
// overrides a family's default; the ScaleFor / FontScaleFor overloads that take a lookup apply
// it, clamped to [kMinScale, kMaxScale].
//
// FontForGlyph names the family a glyph is drawn from (Segoe Fluent Icons for the private-use
// range, a kBlocks font, Segoe UI Emoji for emoji, else Segoe UI); FamilyForIcon does the same
// for a raw icon string (a glyph name, code-point text, an emoji, a path), "" for an image.
// The Sidebar settings page lists the families in use with it.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanGlyphSets.h, so the icon
// converter, the settings editor and the unit tests share it.

#pragma once

#include <AxanGlyphNames.h> // ResolveIcon: glyph names / code-point text -> the glyph

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Axan::GlyphScale
{
    struct FontScale
    {
        std::wstring_view font;
        double scale;
    };

    inline constexpr std::wstring_view kFluent{ L"Segoe Fluent Icons" };
    inline constexpr std::wstring_view kEmoji{ L"Segoe UI Emoji" };
    inline constexpr std::wstring_view kText{ L"Segoe UI" };
    inline constexpr std::wstring_view kHistoric{ L"Segoe UI Historic" };

    // The default multiplier per font family. Segoe Fluent Icons, Segoe UI Emoji and Segoe UI
    // are the reference sizes; Segoe UI Historic draws hieroglyphs and runes small.
    inline constexpr std::array<FontScale, 4> kFonts{ {
        { kFluent, 1.0 },
        { kEmoji, 1.0 },
        { kText, 1.0 },
        { kHistoric, 1.35 },
    } };

    // The range a sidebar.iconFontScale value is clamped to (the settings page's spin box).
    inline constexpr double kMinScale = 0.5;
    inline constexpr double kMaxScale = 3.0;

    struct Block
    {
        char32_t first;
        char32_t last;
        std::wstring_view font;
    };

    // Unicode ranges and the font that draws them: the historic scripts Segoe UI Historic
    // supplies (docs/startup-sessions-toml.md lists them). Syriac is left out: it is a living
    // script with its own fallback fonts.
    inline constexpr std::array<Block, 25> kBlocks{ {
        { 0x1680, 0x169F, kHistoric }, // Ogham
        { 0x16A0, 0x16FF, kHistoric }, // Runic
        { 0x2C00, 0x2C5F, kHistoric }, // Glagolitic
        { 0x10280, 0x1029F, kHistoric }, // Lycian
        { 0x102A0, 0x102DF, kHistoric }, // Carian
        { 0x10300, 0x1032F, kHistoric }, // Old Italic
        { 0x10330, 0x1034F, kHistoric }, // Gothic
        { 0x10380, 0x1039F, kHistoric }, // Ugaritic
        { 0x103A0, 0x103DF, kHistoric }, // Old Persian
        { 0x10450, 0x1047F, kHistoric }, // Shavian
        { 0x10800, 0x1083F, kHistoric }, // Cypriot Syllabary
        { 0x10840, 0x1085F, kHistoric }, // Imperial Aramaic
        { 0x10900, 0x1091F, kHistoric }, // Phoenician
        { 0x10920, 0x1093F, kHistoric }, // Lydian
        { 0x109A0, 0x109FF, kHistoric }, // Meroitic Cursive
        { 0x10A00, 0x10A5F, kHistoric }, // Kharoshthi
        { 0x10A60, 0x10A7F, kHistoric }, // Old South Arabian
        { 0x10B40, 0x10B5F, kHistoric }, // Inscriptional Parthian
        { 0x10B60, 0x10B7F, kHistoric }, // Inscriptional Pahlavi
        { 0x10C00, 0x10C4F, kHistoric }, // Old Turkic
        { 0x11000, 0x1107F, kHistoric }, // Brahmi
        { 0x12000, 0x123FF, kHistoric }, // Cuneiform
        { 0x12400, 0x1247F, kHistoric }, // Cuneiform Numbers and Punctuation
        { 0x13000, 0x1342F, kHistoric }, // Egyptian Hieroglyphs
        { 0x13430, 0x1345F, kHistoric }, // Egyptian Hieroglyph Format Controls
    } };

    // A user value clamped to [kMinScale, kMaxScale]; NaN reads as 1.0.
    constexpr double ClampScale(double value) noexcept
    {
        if (!(value == value))
        {
            return 1.0;
        }
        return value < kMinScale ? kMinScale : (value > kMaxScale ? kMaxScale : value);
    }

    // Font family names compare case-insensitively (ASCII), as Windows font lookup does, so a
    // sidebar.iconFontScale key "segoe ui historic" is the Segoe UI Historic row.
    constexpr wchar_t FoldFamilyChar(wchar_t ch) noexcept
    {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    }

    constexpr bool SameFamily(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (FoldFamilyChar(a[i]) != FoldFamilyChar(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    // A case-insensitive, transparent ordering for maps and sets keyed by family name.
    struct FamilyLess
    {
        using is_transparent = void;
        constexpr bool operator()(std::wstring_view a, std::wstring_view b) const noexcept
        {
            const auto n = a.size() < b.size() ? a.size() : b.size();
            for (size_t i = 0; i < n; ++i)
            {
                const auto x = FoldFamilyChar(a[i]);
                const auto y = FoldFamilyChar(b[i]);
                if (x != y)
                {
                    return x < y;
                }
            }
            return a.size() < b.size();
        }
    };

    // The kFonts spelling of a family ("Segoe UI Historic" for "segoe ui historic"), or ""
    // when it has no kFonts row.
    constexpr std::wstring_view KnownFamily(std::wstring_view family) noexcept
    {
        for (const auto& f : kFonts)
        {
            if (SameFamily(f.font, family))
            {
                return f.font;
            }
        }
        return {};
    }

    // A family as stored: its kFonts spelling when it has a row, else as typed.
    constexpr std::wstring_view CanonicalFamily(std::wstring_view family) noexcept
    {
        const auto known = KnownFamily(family);
        return known.empty() ? family : known;
    }

    // The built-in multiplier for a font family (1.0 for one with no kFonts row).
    constexpr double FontScaleFor(std::wstring_view font) noexcept
    {
        for (const auto& f : kFonts)
        {
            if (SameFamily(f.font, font))
            {
                return f.scale;
            }
        }
        return 1.0;
    }

    // A family's multiplier with the user's overrides: lookup(font) returns the
    // sidebar.iconFontScale value as a std::optional<double>, nullopt when the family has none.
    template<typename Lookup>
    double FontScaleFor(std::wstring_view font, Lookup&& lookup)
    {
        if (const std::optional<double> v = lookup(font))
        {
            return ClampScale(*v);
        }
        return FontScaleFor(font);
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

    // Emoji blocks: Mahjong Tiles .. Symbols and Pictographs Extended-A, plus Miscellaneous
    // Symbols and Dingbats. A character followed by U+FE0F (emoji presentation) counts too.
    constexpr bool IsEmojiCodepoint(char32_t cp) noexcept
    {
        return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF);
    }

    // The family a glyph (resolved text, not a name) is drawn from: Segoe Fluent Icons for the
    // private-use range IconPathConverter sends there, a kBlocks font, Segoe UI Emoji for an
    // emoji, else Segoe UI. "" for an empty string or a lone surrogate. Only the first code
    // point counts (plus a U+FE0F right after it): a surrogate pair is one character and an
    // emoji sequence starts with an emoji.
    constexpr std::wstring_view FontForGlyph(std::wstring_view glyph) noexcept
    {
        const auto cp = FirstCodepoint(glyph);
        if (cp == 0)
        {
            return {};
        }
        if (cp >= 0xE700 && cp <= 0xF8FF)
        {
            return kFluent;
        }
        for (const auto& b : kBlocks)
        {
            if (cp >= b.first && cp <= b.last)
            {
                return b.font;
            }
        }
        const size_t units = cp >= 0x10000 ? 2 : 1;
        if (IsEmojiCodepoint(cp) || (glyph.size() > units && glyph[units] == 0xFE0F))
        {
            return kEmoji;
        }
        return kText;
    }

    // The default multiplier for a glyph icon: the scale of the font it is drawn from.
    constexpr double ScaleFor(std::wstring_view glyph) noexcept
    {
        return FontScaleFor(FontForGlyph(glyph));
    }

    // The same with the user's overrides (see FontScaleFor(font, lookup)).
    template<typename Lookup>
    double ScaleFor(std::wstring_view glyph, Lookup&& lookup)
    {
        return FontScaleFor(FontForGlyph(glyph), std::forward<Lookup>(lookup));
    }

    // Whether resolved icon text is drawn as a glyph rather than loaded as an image path. A
    // pure approximation of Utils::IsLikelyToBeEmojiOrSymbolIcon (which asks ICU whether the
    // text is one grapheme): a first character followed by no ASCII at all. An emoji
    // sequence's joiners and selectors are not ASCII; a path has a separator or an extension.
    constexpr bool LooksLikeGlyph(std::wstring_view text) noexcept
    {
        const auto cp = FirstCodepoint(text);
        if (cp == 0)
        {
            return false;
        }
        for (size_t i = cp >= 0x10000 ? 2 : 1; i < text.size(); ++i)
        {
            if (text[i] <= 0x7F)
            {
                return false;
            }
        }
        return true;
    }

    // The family a raw icon string (as written in settings: a glyph name, "U+131A3", an emoji,
    // a literal glyph, an image path) is drawn from, resolved the way IconPathConverter does.
    // "" for an image path or an empty string.
    inline std::wstring FamilyForIcon(std::wstring_view icon)
    {
        const auto resolved = Axan::GlyphNames::ResolveIcon(icon);
        if (!LooksLikeGlyph(resolved))
        {
            return {};
        }
        return std::wstring{ FontForGlyph(resolved) };
    }
}

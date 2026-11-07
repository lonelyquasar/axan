// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanGlyphSets — the icon picker's character sets, and code-point search.
//
// The picker grid (AxanGlyphGrid.h) shows ONE set at a time, chosen in a selector beside its
// search box; the search filters within that set. A set is a display name, a font family and
// its entries (Set below). Two ship today:
//   * "Segoe Fluent Icons" (the default): the whole font, AxanSegoeFluentGlyphs.h. A pick
//     stores the glyph's NAME ("robot"); AxanGlyphNames.h resolves it at render time.
//   * "Letters and digits": a-z, A-Z, 0-9, drawn in Segoe UI. A pick stores the literal
//     character ("a"). IconPathConverter already draws any single non-symbol character as
//     text, so the stored form needs no resolver and reads the same in settings.json and the
//     TOML. A one-character string is never a file (BitmapIcon needs an absolute URI) and
//     never a glyph name (ResolveIcon resolves only names longer than one character), so it
//     isn't ambiguous. The labels ("letter-a", "letter-A", "digit-0") are search terms only;
//     typing one into an icon field does not resolve.
// A future set (another Unicode font, #42) is a TableSet over a new Entry table plus one
// line in Sets(); the grid and its hosts pick it up without changes.
//
// Code points: the search box also takes a Unicode scalar value, as U+131A3, u+131a3,
// 0x131A3 or a bare 131A3 (4-6 hex digits; ParseCodepoint). The grid filters to it when the
// selected set has it, and otherwise offers one "direct" cell that stores the literal
// character (UTF-16) and leaves the drawing to DirectWrite's font fallback (Segoe UI
// Historic draws U+131A3 on Windows 11). Surrogates, non-characters and C0/C1 controls
// parse to nothing.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanGlyphNames.h, so the grid,
// both editors and the unit tests share it.

#pragma once

#include <AxanGlyphNames.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Axan::GlyphSets
{
    inline constexpr std::wstring_view kSymbolFont{ L"Segoe Fluent Icons, Segoe MDL2 Assets" };
    inline constexpr std::wstring_view kTextFont{ L"Segoe UI" };

    // One cell of a set, as the picker shows and stores it.
    struct Cell
    {
        std::wstring glyph; // what the cell draws, in the set's font
        std::wstring_view key; // the search label, e.g. "robot", "letter-A"
        std::wstring_view displayName; // under the glyph, e.g. "Robot", "Letter A"
        std::wstring_view alias; // a second search name for the tooltip ("git"), or ""
        std::wstring stored; // what a pick writes to the icon field
    };

    class Set
    {
    public:
        virtual ~Set() = default;
        virtual std::wstring_view Name() const noexcept = 0; // the selector's label
        virtual std::wstring_view FontFamily() const noexcept = 0; // a XAML FontFamily string
        virtual size_t Size() const noexcept = 0;
        virtual Cell At(size_t index) const = 0;
        // Indices matching a query, in the set's order; an empty query matches all.
        virtual std::vector<size_t> Search(std::wstring_view query) const = 0;
        // The entry a stored icon value denotes, if it is in this set.
        virtual std::optional<size_t> Find(std::wstring_view icon) const = 0;
        // The entry that draws this code point, if any.
        virtual std::optional<size_t> FindCodepoint(char32_t codepoint) const = 0;
    };

    // ---- code points ----

    // A scalar value worth offering as an icon: not a surrogate, not a non-character
    // (U+FDD0..U+FDEF, U+xxFFFE/U+xxFFFF), not a C0/C1 control, at most U+10FFFF.
    constexpr bool IsPickableScalar(char32_t cp) noexcept
    {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            return false;
        }
        if ((cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE)
        {
            return false;
        }
        return !(cp < 0x20 || (cp >= 0x7F && cp <= 0x9F));
    }

    // "U+131A3", "u+131a3", "0x131A3" or a bare "131A3" (blanks around it ignored): 4 to 6
    // hex digits naming a pickable scalar value. Anything else is nullopt.
    inline std::optional<char32_t> ParseCodepoint(std::wstring_view query) noexcept
    {
        const auto first = query.find_first_not_of(L" \t");
        if (first == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        query = query.substr(first, query.find_last_not_of(L" \t") - first + 1);
        if (query.size() > 2 &&
            (((query[0] == L'U' || query[0] == L'u') && query[1] == L'+') ||
             (query[0] == L'0' && (query[1] == L'x' || query[1] == L'X'))))
        {
            query.remove_prefix(2);
        }
        if (query.size() < 4 || query.size() > 6)
        {
            return std::nullopt;
        }
        char32_t value = 0;
        for (const auto ch : query)
        {
            unsigned digit;
            if (ch >= L'0' && ch <= L'9')
            {
                digit = static_cast<unsigned>(ch - L'0');
            }
            else if (ch >= L'a' && ch <= L'f')
            {
                digit = static_cast<unsigned>(ch - L'a') + 10;
            }
            else if (ch >= L'A' && ch <= L'F')
            {
                digit = static_cast<unsigned>(ch - L'A') + 10;
            }
            else
            {
                return std::nullopt;
            }
            value = static_cast<char32_t>(value * 16 + digit);
        }
        return IsPickableScalar(value) ? std::optional<char32_t>{ value } : std::nullopt;
    }

    // The code point as UTF-16 (a surrogate pair above U+FFFF): the stored form.
    inline std::wstring ToUtf16(char32_t cp)
    {
        if (cp < 0x10000)
        {
            return std::wstring(1, static_cast<wchar_t>(cp));
        }
        const auto v = cp - 0x10000;
        return std::wstring{ static_cast<wchar_t>(0xD800 + (v >> 10)), static_cast<wchar_t>(0xDC00 + (v & 0x3FF)) };
    }

    // "U+131A3": uppercase hex, at least four digits.
    inline std::wstring FormatCodepoint(char32_t cp)
    {
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        std::wstring digits;
        for (auto v = cp; v != 0 || digits.size() < 4; v >>= 4)
        {
            digits.insert(digits.begin(), hex[v & 0xF]);
        }
        return L"U+" + digits;
    }

    // ---- the Segoe Fluent Icons set ----

    class SegoeFluentSet final : public Set
    {
    public:
        std::wstring_view Name() const noexcept override { return L"Segoe Fluent Icons"; }
        std::wstring_view FontFamily() const noexcept override { return kSymbolFont; }
        size_t Size() const noexcept override { return GlyphNames::All().size(); }
        Cell At(size_t index) const override
        {
            const auto& g = GlyphNames::All()[index];
            return { std::wstring(1, g.codepoint), g.key, g.displayName, GlyphNames::AliasFor(g.codepoint), std::wstring{ g.key } };
        }
        std::vector<size_t> Search(std::wstring_view query) const override { return GlyphNames::Search(query); }
        std::optional<size_t> Find(std::wstring_view icon) const override
        {
            if (const auto cp = GlyphNames::ResolveCodepoint(icon))
            {
                return FindCodepoint(*cp);
            }
            return std::nullopt;
        }
        std::optional<size_t> FindCodepoint(char32_t codepoint) const override
        {
            if (codepoint > 0xFFFF)
            {
                return std::nullopt;
            }
            if (const auto g = GlyphNames::FindByCodepoint(static_cast<wchar_t>(codepoint)))
            {
                return static_cast<size_t>(g - GlyphNames::All().data());
            }
            return std::nullopt;
        }
    };

    // ---- table sets: literal characters in a text font ----

    struct Entry
    {
        std::wstring_view text; // the character(s) drawn and stored
        std::wstring_view key; // search label, case kept ("letter-A")
        std::wstring_view displayName;
    };

    // Indices into a table matching a query, by Search()'s word rules: every word must
    // match. A one-character word matches an entry whose text is that character, either
    // case ("a" finds a and A, not every "letter-..."); a longer word must appear in the
    // lowercased key ("letter", "digit", "letter q"). An empty query matches all.
    inline std::vector<size_t> SearchTable(const Entry* entries, size_t count, std::wstring_view query)
    {
        const auto lower = [](wchar_t ch) noexcept {
            return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
        };
        std::vector<std::wstring> words;
        {
            std::wstring word;
            for (const auto ch : query)
            {
                if (ch == L' ' || ch == L'_' || ch == L'-' || ch == L'\t')
                {
                    if (!word.empty())
                    {
                        words.push_back(std::move(word));
                        word.clear();
                    }
                    continue;
                }
                word.push_back(lower(ch));
            }
            if (!word.empty())
            {
                words.push_back(std::move(word));
            }
        }

        std::vector<size_t> hits;
        for (size_t i = 0; i < count; ++i)
        {
            const auto& e = entries[i];
            std::wstring key{ e.key };
            std::transform(key.begin(), key.end(), key.begin(), lower);
            const auto matches = std::all_of(words.begin(), words.end(), [&](const std::wstring& w) {
                return w.size() == 1 ? (e.text.size() == 1 && lower(e.text.front()) == w.front()) : key.find(w) != std::wstring::npos;
            });
            if (matches)
            {
                hits.push_back(i);
            }
        }
        return hits;
    }

    class TableSet final : public Set
    {
    public:
        template<size_t N>
        constexpr TableSet(std::wstring_view name, std::wstring_view font, const std::array<Entry, N>& entries) noexcept :
            _name{ name }, _font{ font }, _entries{ entries.data() }, _count{ N } {}

        std::wstring_view Name() const noexcept override { return _name; }
        std::wstring_view FontFamily() const noexcept override { return _font; }
        size_t Size() const noexcept override { return _count; }
        Cell At(size_t index) const override
        {
            const auto& e = _entries[index];
            return { std::wstring{ e.text }, e.key, e.displayName, {}, std::wstring{ e.text } };
        }
        std::vector<size_t> Search(std::wstring_view query) const override { return SearchTable(_entries, _count, query); }
        std::optional<size_t> Find(std::wstring_view icon) const override
        {
            for (size_t i = 0; i < _count; ++i)
            {
                if (_entries[i].text == icon)
                {
                    return i;
                }
            }
            return std::nullopt;
        }
        std::optional<size_t> FindCodepoint(char32_t codepoint) const override { return Find(ToUtf16(codepoint)); }

    private:
        std::wstring_view _name;
        std::wstring_view _font;
        const Entry* _entries;
        size_t _count;
    };

    // Hand-written: a-z, A-Z, 0-9, in the grid's order.
    inline constexpr std::array<Entry, 62> kLettersAndDigits{ {
        { L"a", L"letter-a", L"Letter a" },
        { L"b", L"letter-b", L"Letter b" },
        { L"c", L"letter-c", L"Letter c" },
        { L"d", L"letter-d", L"Letter d" },
        { L"e", L"letter-e", L"Letter e" },
        { L"f", L"letter-f", L"Letter f" },
        { L"g", L"letter-g", L"Letter g" },
        { L"h", L"letter-h", L"Letter h" },
        { L"i", L"letter-i", L"Letter i" },
        { L"j", L"letter-j", L"Letter j" },
        { L"k", L"letter-k", L"Letter k" },
        { L"l", L"letter-l", L"Letter l" },
        { L"m", L"letter-m", L"Letter m" },
        { L"n", L"letter-n", L"Letter n" },
        { L"o", L"letter-o", L"Letter o" },
        { L"p", L"letter-p", L"Letter p" },
        { L"q", L"letter-q", L"Letter q" },
        { L"r", L"letter-r", L"Letter r" },
        { L"s", L"letter-s", L"Letter s" },
        { L"t", L"letter-t", L"Letter t" },
        { L"u", L"letter-u", L"Letter u" },
        { L"v", L"letter-v", L"Letter v" },
        { L"w", L"letter-w", L"Letter w" },
        { L"x", L"letter-x", L"Letter x" },
        { L"y", L"letter-y", L"Letter y" },
        { L"z", L"letter-z", L"Letter z" },
        { L"A", L"letter-A", L"Letter A" },
        { L"B", L"letter-B", L"Letter B" },
        { L"C", L"letter-C", L"Letter C" },
        { L"D", L"letter-D", L"Letter D" },
        { L"E", L"letter-E", L"Letter E" },
        { L"F", L"letter-F", L"Letter F" },
        { L"G", L"letter-G", L"Letter G" },
        { L"H", L"letter-H", L"Letter H" },
        { L"I", L"letter-I", L"Letter I" },
        { L"J", L"letter-J", L"Letter J" },
        { L"K", L"letter-K", L"Letter K" },
        { L"L", L"letter-L", L"Letter L" },
        { L"M", L"letter-M", L"Letter M" },
        { L"N", L"letter-N", L"Letter N" },
        { L"O", L"letter-O", L"Letter O" },
        { L"P", L"letter-P", L"Letter P" },
        { L"Q", L"letter-Q", L"Letter Q" },
        { L"R", L"letter-R", L"Letter R" },
        { L"S", L"letter-S", L"Letter S" },
        { L"T", L"letter-T", L"Letter T" },
        { L"U", L"letter-U", L"Letter U" },
        { L"V", L"letter-V", L"Letter V" },
        { L"W", L"letter-W", L"Letter W" },
        { L"X", L"letter-X", L"Letter X" },
        { L"Y", L"letter-Y", L"Letter Y" },
        { L"Z", L"letter-Z", L"Letter Z" },
        { L"0", L"digit-0", L"Digit 0" },
        { L"1", L"digit-1", L"Digit 1" },
        { L"2", L"digit-2", L"Digit 2" },
        { L"3", L"digit-3", L"Digit 3" },
        { L"4", L"digit-4", L"Digit 4" },
        { L"5", L"digit-5", L"Digit 5" },
        { L"6", L"digit-6", L"Digit 6" },
        { L"7", L"digit-7", L"Digit 7" },
        { L"8", L"digit-8", L"Digit 8" },
        { L"9", L"digit-9", L"Digit 9" },
    } };

    // ---- the registry ----

    // Every set, in the selector's order; the first is the default. Add a set here.
    inline const std::vector<const Set*>& Sets()
    {
        static const SegoeFluentSet segoe;
        static const TableSet letters{ L"Letters and digits", kTextFont, kLettersAndDigits };
        static const std::vector<const Set*> sets{ &segoe, &letters };
        return sets;
    }

    // The index of the first set holding a stored icon value, or nullopt.
    inline std::optional<size_t> SetForIcon(std::wstring_view icon)
    {
        const auto& sets = Sets();
        for (size_t i = 0; i < sets.size(); ++i)
        {
            if (sets[i]->Find(icon))
            {
                return i;
            }
        }
        return std::nullopt;
    }
}

// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanGlyphNames — resolve a stored icon NAME to its Segoe Fluent glyph (#39).
//
// The icon picker offers the whole Segoe Fluent Icons font (AxanSegoeFluentGlyphs.h, ~1,400
// glyphs) and stores the glyph's name ("robot") so settings.json and the TOML stay readable.
// Every renderer goes through IconPathConverter, which calls ResolveIcon() first, so a name
// renders wherever a literal glyph did. The older forms keep working unchanged: a literal
// glyph character, an emoji, an image path, and a portable "builtin:NAME" token.
//
// Name forms accepted (case-insensitive; spaces and underscores read as hyphens):
//   * a font key ("robot", "global-nav-button", "Global Nav Button")
//   * one of the eleven AxanIconRegistry builtin names, bare ("terminal", "git"), which
//     win over a font key of the same spelling so "git" and "builtin:git" agree
//   * a "builtin:NAME" token
// Code-point text resolves too (#44): "U+131A3", "u+131a3", "0x131A3" or a bare "131A3"
// (4-6 hex digits, the picker search's grammar; surrogates, non-characters and controls
// rejected) renders as that character. A name of the same spelling wins.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanIconRegistry.h, so the
// UI helpers, both editors and the unit tests share it.

#pragma once

#include <AxanIconRegistry.h>
#include <AxanSegoeFluentGlyphs.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Axan::GlyphNames
{
    using Glyph = Axan::SegoeFluentGlyphs::Glyph;

    inline constexpr wchar_t kFirstSymbol = 0xE700; // IconPathConverter's symbol-font range
    inline constexpr wchar_t kLastSymbol = 0xF8FF;

    inline const auto& All() noexcept
    {
        return Axan::SegoeFluentGlyphs::kGlyphs;
    }

    // The key form of a typed name: lowercase ASCII, runs of space / '_' / '-' collapsed to
    // one hyphen, trimmed. Anything outside [A-Za-z0-9 _-] (a path separator, a dot, an
    // emoji) means "not a name" and yields "" so a file path never resolves as a glyph.
    inline std::wstring NormalizeName(std::wstring_view text)
    {
        if (text.empty() || text.size() > 64)
        {
            return {};
        }
        std::wstring out;
        out.reserve(text.size());
        bool pendingHyphen = false;
        for (const auto ch : text)
        {
            if (ch == L' ' || ch == L'_' || ch == L'-')
            {
                pendingHyphen = !out.empty();
                continue;
            }
            wchar_t lower;
            if (ch >= L'A' && ch <= L'Z')
            {
                lower = static_cast<wchar_t>(ch - L'A' + L'a');
            }
            else if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9'))
            {
                lower = ch;
            }
            else
            {
                return {};
            }
            if (pendingHyphen)
            {
                out.push_back(L'-');
                pendingHyphen = false;
            }
            out.push_back(lower);
        }
        return out;
    }

    // ---- code points (shared with the picker search, AxanGlyphSets.h) ----

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

    // The font entry with exactly this key (already normalized), or null.
    inline const Glyph* FindByKey(std::wstring_view key) noexcept
    {
        const auto& all = All();
        const auto it = std::lower_bound(all.begin(), all.end(), key, [](const Glyph& g, std::wstring_view k) { return g.key < k; });
        return (it != all.end() && it->key == key) ? &*it : nullptr;
    }

    // The font entry for a codepoint, or null (a PUA glyph the list doesn't name).
    inline const Glyph* FindByCodepoint(wchar_t codepoint) noexcept
    {
        for (const auto& g : All())
        {
            if (g.codepoint == codepoint)
            {
                return &g;
            }
        }
        return nullptr;
    }

    // The builtin alias name for a codepoint ("terminal" for U+E756), or "".
    inline std::wstring_view AliasFor(wchar_t codepoint) noexcept
    {
        for (const auto& b : Axan::IconRegistry::Builtins())
        {
            if (b.glyph == codepoint)
            {
                return b.name;
            }
        }
        return {};
    }

    // The glyph an icon string denotes, if it denotes one: a single symbol-range character,
    // a builtin:NAME token, a bare builtin name, or a font key. Emoji, image paths and
    // unknown names yield nullopt.
    inline std::optional<wchar_t> ResolveCodepoint(std::wstring_view icon)
    {
        if (icon.size() == 1 && icon.front() >= kFirstSymbol && icon.front() <= kLastSymbol)
        {
            return icon.front();
        }
        auto name = icon;
        if (name.size() > Axan::IconRegistry::kBuiltinPrefix.size() &&
            name.substr(0, Axan::IconRegistry::kBuiltinPrefix.size()) == Axan::IconRegistry::kBuiltinPrefix)
        {
            name.remove_prefix(Axan::IconRegistry::kBuiltinPrefix.size());
        }
        const auto key = NormalizeName(name);
        if (key.empty())
        {
            return std::nullopt;
        }
        for (const auto& b : Axan::IconRegistry::Builtins())
        {
            if (key == b.name)
            {
                return b.glyph;
            }
        }
        if (name.size() != icon.size())
        {
            return std::nullopt; // builtin: is the eleven-name vocabulary only
        }
        if (const auto g = FindByKey(key))
        {
            return g->codepoint;
        }
        return std::nullopt;
    }

    // What IconPathConverter renders: a name becomes its glyph character; code-point text
    // ("U+131A3", "0x131A3", a bare "131A3"; ParseCodepoint's grammar, #44) becomes that
    // character in UTF-16, exactly what a picker "direct" pick stores, so it draws through
    // the same single-character FontIcon path (and the same AxanGlyphScale factor). Names
    // win: a bare hex word that is also a glyph key ("cafe") is the glyph; write "U+CAFE"
    // for the code point. Anything else (a literal glyph, an emoji, a path, an unknown
    // name) is returned unchanged.
    inline std::wstring ResolveIcon(std::wstring_view icon)
    {
        if (icon.size() > 1)
        {
            if (const auto cp = ResolveCodepoint(icon))
            {
                return std::wstring(1, *cp);
            }
            if (const auto scalar = ParseCodepoint(icon))
            {
                return ToUtf16(*scalar);
            }
        }
        return std::wstring{ icon };
    }

    // True when an icon string reads as a glyph NAME (or a builtin:NAME token) but denotes
    // nothing, so it renders as the placeholder (#44: IconPathConverter logs it). Paths,
    // URIs, emoji and literal characters never qualify: a name is [A-Za-z0-9 _-] only.
    // "none" is WT's "no icon" value, not a misspelled glyph.
    inline bool IsUnresolvedName(std::wstring_view icon)
    {
        if (icon.size() < 2 || ResolveCodepoint(icon) || ParseCodepoint(icon))
        {
            return false;
        }
        auto name = icon;
        if (name.size() > Axan::IconRegistry::kBuiltinPrefix.size() &&
            name.substr(0, Axan::IconRegistry::kBuiltinPrefix.size()) == Axan::IconRegistry::kBuiltinPrefix)
        {
            name.remove_prefix(Axan::IconRegistry::kBuiltinPrefix.size());
        }
        const auto key = NormalizeName(name);
        return !key.empty() && key != L"none";
    }

    // Indices into All() whose key contains the normalized query, or whose builtin alias
    // does (so "git" finds the branch glyph). Every word of a multi-word query must appear,
    // in any order ("button nav" finds "global-nav-button"). An empty query matches all.
    inline std::vector<size_t> Search(std::wstring_view query)
    {
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
                word.push_back((ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch);
            }
            if (!word.empty())
            {
                words.push_back(std::move(word));
            }
        }

        std::vector<size_t> hits;
        const auto& all = All();
        hits.reserve(words.empty() ? all.size() : 64);
        for (size_t i = 0; i < all.size(); ++i)
        {
            const auto& g = all[i];
            const auto alias = AliasFor(g.codepoint);
            const auto matches = std::all_of(words.begin(), words.end(), [&](const std::wstring& w) {
                return g.key.find(w) != std::wstring_view::npos ||
                       (!alias.empty() && alias.find(w) != std::wstring_view::npos);
            });
            if (matches)
            {
                hits.push_back(i);
            }
        }
        return hits;
    }
}

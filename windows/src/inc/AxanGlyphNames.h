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
// (4-6 hex digits, the picker search's grammar; the bare form needs a decimal digit, so an
// all-letter word like "face" is a name, not U+FACE; surrogates, non-characters and controls
// rejected) renders as that character. A name of the same spelling wins.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanIconRegistry.h, so the
// UI helpers, both editors and the unit tests share it.

#pragma once

#include <AxanIconRegistry.h>
#include <AxanSegoeFluentGlyphs.h>

#include <algorithm>
#include <cstdint>
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

    // How a string reads as code-point text: not code-point text at all, code-point text that
    // names no pickable scalar (a surrogate, a non-character, a control, past U+10FFFF, or the
    // wrong digit count after a prefix), or a valid one.
    enum class CodepointTextKind
    {
        NotCodepoint,
        Invalid,
        Valid,
    };

    struct CodepointText
    {
        CodepointTextKind kind = CodepointTextKind::NotCodepoint;
        char32_t value = 0;
    };

    // The grammar ParseCodepoint accepts (blanks around it ignored): "U+131A3", "u+131a3",
    // "0x131A3" with 4 to 6 hex digits, or a bare "131A3": 4 to 6 hex digits with at least one
    // decimal digit among them. An all-letter word ("face", "decade") is never code-point text
    // bare; write "U+FACE". A prefix with 1 to 8 hex digits is code-point text even when the
    // count is wrong, so "U+41" reads as an invalid code point rather than a name.
    inline CodepointText ClassifyCodepoint(std::wstring_view query) noexcept
    {
        const auto first = query.find_first_not_of(L" \t");
        if (first == std::wstring_view::npos)
        {
            return {};
        }
        query = query.substr(first, query.find_last_not_of(L" \t") - first + 1);
        bool prefixed = false;
        if (query.size() > 2 &&
            (((query[0] == L'U' || query[0] == L'u') && query[1] == L'+') ||
             (query[0] == L'0' && (query[1] == L'x' || query[1] == L'X'))))
        {
            query.remove_prefix(2);
            prefixed = true;
        }
        if (query.empty() || query.size() > (prefixed ? 8u : 6u))
        {
            return {};
        }
        uint64_t value = 0;
        bool anyDecimal = false;
        for (const auto ch : query)
        {
            unsigned digit;
            if (ch >= L'0' && ch <= L'9')
            {
                digit = static_cast<unsigned>(ch - L'0');
                anyDecimal = true;
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
                return {};
            }
            value = value * 16 + digit;
        }
        if (!prefixed && (query.size() < 4 || !anyDecimal))
        {
            return {};
        }
        if (query.size() < 4 || query.size() > 6 || value > 0x10FFFF || !IsPickableScalar(static_cast<char32_t>(value)))
        {
            return { CodepointTextKind::Invalid, static_cast<char32_t>(value) };
        }
        return { CodepointTextKind::Valid, static_cast<char32_t>(value) };
    }

    // The pickable scalar code-point text names (ClassifyCodepoint's grammar), else nullopt.
    inline std::optional<char32_t> ParseCodepoint(std::wstring_view query) noexcept
    {
        const auto parsed = ClassifyCodepoint(query);
        return parsed.kind == CodepointTextKind::Valid ? std::optional<char32_t>{ parsed.value } : std::nullopt;
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

    namespace Detail
    {
        // An icon string split into its name key: "builtin:" stripped (prefixed = true), then
        // NormalizeName'd. key is "" when the string is not name-shaped.
        struct NameKey
        {
            std::wstring key;
            bool prefixed = false;
        };

        inline NameKey SplitName(std::wstring_view icon)
        {
            auto name = icon;
            const auto prefix = Axan::IconRegistry::kBuiltinPrefix;
            const bool prefixed = name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix;
            if (prefixed)
            {
                name.remove_prefix(prefix.size());
            }
            return { NormalizeName(name), prefixed };
        }

        // The glyph a name key denotes: a builtin name, or (unprefixed only) a font key.
        inline std::optional<wchar_t> GlyphForKey(const NameKey& name)
        {
            if (name.key.empty())
            {
                return std::nullopt;
            }
            for (const auto& b : Axan::IconRegistry::Builtins())
            {
                if (name.key == b.name)
                {
                    return b.glyph;
                }
            }
            if (name.prefixed)
            {
                return std::nullopt; // builtin: is the eleven-name vocabulary only
            }
            if (const auto g = FindByKey(name.key))
            {
                return g->codepoint;
            }
            return std::nullopt;
        }
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
        return Detail::GlyphForKey(Detail::SplitName(icon));
    }

    // What Resolve made of an icon string.
    enum class IconResolution
    {
        PassThrough, // not a name or code point: a literal glyph, emoji, path, "none", ""
        Resolved, // a glyph name or valid code-point text, now its character
        UnknownName, // name-shaped but denotes nothing: renders as the placeholder
        InvalidCodepoint, // code-point text naming no pickable scalar: renders as the placeholder
    };

    struct ResolvedIcon
    {
        std::wstring text;
        IconResolution kind = IconResolution::PassThrough;
    };

    // What IconPathConverter renders, and why, in one pass: a name becomes its glyph
    // character; code-point text ("U+131A3", "0x131A3", a bare "131A3"; ClassifyCodepoint's
    // grammar, #44) becomes that character in UTF-16, exactly what a picker "direct" pick
    // stores, so it draws through the same single-character FontIcon path (and the same
    // AxanGlyphScale factor). Names win: a bare hex word that is also a glyph key ("cafe")
    // is the glyph; write "U+CAFE" for the code point. Anything else (a literal glyph, an
    // emoji, a path, an unknown name, invalid code-point text) is returned unchanged, and
    // kind says which of those it was. Paths, URIs, emoji and literal characters are never
    // UnknownName: a name is [A-Za-z0-9 _-] only, and "none" is WT's "no icon" value.
    inline ResolvedIcon Resolve(std::wstring_view icon)
    {
        if (icon.size() <= 1)
        {
            return { std::wstring{ icon }, IconResolution::PassThrough };
        }
        const auto name = Detail::SplitName(icon);
        if (const auto cp = Detail::GlyphForKey(name))
        {
            return { std::wstring(1, *cp), IconResolution::Resolved };
        }
        const auto parsed = ClassifyCodepoint(icon);
        if (parsed.kind == CodepointTextKind::Valid)
        {
            return { ToUtf16(parsed.value), IconResolution::Resolved };
        }
        if (parsed.kind == CodepointTextKind::Invalid)
        {
            return { std::wstring{ icon }, IconResolution::InvalidCodepoint };
        }
        const bool unknown = !name.key.empty() && name.key != L"none";
        return { std::wstring{ icon }, unknown ? IconResolution::UnknownName : IconResolution::PassThrough };
    }

    // Resolve's text alone.
    inline std::wstring ResolveIcon(std::wstring_view icon)
    {
        return Resolve(icon).text;
    }

    // True when an icon string reads as a glyph NAME (or a builtin:NAME token) but denotes
    // nothing, so it renders as the placeholder (#44).
    inline bool IsUnresolvedName(std::wstring_view icon)
    {
        return Resolve(icon).kind == IconResolution::UnknownName;
    }

    // A search query split into its words: separated by space, tab, '_' or '-' (empty words
    // dropped), ASCII-lowercased. Shared with the picker's set search (AxanGlyphSets.h
    // SearchTable), so both read a query the same way.
    inline std::vector<std::wstring> SplitQueryWords(std::wstring_view query)
    {
        std::vector<std::wstring> words;
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
        return words;
    }

    // Indices into All() whose key contains the normalized query, or whose builtin alias
    // does (so "git" finds the branch glyph). Every word of a multi-word query must appear,
    // in any order ("button nav" finds "global-nav-button"). An empty query matches all.
    inline std::vector<size_t> Search(std::wstring_view query)
    {
        const auto words = SplitQueryWords(query);

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

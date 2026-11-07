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

    // What IconPathConverter renders: a name becomes its glyph character; anything else
    // (a literal glyph, an emoji, a path, an unknown name) is returned unchanged.
    inline std::wstring ResolveIcon(std::wstring_view icon)
    {
        if (icon.size() > 1)
        {
            if (const auto cp = ResolveCodepoint(icon))
            {
                return std::wstring(1, *cp);
            }
        }
        return std::wstring{ icon };
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

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
// Letters and digits (a-z, A-Z, 0-9; kTextIcons) are offered as text icons too. They are
// stored as the literal character ("a", "Q", "7"), not as a name: IconPathConverter already
// draws any single non-symbol character as text (Segoe UI), so the stored form needs no
// resolver and reads the same in settings.json and the TOML. A bare one-character string is
// never a file (BitmapIcon needs an absolute URI) and never a glyph name (ResolveIcon only
// resolves names longer than one character), so it isn't ambiguous. Their names
// ("letter-a", "letter-A", "digit-0") are picker labels and search terms only; typing one
// into an icon field does not resolve.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanIconRegistry.h, so the
// UI helpers, both editors and the unit tests share it.

#pragma once

#include <AxanIconRegistry.h>
#include <AxanSegoeFluentGlyphs.h>

#include <algorithm>
#include <array>
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

    // ---- letters and digits ----

    struct TextIcon
    {
        wchar_t character; // what settings store: the character itself
        std::wstring_view key; // the picker's label / search term, case kept ("letter-A")
        std::wstring_view displayName;
    };

    // Hand-written (not generated): a-z, A-Z, 0-9, in that order (the picker's order).
    inline constexpr std::array<TextIcon, 62> kTextIcons{ {
        { L'a', L"letter-a", L"Letter a" },
        { L'b', L"letter-b", L"Letter b" },
        { L'c', L"letter-c", L"Letter c" },
        { L'd', L"letter-d", L"Letter d" },
        { L'e', L"letter-e", L"Letter e" },
        { L'f', L"letter-f", L"Letter f" },
        { L'g', L"letter-g", L"Letter g" },
        { L'h', L"letter-h", L"Letter h" },
        { L'i', L"letter-i", L"Letter i" },
        { L'j', L"letter-j", L"Letter j" },
        { L'k', L"letter-k", L"Letter k" },
        { L'l', L"letter-l", L"Letter l" },
        { L'm', L"letter-m", L"Letter m" },
        { L'n', L"letter-n", L"Letter n" },
        { L'o', L"letter-o", L"Letter o" },
        { L'p', L"letter-p", L"Letter p" },
        { L'q', L"letter-q", L"Letter q" },
        { L'r', L"letter-r", L"Letter r" },
        { L's', L"letter-s", L"Letter s" },
        { L't', L"letter-t", L"Letter t" },
        { L'u', L"letter-u", L"Letter u" },
        { L'v', L"letter-v", L"Letter v" },
        { L'w', L"letter-w", L"Letter w" },
        { L'x', L"letter-x", L"Letter x" },
        { L'y', L"letter-y", L"Letter y" },
        { L'z', L"letter-z", L"Letter z" },
        { L'A', L"letter-A", L"Letter A" },
        { L'B', L"letter-B", L"Letter B" },
        { L'C', L"letter-C", L"Letter C" },
        { L'D', L"letter-D", L"Letter D" },
        { L'E', L"letter-E", L"Letter E" },
        { L'F', L"letter-F", L"Letter F" },
        { L'G', L"letter-G", L"Letter G" },
        { L'H', L"letter-H", L"Letter H" },
        { L'I', L"letter-I", L"Letter I" },
        { L'J', L"letter-J", L"Letter J" },
        { L'K', L"letter-K", L"Letter K" },
        { L'L', L"letter-L", L"Letter L" },
        { L'M', L"letter-M", L"Letter M" },
        { L'N', L"letter-N", L"Letter N" },
        { L'O', L"letter-O", L"Letter O" },
        { L'P', L"letter-P", L"Letter P" },
        { L'Q', L"letter-Q", L"Letter Q" },
        { L'R', L"letter-R", L"Letter R" },
        { L'S', L"letter-S", L"Letter S" },
        { L'T', L"letter-T", L"Letter T" },
        { L'U', L"letter-U", L"Letter U" },
        { L'V', L"letter-V", L"Letter V" },
        { L'W', L"letter-W", L"Letter W" },
        { L'X', L"letter-X", L"Letter X" },
        { L'Y', L"letter-Y", L"Letter Y" },
        { L'Z', L"letter-Z", L"Letter Z" },
        { L'0', L"digit-0", L"Digit 0" },
        { L'1', L"digit-1", L"Digit 1" },
        { L'2', L"digit-2", L"Digit 2" },
        { L'3', L"digit-3", L"Digit 3" },
        { L'4', L"digit-4", L"Digit 4" },
        { L'5', L"digit-5", L"Digit 5" },
        { L'6', L"digit-6", L"Digit 6" },
        { L'7', L"digit-7", L"Digit 7" },
        { L'8', L"digit-8", L"Digit 8" },
        { L'9', L"digit-9", L"Digit 9" },
    } };

    // The letter / digit an icon string is, or null: exactly one ASCII alphanumeric.
    inline const TextIcon* FindTextIcon(std::wstring_view icon) noexcept
    {
        if (icon.size() != 1)
        {
            return nullptr;
        }
        for (const auto& t : kTextIcons)
        {
            if (t.character == icon.front())
            {
                return &t;
            }
        }
        return nullptr;
    }

    // Indices into kTextIcons matching a query, by the same word rules as Search(): every
    // word must match. A one-character word matches the character itself, either case ("a"
    // finds a and A, not every "letter-..."); a longer word must appear in the lowercased
    // key ("letter", "digit", "letter q"). An empty query matches all.
    inline std::vector<size_t> SearchTextIcons(std::wstring_view query)
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
        for (size_t i = 0; i < kTextIcons.size(); ++i)
        {
            const auto& t = kTextIcons[i];
            std::wstring key{ t.key };
            std::transform(key.begin(), key.end(), key.begin(), lower);
            const auto matches = std::all_of(words.begin(), words.end(), [&](const std::wstring& w) {
                return w.size() == 1 ? w.front() == lower(t.character) : key.find(w) != std::wstring::npos;
            });
            if (matches)
            {
                hits.push_back(i);
            }
        }
        return hits;
    }

    // True when the query is one letter or digit (blanks aside): the picker then lists the
    // matching text icons first, ahead of the font glyphs whose names merely contain it.
    inline bool IsSingleCharacterQuery(std::wstring_view query) noexcept
    {
        const auto first = query.find_first_not_of(L" \t");
        const auto last = query.find_last_not_of(L" \t");
        return first != std::wstring_view::npos && first == last && FindTextIcon(query.substr(first, 1)) != nullptr;
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

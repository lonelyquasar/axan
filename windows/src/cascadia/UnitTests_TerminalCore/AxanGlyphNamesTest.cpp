// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #39: the Segoe Fluent glyph name table (src/inc/AxanSegoeFluentGlyphs.h) and its
// lookups (src/inc/AxanGlyphNames.h): name -> codepoint, the eleven builtin aliases, the
// forms that must NOT resolve (paths, emoji, unknown names), search, the export mapping;
// and the picker's character sets and code-point search (src/inc/AxanGlyphSets.h).

#include "pch.h"
#include <WexTestClass.h>

#include <AxanGlyphNames.h>
#include <AxanGlyphSets.h>
#include <AxanLaunchEntryWire.h>

#include <string>

using namespace WEX::Logging;
namespace G = Axan::GlyphNames;
namespace S = Axan::GlyphSets;

namespace TerminalCoreUnitTests
{
    class AxanGlyphNamesTest
    {
        TEST_CLASS(AxanGlyphNamesTest);

        static bool Resolves(std::wstring_view icon, wchar_t expected)
        {
            const auto cp = G::ResolveCodepoint(icon);
            return cp.has_value() && *cp == expected;
        }

        TEST_METHOD(TableIsSortedUniqueAndInTheSymbolRange)
        {
            const auto& all = G::All();
            VERIFY_IS_GREATER_THAN(all.size(), size_t{ 1000 });
            for (size_t i = 0; i < all.size(); ++i)
            {
                VERIFY_IS_TRUE(all[i].codepoint >= G::kFirstSymbol && all[i].codepoint <= G::kLastSymbol);
                VERIFY_ARE_EQUAL(std::wstring{ all[i].key }, G::NormalizeName(all[i].key));
                if (i > 0)
                {
                    VERIFY_IS_TRUE(all[i - 1].key < all[i].key); // binary search depends on it
                }
            }
        }

        TEST_METHOD(NameToCodepoint)
        {
            VERIFY_IS_TRUE(Resolves(L"robot", 0xE99A));
            VERIFY_IS_TRUE(Resolves(L"command-prompt", 0xE756));
            Log::Comment(L"Case, spaces and underscores are forgiven: the display name works too");
            VERIFY_IS_TRUE(Resolves(L"Robot", 0xE99A));
            VERIFY_IS_TRUE(Resolves(L"Global Nav Button", 0xE700));
            VERIFY_IS_TRUE(Resolves(L"global_nav__button", 0xE700));
            VERIFY_IS_TRUE(Resolves(L"  global-nav-button ", 0xE700));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE99A }), G::ResolveIcon(L"robot"));
            const auto g = G::FindByCodepoint(0xE99A);
            VERIFY_IS_NOT_NULL(g);
            VERIFY_ARE_EQUAL(std::wstring{ L"robot" }, std::wstring{ g->key });
        }

        TEST_METHOD(BuiltinAliasesResolve)
        {
            for (const auto& b : Axan::IconRegistry::Builtins())
            {
                Log::Comment(std::wstring{ b.name }.c_str());
                VERIFY_IS_TRUE(Resolves(b.name, b.glyph));
                VERIFY_IS_TRUE(Resolves(std::wstring{ Axan::IconRegistry::kBuiltinPrefix } + std::wstring{ b.name }, b.glyph));
                VERIFY_ARE_EQUAL(std::wstring{ b.name }, std::wstring{ G::AliasFor(b.glyph) });
            }
            Log::Comment(L"An alias wins over nothing else: 'git' is not a font key but resolves");
            VERIFY_IS_NULL(G::FindByKey(L"git"));
            VERIFY_IS_TRUE(Resolves(L"GIT", 0xE8AB));
            Log::Comment(L"builtin: is the eleven-name vocabulary only, not the whole font");
            VERIFY_IS_FALSE(G::ResolveCodepoint(L"builtin:robot").has_value());
        }

        TEST_METHOD(LiteralFormsAndUnknownNamesPassThrough)
        {
            Log::Comment(L"A literal symbol glyph is its own codepoint, named or not");
            VERIFY_IS_TRUE(Resolves(std::wstring(1, wchar_t{ 0xE756 }), 0xE756));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE756 }), G::ResolveIcon(std::wstring(1, wchar_t{ 0xE756 })));

            for (const auto* icon : { L"", L"no-such-glyph-name", L"C:\\icons\\robot.png", L"robot.png",
                                      L"ms-appx:///ProfileIcons/robot.png", L"%USERPROFILE%\\robot",
                                      L"\U0001F916", L"x", L"builtin:", L"builtin:nope" })
            {
                Log::Comment(icon);
                VERIFY_IS_FALSE(G::ResolveCodepoint(icon).has_value());
                VERIFY_ARE_EQUAL(std::wstring{ icon }, G::ResolveIcon(icon));
            }
            Log::Comment(L"An over-long string is never treated as a name");
            VERIFY_IS_FALSE(G::ResolveCodepoint(std::wstring(80, L'a')).has_value());
        }

        TEST_METHOD(SearchMatchesKeysWordsAndAliases)
        {
            VERIFY_ARE_EQUAL(G::All().size(), G::Search(L"").size());
            VERIFY_ARE_EQUAL(G::All().size(), G::Search(L"   ").size());

            const auto contains = [](const std::vector<size_t>& hits, std::wstring_view key) {
                for (const auto i : hits)
                {
                    if (G::All()[i].key == key)
                    {
                        return true;
                    }
                }
                return false;
            };
            VERIFY_IS_TRUE(contains(G::Search(L"robot"), L"robot"));
            VERIFY_IS_TRUE(contains(G::Search(L"ROB"), L"robot"));
            Log::Comment(L"Words match in any order");
            VERIFY_IS_TRUE(contains(G::Search(L"button nav"), L"global-nav-button"));
            Log::Comment(L"An alias finds its glyph under the font's own name");
            VERIFY_IS_TRUE(contains(G::Search(L"terminal"), L"command-prompt"));
            VERIFY_ARE_EQUAL(size_t{ 0 }, G::Search(L"zzzz-no-match").size());
        }

        TEST_METHOD(SetsRegistry)
        {
            const auto& sets = S::Sets();
            VERIFY_ARE_EQUAL(size_t{ 2 }, sets.size());
            Log::Comment(L"Segoe Fluent Icons is first (the default) and is the whole font, stored by name");
            VERIFY_ARE_EQUAL(std::wstring{ L"Segoe Fluent Icons" }, std::wstring{ sets[0]->Name() });
            VERIFY_ARE_EQUAL(G::All().size(), sets[0]->Size());
            VERIFY_ARE_EQUAL(G::All().size(), sets[0]->Search(L"").size());
            const auto robot = sets[0]->Find(L"robot");
            VERIFY_IS_TRUE(robot.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"robot" }, sets[0]->At(*robot).stored);
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE99A }), sets[0]->At(*robot).glyph);
            VERIFY_IS_TRUE(sets[0]->Find(std::wstring(1, wchar_t{ 0xE99A })) == robot); // a literal glyph too
            VERIFY_IS_TRUE(sets[0]->FindCodepoint(0xE99A) == robot);
            VERIFY_IS_FALSE(sets[0]->Find(L"a").has_value());
            VERIFY_IS_FALSE(sets[0]->FindCodepoint(0x131A3).has_value());

            Log::Comment(L"The second set is the letters and digits; a stored icon finds its set");
            VERIFY_ARE_EQUAL(std::wstring{ L"Letters and digits" }, std::wstring{ sets[1]->Name() });
            VERIFY_IS_TRUE(S::SetForIcon(L"robot") == std::optional<size_t>{ 0 });
            VERIFY_IS_TRUE(S::SetForIcon(L"Q") == std::optional<size_t>{ 1 });
            VERIFY_IS_FALSE(S::SetForIcon(L"C:\\x\\y.png").has_value());
            VERIFY_IS_FALSE(S::SetForIcon(L"").has_value());
        }

        TEST_METHOD(LettersAndDigitsAreTextIcons)
        {
            const auto& letters = *S::Sets()[1];
            VERIFY_ARE_EQUAL(size_t{ 62 }, letters.Size());
            for (size_t i = 0; i < letters.Size(); ++i)
            {
                const auto cell = letters.At(i);
                VERIFY_ARE_EQUAL(size_t{ 1 }, cell.stored.size());
                const auto ch = cell.stored.front();
                VERIFY_IS_TRUE((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'));
                for (size_t j = 0; j < i; ++j)
                {
                    VERIFY_IS_TRUE(letters.At(j).stored != cell.stored); // one cell per character
                    VERIFY_IS_TRUE(letters.At(j).key != cell.key);
                }
                VERIFY_IS_TRUE(letters.Find(cell.stored) == std::optional<size_t>{ i });
                VERIFY_IS_TRUE(letters.FindCodepoint(ch) == std::optional<size_t>{ i });
                Log::Comment(L"Stored as the character itself, which renders as text unchanged");
                VERIFY_ARE_EQUAL(cell.stored, G::ResolveIcon(cell.stored));
                VERIFY_IS_FALSE(G::ResolveCodepoint(cell.stored).has_value());
            }
            VERIFY_ARE_EQUAL(std::wstring{ L"letter-a" }, std::wstring{ letters.At(*letters.Find(L"a")).key });
            VERIFY_ARE_EQUAL(std::wstring{ L"letter-A" }, std::wstring{ letters.At(*letters.Find(L"A")).key });
            VERIFY_ARE_EQUAL(std::wstring{ L"digit-0" }, std::wstring{ letters.At(*letters.Find(L"0")).key });
            for (const auto* icon : { L"", L"ab", L"-", L"\U0001F916", L"letter-a", L"robot" })
            {
                Log::Comment(icon);
                VERIFY_IS_FALSE(letters.Find(icon).has_value());
            }
        }

        TEST_METHOD(SearchFindsLettersAndDigits)
        {
            const auto& letters = *S::Sets()[1];
            const auto chars = [&](const std::vector<size_t>& hits) {
                std::wstring out;
                for (const auto i : hits)
                {
                    out += letters.At(i).stored;
                }
                return out;
            };
            VERIFY_ARE_EQUAL(size_t{ 62 }, letters.Search(L"").size());
            Log::Comment(L"One character finds that letter in both cases, not every 'letter-...'");
            VERIFY_ARE_EQUAL(std::wstring{ L"aA" }, chars(letters.Search(L"a")));
            VERIFY_ARE_EQUAL(std::wstring{ L"aA" }, chars(letters.Search(L" A ")));
            VERIFY_ARE_EQUAL(std::wstring{ L"eE" }, chars(letters.Search(L"e")));
            VERIFY_ARE_EQUAL(std::wstring{ L"7" }, chars(letters.Search(L"7")));
            Log::Comment(L"Longer words match the label");
            VERIFY_ARE_EQUAL(size_t{ 52 }, letters.Search(L"letter").size());
            VERIFY_ARE_EQUAL(size_t{ 10 }, letters.Search(L"DIGIT").size());
            VERIFY_ARE_EQUAL(std::wstring{ L"qQ" }, chars(letters.Search(L"letter q")));
            VERIFY_ARE_EQUAL(std::wstring{ L"3" }, chars(letters.Search(L"digit-3")));
            VERIFY_ARE_EQUAL(size_t{ 0 }, letters.Search(L"robot").size());
            Log::Comment(L"The sets search separately: the font's search has no letters in it");
            VERIFY_ARE_EQUAL(G::Search(L"letter").size(), S::Sets()[0]->Search(L"letter").size());
        }

        TEST_METHOD(CodepointQueryForms)
        {
            const auto is = [](std::wstring_view q, char32_t cp) {
                const auto v = S::ParseCodepoint(q);
                return v.has_value() && *v == cp;
            };
            for (const auto* q : { L"U+131A3", L"u+131a3", L"0x131A3", L"0X131a3", L"131A3", L"  U+131A3  " })
            {
                Log::Comment(q);
                VERIFY_IS_TRUE(is(q, 0x131A3));
            }
            Log::Comment(L"4 to 6 hex digits, leading zeros allowed");
            VERIFY_IS_TRUE(is(L"U+0041", 0x41));
            VERIFY_IS_TRUE(is(L"E99A", 0xE99A));
            VERIFY_IS_TRUE(is(L"face", 0xFACE)); // a bare hex word counts
            VERIFY_IS_TRUE(is(L"U+10FFFD", 0x10FFFD));
            VERIFY_IS_TRUE(is(L"01F916", 0x1F916));
            for (const auto* q : { L"", L"U+", L"U+41", L"123", L"1234567", L"U+1234567", L"U+12G4", L"robot",
                                   L"x1234", L"U 1234", L"+1234", L"0x", L"U+110000", L"FFFFFF" })
            {
                Log::Comment(q);
                VERIFY_IS_FALSE(S::ParseCodepoint(q).has_value());
            }
            Log::Comment(L"Surrogates, non-characters and controls parse to nothing");
            for (const auto* q : { L"U+D800", L"U+DBFF", L"U+DC00", L"U+DFFF", L"U+FDD0", L"U+FDEF", L"U+FFFE",
                                   L"U+FFFF", L"U+1FFFE", L"U+10FFFF", L"U+0000", L"U+001F", L"U+007F", L"U+009F" })
            {
                Log::Comment(q);
                VERIFY_IS_FALSE(S::ParseCodepoint(q).has_value());
            }
            VERIFY_IS_TRUE(is(L"U+FDCF", 0xFDCF));
            VERIFY_IS_TRUE(is(L"U+FFFD", 0xFFFD));
            VERIFY_IS_TRUE(is(L"U+00A0", 0xA0));
        }

        TEST_METHOD(CodepointStoredFormAndLabel)
        {
            VERIFY_ARE_EQUAL(std::wstring{ L"A" }, S::ToUtf16(0x41));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE99A }), S::ToUtf16(0xE99A));
            VERIFY_ARE_EQUAL(std::wstring{ L"\U000131A3" }, S::ToUtf16(0x131A3));
            VERIFY_ARE_EQUAL(std::wstring{ L"\U0010FFFD" }, S::ToUtf16(0x10FFFD));
            VERIFY_ARE_EQUAL(std::wstring{ L"U+131A3" }, S::FormatCodepoint(0x131A3));
            VERIFY_ARE_EQUAL(std::wstring{ L"U+0041" }, S::FormatCodepoint(0x41));
            VERIFY_ARE_EQUAL(std::wstring{ L"U+E99A" }, S::FormatCodepoint(0xE99A));
            Log::Comment(L"A direct pick passes through the resolver as a literal character");
            VERIFY_ARE_EQUAL(S::ToUtf16(0x131A3), G::ResolveIcon(S::ToUtf16(0x131A3)));
        }

        TEST_METHOD(CodepointTextResolvesAsAnIcon)
        {
            Log::Comment(L"#44: each picker-search form resolves to the character, as a direct pick stores it");
            for (const auto* icon : { L"U+131A3", L"u+131a3", L"0x131A3", L"0X131a3", L"131A3", L" U+131A3 " })
            {
                Log::Comment(icon);
                VERIFY_ARE_EQUAL(S::ToUtf16(0x131A3), G::ResolveIcon(icon));
                VERIFY_IS_FALSE(G::IsUnresolvedName(icon));
            }
            Log::Comment(L"A BMP symbol code point gives the same single character a literal glyph does");
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE756 }), G::ResolveIcon(L"U+E756"));
            VERIFY_ARE_EQUAL(std::wstring{ L"\u16A0" }, G::ResolveIcon(L"U+16A0"));
            Log::Comment(L"Surrogates, non-characters and controls are rejected and pass through verbatim");
            for (const auto* icon : { L"U+D800", L"U+DFFF", L"U+FFFE", L"U+FDD0", L"U+001F", L"U+110000" })
            {
                Log::Comment(icon);
                VERIFY_ARE_EQUAL(std::wstring{ icon }, G::ResolveIcon(icon));
            }
            Log::Comment(L"A glyph name wins over a bare hex word of the same spelling; U+ forces the code point");
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xEC32 }), G::ResolveIcon(L"cafe"));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xCAFE }), G::ResolveIcon(L"U+CAFE"));
        }

        TEST_METHOD(UnresolvedNamesAreDetected)
        {
            Log::Comment(L"#44: name-shaped strings that denote nothing are flagged for the converter's warning");
            for (const auto* icon : { L"no-such-glyph-name", L"Robto", L"builtin:nope", L"Global Nav Buton" })
            {
                Log::Comment(icon);
                VERIFY_IS_TRUE(G::IsUnresolvedName(icon));
            }
            Log::Comment(L"Real names, code points, paths, emoji, single characters and \"none\" are not");
            for (const auto* icon : { L"", L"x", L"robot", L"git", L"builtin:git", L"U+131A3", L"none", L"None",
                                      L"C:\\icons\\robot.png", L"robot.png", L"ms-appx:///ProfileIcons/robot.png",
                                      L"%USERPROFILE%\\robot", L"\U0001F916", L"builtin:" })
            {
                Log::Comment(icon);
                VERIFY_IS_FALSE(G::IsUnresolvedName(icon));
            }
        }

        TEST_METHOD(ExportMapsBuiltinGlyphNamesToPortableTokens)
        {
            namespace W = Axan::LaunchEntryWire;
            VERIFY_ARE_EQUAL(std::wstring{ L"builtin:terminal" }, W::ExportIcon(L"command-prompt"));
            VERIFY_ARE_EQUAL(std::wstring{ L"builtin:terminal" }, W::ExportIcon(L"terminal"));
            VERIFY_ARE_EQUAL(std::wstring{ L"builtin:terminal" }, W::ExportIcon(std::wstring(1, wchar_t{ 0xE756 })));
            Log::Comment(L"Other names stay readable and verbatim (Windows-only)");
            VERIFY_ARE_EQUAL(std::wstring{ L"robot" }, W::ExportIcon(L"robot"));
            VERIFY_ARE_EQUAL(std::wstring{ L"C:\\x\\y.png" }, W::ExportIcon(L"C:\\x\\y.png"));
        }
    };
}

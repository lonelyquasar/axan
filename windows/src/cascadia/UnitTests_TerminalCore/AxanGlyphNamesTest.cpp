// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #39: the Segoe Fluent glyph name table (src/inc/AxanSegoeFluentGlyphs.h) and its
// lookups (src/inc/AxanGlyphNames.h): name -> codepoint, the eleven builtin aliases, the
// forms that must NOT resolve (paths, emoji, unknown names), search, and the export mapping.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanGlyphNames.h>
#include <AxanLaunchEntryWire.h>

#include <string>

using namespace WEX::Logging;
namespace G = Axan::GlyphNames;

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

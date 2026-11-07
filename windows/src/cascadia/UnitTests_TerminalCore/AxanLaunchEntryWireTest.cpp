// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #33: the TOML writer half of the launch-entry wire format (src/inc/AxanLaunchEntryWire.h)
// for separator rows, including the section-header keys. Header-only, so testable here.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanLaunchEntryWire.h>
// axan #34: the reader half (FromTomlTable) needs toml++, vendored header-only under dep/.
#include "../../../dep/tomlplusplus/toml.hpp"

using namespace WEX::Logging;
namespace Wire = Axan::LaunchEntryWire;

namespace TerminalCoreUnitTests
{
    class AxanLaunchEntryWireTest
    {
        TEST_CLASS(AxanLaunchEntryWireTest);

        static Wire::Entry Separator()
        {
            Wire::Entry e;
            e.id = "sep-1";
            e.kind = std::string{ Wire::KindSeparator };
            return e;
        }

        static bool Contains(const std::string& haystack, std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        TEST_METHOD(PlainSeparatorIsUnchanged)
        {
            const auto block = Wire::ToTomlBlock(Separator());
            Log::Comment(WEX::Common::NoThrowString().Format(L"%hs", block.c_str()));
            VERIFY_IS_TRUE(Contains(block, "kind         = \"separator\""));
            VERIFY_IS_FALSE(Contains(block, "name"));
            VERIFY_IS_FALSE(Contains(block, "collapsed"));
        }

        TEST_METHOD(LabelledSeparatorWritesNameAndCollapsed)
        {
            auto e = Separator();
            e.name = "Work";
            e.collapsed = true;
            const auto block = Wire::ToTomlBlock(e);
            Log::Comment(WEX::Common::NoThrowString().Format(L"%hs", block.c_str()));
            VERIFY_IS_TRUE(Contains(block, "name         = \"Work\"\n"));
            VERIFY_IS_TRUE(Contains(block, "collapsed    = true\n"));
            Log::Comment(L"Session-only keys stay off a separator");
            VERIFY_IS_FALSE(Contains(block, "directory"));
            VERIFY_IS_FALSE(Contains(block, "command"));
        }

        TEST_METHOD(ExpandedLabelledSeparatorOmitsCollapsed)
        {
            auto e = Separator();
            e.name = "Infra";
            const auto block = Wire::ToTomlBlock(e);
            VERIFY_IS_TRUE(Contains(block, "name         = \"Infra\""));
            VERIFY_IS_FALSE(Contains(block, "collapsed"));
        }

        TEST_METHOD(LabelIsTomlQuoted)
        {
            auto e = Separator();
            e.name = "a \"b\"";
            const auto block = Wire::ToTomlBlock(e);
            VERIFY_IS_TRUE(Contains(block, "name         = \"a \\\"b\\\"\""));
        }

        TEST_METHOD(SeparatorsStillMeanVersion2)
        {
            std::vector<Wire::Entry> entries{ Separator() };
            VERIFY_ARE_EQUAL(Wire::kTomlVersionSeparators, Wire::TomlVersionFor(entries));
        }

        // ---- axan #34: the per-entry hoist override ----

        static Wire::Entry Session()
        {
            Wire::Entry e;
            e.id = "s-1";
            e.name = "build";
            return e;
        }

        // Write one entry, parse it back with toml++, and read it through FromTomlTable.
        static Wire::Entry RoundTrip(const Wire::Entry& e)
        {
            const auto block = Wire::ToTomlBlock(e);
            Log::Comment(WEX::Common::NoThrowString().Format(L"%hs", block.c_str()));
            const auto doc = toml::parse(block);
            const auto* arr = doc[Wire::TomlKey::Table].as_array();
            VERIFY_IS_NOT_NULL(arr);
            VERIFY_ARE_EQUAL(size_t{ 1 }, arr->size());
            const auto* t = (*arr)[0].as_table();
            VERIFY_IS_NOT_NULL(t);
            return Wire::FromTomlTable(*t);
        }

        TEST_METHOD(HoistDefaultIsOmitted)
        {
            const auto block = Wire::ToTomlBlock(Session());
            VERIFY_IS_FALSE(Contains(block, "hoist"));
            VERIFY_ARE_EQUAL(std::string{}, RoundTrip(Session()).hoist);
        }

        TEST_METHOD(HoistNeverIsAString)
        {
            auto e = Session();
            e.hoist = "never";
            VERIFY_IS_TRUE(Contains(Wire::ToTomlBlock(e), "hoist        = \"never\"\n"));
            VERIFY_ARE_EQUAL(std::string{ "never" }, RoundTrip(e).hoist);
        }

        TEST_METHOD(HoistListIsAnArray)
        {
            auto e = Session();
            e.hoist = "blocked,running";
            VERIFY_IS_TRUE(Contains(Wire::ToTomlBlock(e), "hoist        = [\"blocked\", \"running\"]\n"));
            VERIFY_ARE_EQUAL(std::string{ "blocked,running" }, RoundTrip(e).hoist);
        }

        TEST_METHOD(HoistReaderEdgeCases)
        {
            Log::Comment(L"An empty array reads as never");
            {
                const auto doc = toml::parse("[[startup-sessions]]\nid = \"a\"\nhoist = []\n");
                VERIFY_ARE_EQUAL(std::string{ "never" }, Wire::FromTomlTable(*doc["startup-sessions"][0].as_table()).hoist);
            }
            Log::Comment(L"Non-string array elements are skipped; names pass through raw for the caller to normalize");
            {
                const auto doc = toml::parse("[[startup-sessions]]\nid = \"a\"\nhoist = [\"error\", 3, \"bogus\"]\n");
                VERIFY_ARE_EQUAL(std::string{ "error,bogus" }, Wire::FromTomlTable(*doc["startup-sessions"][0].as_table()).hoist);
            }
        }

        TEST_METHOD(HoistIsNotWrittenOnSeparators)
        {
            auto e = Separator();
            e.hoist = "never";
            VERIFY_IS_FALSE(Contains(Wire::ToTomlBlock(e), "hoist"));
        }

        TEST_METHOD(HoistDoesNotBumpTheVersion)
        {
            auto e = Session();
            e.hoist = "running";
            std::vector<Wire::Entry> entries{ e };
            VERIFY_ARE_EQUAL(Wire::kTomlVersion, Wire::TomlVersionFor(entries));
        }

        TEST_METHOD(ProfileNameMatchesExactThenCaseInsensitive)
        {
            const std::vector<std::wstring> names{ L"PowerShell", L"Ubuntu", L"ubuntu", L"Command Prompt" };
            Log::Comment(L"#44: a name alone resolves; exact wins over a case-insensitive match");
            VERIFY_ARE_EQUAL(std::optional<size_t>{ 0 }, Wire::MatchProfileName(L"PowerShell", names));
            VERIFY_ARE_EQUAL(std::optional<size_t>{ 2 }, Wire::MatchProfileName(L"ubuntu", names));
            VERIFY_ARE_EQUAL(std::optional<size_t>{ 1 }, Wire::MatchProfileName(L"Ubuntu", names));
            Log::Comment(L"Case-insensitive fallback takes the first in list order");
            VERIFY_ARE_EQUAL(std::optional<size_t>{ 1 }, Wire::MatchProfileName(L"UBUNTU", names));
            VERIFY_ARE_EQUAL(std::optional<size_t>{ 3 }, Wire::MatchProfileName(L"command prompt", names));
            Log::Comment(L"An unknown or empty name does not match (the importer falls back to the default profile)");
            VERIFY_IS_FALSE(Wire::MatchProfileName(L"Fish", names).has_value());
            VERIFY_IS_FALSE(Wire::MatchProfileName(L"Power Shell", names).has_value());
            VERIFY_IS_FALSE(Wire::MatchProfileName(L"", names).has_value());
            VERIFY_IS_FALSE(Wire::MatchProfileName(L"Ubuntu", {}).has_value());
        }

        TEST_METHOD(CustomHexColorRoundTrips)
        {
            Log::Comment(L"axan #50: a custom colour is stored as \"#rrggbb\" and survives TOML unchanged");
            auto e = Session();
            e.color = "#2e8b57";
            e.colorTarget = "icon";
            VERIFY_IS_TRUE(Contains(Wire::ToTomlBlock(e), "\"#2e8b57\""));
            const auto back = RoundTrip(e);
            VERIFY_ARE_EQUAL(std::string{ "#2e8b57" }, back.color);
            VERIFY_ARE_EQUAL(std::string{ "icon" }, back.colorTarget);
            Log::Comment(L"A palette name is untouched too");
            e.color = "blue";
            VERIFY_ARE_EQUAL(std::string{ "blue" }, RoundTrip(e).color);
        }

        TEST_METHOD(ProfileNameAloneIsRead)
        {
            const auto doc = toml::parse("[[startup-sessions]]\nid = \"a\"\nprofile-name = \"Ubuntu\"\n");
            const auto e = Wire::FromTomlTable(*doc["startup-sessions"][0].as_table());
            VERIFY_ARE_EQUAL(std::string{}, e.profile);
            VERIFY_ARE_EQUAL(std::string{ "Ubuntu" }, e.profileName);
        }

        TEST_METHOD(CheckTomlTableFlagsUnknownKeysAndWrongTypes)
        {
            Log::Comment(L"#44: a well-formed session and separator row have no issues");
            {
                const auto doc = toml::parse("[[startup-sessions]]\nid = \"a\"\nprofile-name = \"Ubuntu\"\nicon = \"U+131A3\"\n"
                                             "color = \"#ff0000\"\nhoist = [\"error\"]\n"
                                             "[[startup-sessions]]\nid = \"s\"\nkind = \"separator\"\nheight = 2\ncollapsed = true\n"
                                             "[[startup-sessions]]\nid = \"t\"\nkind = \"separator\"\nheight = 0.5\nhoist = \"never\"\n");
                for (size_t i = 0; i < 3; ++i)
                {
                    VERIFY_ARE_EQUAL(size_t{ 0 }, Wire::CheckTomlTable(*doc["startup-sessions"][i].as_table()).size());
                }
            }
            Log::Comment(L"An unknown key and each wrongly typed value is one issue");
            {
                const auto doc = toml::parse("[[startup-sessions]]\nid = \"a\"\ncolour = \"red\"\nname = 3\nheight = \"2\"\n"
                                             "collapsed = \"yes\"\nhoist = [\"error\", 3]\nicon = \"robot\"\n");
                const auto issues = Wire::CheckTomlTable(*doc["startup-sessions"][0].as_table());
                VERIFY_ARE_EQUAL(size_t{ 5 }, issues.size());
                const auto has = [&](std::string_view key, std::string_view problem) {
                    return std::any_of(issues.begin(), issues.end(), [&](const Wire::TomlKeyIssue& i) { return i.key == key && i.problem == problem; });
                };
                VERIFY_IS_TRUE(has("colour", "unknown key"));
                VERIFY_IS_TRUE(has("name", "expected a string"));
                VERIFY_IS_TRUE(has("height", "expected a number"));
                VERIFY_IS_TRUE(has("collapsed", "expected a boolean"));
                VERIFY_IS_TRUE(has("hoist", "expected a string or an array of strings"));
            }
        }
    };
}

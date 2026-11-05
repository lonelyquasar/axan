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
    };
}

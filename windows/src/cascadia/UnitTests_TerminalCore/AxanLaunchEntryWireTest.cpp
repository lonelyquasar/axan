// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #33: the TOML writer half of the launch-entry wire format (src/inc/AxanLaunchEntryWire.h)
// for separator rows, including the section-header keys. Header-only, so testable here.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanLaunchEntryWire.h>

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
    };
}

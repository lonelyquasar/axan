// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #50: the identity editor's custom colour box parser (src/inc/AxanSessionPalette.h
// ParseCustomColor). Every accepted form canonicalises to lowercase "#rrggbb", which
// ParseHexColor reads back unchanged.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanSessionPalette.h>

#include <optional>
#include <string>

using namespace WEX::Logging;
namespace P = Axan::SessionPalette;

namespace TerminalCoreUnitTests
{
    class AxanSessionPaletteTest
    {
        TEST_CLASS(AxanSessionPaletteTest);

        static void Accepts(std::wstring_view input, const wchar_t* expected)
        {
            const auto r = P::ParseCustomColor(input);
            VERIFY_IS_TRUE(r.has_value(), std::wstring{ input }.c_str());
            VERIFY_ARE_EQUAL(std::wstring{ expected }, *r);
        }

        static void Rejects(std::wstring_view input)
        {
            VERIFY_IS_FALSE(P::ParseCustomColor(input).has_value(), std::wstring{ input }.c_str());
        }

        TEST_METHOD(SixDigitHexWithAndWithoutHash)
        {
            Accepts(L"#2E8B57", L"#2e8b57");
            Accepts(L"#2e8b57", L"#2e8b57");
            Accepts(L"2E8B57", L"#2e8b57");
            Accepts(L"000000", L"#000000");
            Accepts(L"#FFFFFF", L"#ffffff");
        }

        TEST_METHOD(ThreeDigitHexDoublesEachDigit)
        {
            Accepts(L"#abc", L"#aabbcc");
            Accepts(L"#F0A", L"#ff00aa");
            Log::Comment(L"A bare three-digit value is too easily a word; it needs the hash");
            Rejects(L"abc");
            Rejects(L"fed");
        }

        TEST_METHOD(RgbFunctionWithSpaces)
        {
            Accepts(L"rgb(46, 139, 87)", L"#2e8b57");
            Accepts(L"rgb(46,139,87)", L"#2e8b57");
            Accepts(L"  rgb( 0 , 0 , 0 )  ", L"#000000");
            Accepts(L"RGB(255, 255, 255)", L"#ffffff");
            Accepts(L"rgb(255,0,10)", L"#ff000a");
        }

        TEST_METHOD(OuterWhitespaceIsTrimmed)
        {
            Accepts(L"  #12bbff\t", L"#12bbff");
        }

        TEST_METHOD(OutOfRangeAndMalformedRgbAreRejected)
        {
            Rejects(L"rgb(256, 0, 0)");
            Rejects(L"rgb(0, 0, 1000)");
            Rejects(L"rgb(-1, 0, 0)");
            Rejects(L"rgb(0, 0)");
            Rejects(L"rgb(0, 0, 0, 0)");
            Rejects(L"rgb(0, 0, 0");
            Rejects(L"rgb(50%, 0, 0)");
            Rejects(L"rgb(1.5, 0, 0)");
            Rejects(L"rgb(, 0, 0)");
            Rejects(L"rgb()");
            Rejects(L"rgba(0, 0, 0, 1)");
        }

        TEST_METHOD(GarbageAndEmptyAreRejected)
        {
            Rejects(L"");
            Rejects(L"   ");
            Rejects(L"#");
            Rejects(L"blue");
            Rejects(L"red");
            Rejects(L"#12345");
            Rejects(L"#1234567");
            Rejects(L"#gg0000");
            Rejects(L"12 34 56");
            Log::Comment(L"#AARRGGBB still renders when stored, but the box doesn't produce it");
            Rejects(L"#ff2e8b57");
        }

        TEST_METHOD(CanonicalFormRoundTripsThroughParseHexColor)
        {
            const auto stored = P::ParseCustomColor(L"rgb(46, 139, 87)");
            VERIFY_IS_TRUE(stored.has_value());
            const auto c = P::ParseHexColor(*stored);
            VERIFY_IS_TRUE(c.has_value());
            VERIFY_ARE_EQUAL(0xFF, static_cast<int>(c->a));
            VERIFY_ARE_EQUAL(46, static_cast<int>(c->r));
            VERIFY_ARE_EQUAL(139, static_cast<int>(c->g));
            VERIFY_ARE_EQUAL(87, static_cast<int>(c->b));
            Log::Comment(L"A stored custom value resolves verbatim in either theme");
            VERIFY_ARE_EQUAL(*stored, std::wstring{ P::ResolveToken(*stored, true) });
            VERIFY_ARE_EQUAL(*stored, std::wstring{ P::ResolveToken(*stored, false) });
            Log::Comment(L"Re-parsing the canonical form is a fixed point");
            Accepts(*stored, stored->c_str());
        }
    };
}

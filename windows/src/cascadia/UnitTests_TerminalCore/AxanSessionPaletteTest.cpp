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

        // Rejected with exactly this inline hint.
        static void Hint(std::wstring_view input, const wchar_t* expected)
        {
            const auto r = P::ParseCustomColorDetailed(input);
            VERIFY_IS_FALSE(r.color.has_value(), std::wstring{ input }.c_str());
            VERIFY_ARE_EQUAL(std::wstring{ expected }, r.error, std::wstring{ input }.c_str());
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
            Accepts(L"rgb (254,200,144)", L"#fec890");
            Accepts(L"rgb(254 200 144)", L"#fec890");
        }

        TEST_METHOD(ExactUatInputsAreAccepted)
        {
            Log::Comment(L"The three strings typed in UAT that the first cut rejected");
            Accepts(L"255,200,144", L"#ffc890");
            Accepts(L"254, 200, 144", L"#fec890");
            Accepts(L"(254, 200, 144)", L"#fec890");
        }

        TEST_METHOD(BareTripletWithAnySeparators)
        {
            Accepts(L"254 200 144", L"#fec890");
            Accepts(L"254,200 144", L"#fec890");
            Accepts(L"254 ,200,  144", L"#fec890");
            Accepts(L"254\t200\t144", L"#fec890");
            Accepts(L"12 34 56", L"#0c2238");
            Accepts(L"0,0,0", L"#000000");
        }

        TEST_METHOD(ParenthesisedTripletWithOrWithoutKeyword)
        {
            Accepts(L"(254,200,144)", L"#fec890");
            Accepts(L"( 254 200 144 )", L"#fec890");
            Accepts(L"RGB(254, 200, 144)", L"#fec890");
            Accepts(L"Rgb ( 254 , 200 , 144 )", L"#fec890");
        }

        TEST_METHOD(RgbaAlphaIsIgnored)
        {
            Accepts(L"rgba(254, 200, 144, 0.5)", L"#fec890");
            Accepts(L"rgba(254,200,144,1)", L"#fec890");
            Accepts(L"RGBA(254, 200, 144, .25)", L"#fec890");
            Accepts(L"rgba(254, 200, 144)", L"#fec890");
            Accepts(L"rgb(254 200 144 / 50%)", L"#fec890");
            Accepts(L"rgb(254 200 144 0.5)", L"#fec890");
            Accepts(L"rgba(0, 0, 0, 0)", L"#000000");
            Log::Comment(L"The alpha must still look like a number");
            Rejects(L"rgba(254, 200, 144, x)");
            Rejects(L"rgba(254, 200, 144, 0.5.5)");
            Rejects(L"rgba(254, 200, 144, 1, 1)");
            Log::Comment(L"No alpha without a keyword: a bare or parenthesised quad is rejected");
            Rejects(L"254, 200, 144, 1");
            Rejects(L"(254, 200, 144, 1)");
        }

        TEST_METHOD(OuterWhitespaceAndTrailingPunctuationAreIgnored)
        {
            Accepts(L"  #12bbff\t", L"#12bbff");
            Accepts(L"#12bbff;", L"#12bbff");
            Accepts(L"254, 200, 144.", L"#fec890");
            Accepts(L"  rgb(254, 200, 144) ; ", L"#fec890");
            Accepts(L"rgba(254, 200, 144, 0.5);", L"#fec890");
            Rejects(L";");
            Rejects(L"#12bbff;;");
        }

        TEST_METHOD(OutOfRangeAndMalformedRgbAreRejected)
        {
            Rejects(L"rgb(256, 0, 0)");
            Rejects(L"rgb(0, 0, 1000)");
            Rejects(L"rgb(-1, 0, 0)");
            Rejects(L"rgb(0, 0)");
            Rejects(L"rgb(0, 0, 0");
            Rejects(L"rgb(50%, 0, 0)");
            Rejects(L"rgb(1.5, 0, 0)");
            Rejects(L"rgb(, 0, 0)");
            Rejects(L"rgb()");
            Rejects(L"rgb 254, 200, 144");
            Rejects(L"rgb(254,,200,144)");
            Log::Comment(L"The same limits hold for the bare and parenthesised forms");
            Rejects(L"256, 0, 0");
            Rejects(L"255, 200");
            Rejects(L"1.5, 2, 3");
            Rejects(L"-1 2 3");
            Rejects(L"(254, 200, 144");
            Rejects(L"254, 200, 144)");
            Rejects(L"0256 0 0");
            Log::Comment(L"Leading zeros are fine while the value stays in range");
            Accepts(L"0255 0 0", L"#ff0000");
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
            Rejects(L"#abc def");
            Log::Comment(L"#AARRGGBB still renders when stored, but the box doesn't produce it");
            Rejects(L"#ff2e8b57");
        }

        TEST_METHOD(NoSpaceTripletsParse)
        {
            Accepts(L"255,200,144", L"#ffc890");
            Accepts(L"rgb(255,200,144)", L"#ffc890");
            Accepts(L"(0,0,0)", L"#000000");
            Accepts(L"rgba(255,200,144,0.5)", L"#ffc890");
        }

        TEST_METHOD(FailureHintsNameTheProblem)
        {
            Log::Comment(L"Out of range or negative: the channel by position, the value as typed");
            Hint(L"255, 200, 400", L"blue cannot be 400");
            Hint(L"300,0,0", L"red cannot be 300");
            Hint(L"0 -1 0", L"green cannot be -1");
            Hint(L"rgb(0, 0, 999999999999)", L"blue cannot be 99999999\x2026");
            Log::Comment(L"The first bad channel wins");
            Hint(L"300, 400, 500", L"red cannot be 300");
            Log::Comment(L"Non-integer");
            Hint(L"0, 1.5, 0", L"green must be a whole number");
            Hint(L"rgb(0.5, 0, 0)", L"red must be a whole number");
            Hint(L"0, 0, 2.5", L"blue must be a whole number");
            Log::Comment(L"A single trailing '.' is sentence punctuation, not a decimal point");
            Accepts(L"0, 0, 2.", L"#000002");
            Log::Comment(L"Wrong count (rgba's ignored fourth is fine; a fifth is not)");
            Hint(L"255, 200", L"Use three numbers: r, g, b");
            Hint(L"1, 2, 3, 4", L"Use three numbers: r, g, b");
            Hint(L"(1, 2, 3, 4)", L"Use three numbers: r, g, b");
            Hint(L"rgba(1, 2, 3, 0.5, 1)", L"Use three numbers: r, g, b");
            Hint(L"rgb(7)", L"Use three numbers: r, g, b");
            Log::Comment(L"Bad hex length or digit");
            Hint(L"#12345", L"Hex colors are #RRGGBB");
            Hint(L"#gg0000", L"Hex colors are #RRGGBB");
            Hint(L"#ff2e8b57", L"Hex colors are #RRGGBB");
            Hint(L"abc", L"Hex colors are #RRGGBB");
            Hint(L"12345", L"Hex colors are #RRGGBB");
            Log::Comment(L"Anything else");
            Hint(L"blue", L"Use #RRGGBB or r, g, b");
            Hint(L"", L"Use #RRGGBB or r, g, b");
            Hint(L"rgb 1 2 3", L"Use #RRGGBB or r, g, b");
            Hint(L"(1, 2, 3", L"Use #RRGGBB or r, g, b");
            Hint(L"1,,2,3", L"Use #RRGGBB or r, g, b");
            Hint(L"red, 0, 0", L"Use #RRGGBB or r, g, b");
            Hint(L"rgba(1, 2, 3, x)", L"Use #RRGGBB or r, g, b");
            Log::Comment(L"Success carries no hint");
            VERIFY_ARE_EQUAL(std::wstring{}, P::ParseCustomColorDetailed(L"255,200,144").error);
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

// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan: the per-font glyph scale (src/inc/AxanGlyphScale.h): which characters get scaled up,
// surrogate-pair decoding, and the 1.0 default for everything else.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanGlyphScale.h>

using namespace WEX::Logging;
namespace S = Axan::GlyphScale;

namespace TerminalCoreUnitTests
{
    class AxanGlyphScaleTest
    {
        TEST_CLASS(AxanGlyphScaleTest);

        TEST_METHOD(ReferenceFontsAreOne)
        {
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"Segoe Fluent Icons"), 1.0);
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"Segoe UI Emoji"), 1.0);
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"Segoe UI"), 1.0);
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"No Such Font"), 1.0);
            VERIFY_IS_TRUE(S::FontScaleFor(L"Segoe UI Historic") > 1.0);
        }

        TEST_METHOD(HieroglyphsAndRunesScaleUp)
        {
            const auto historic = S::FontScaleFor(L"Segoe UI Historic");
            Log::Comment(L"U+131A3 (an Egyptian hieroglyph) as a surrogate pair");
            VERIFY_ARE_EQUAL(S::FirstCodepoint(L"\xD80C\xDDA3"), static_cast<char32_t>(0x131A3));
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD80C\xDDA3"), historic);
            Log::Comment(L"U+16A0 RUNIC LETTER FEHU and U+16FF, the end of the block");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x16A0"), historic);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x16FF"), historic);
        }

        TEST_METHOD(EverythingElseIsOne)
        {
            VERIFY_ARE_EQUAL(S::ScaleFor(L""), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"a"), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"robot"), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"C:\\icons\\x.png"), 1.0);
            Log::Comment(L"A Segoe Fluent private-use glyph");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xE99A"), 1.0);
            Log::Comment(L"U+1F680 ROCKET, an emoji");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD83D\xDE80"), 1.0);
            Log::Comment(L"Just outside the Runic block");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x169F"), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x1700"), 1.0);
        }

        TEST_METHOD(LoneSurrogatesAreOne)
        {
            VERIFY_ARE_EQUAL(S::FirstCodepoint(L"\xD80C"), static_cast<char32_t>(0));
            VERIFY_ARE_EQUAL(S::FirstCodepoint(L"\xDDA3"), static_cast<char32_t>(0));
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD80Cx"), 1.0);
        }
    };
}

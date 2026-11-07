// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan: the per-font glyph scale (src/inc/AxanGlyphScale.h): which characters get scaled up,
// surrogate-pair decoding, and the 1.0 default for everything else.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanGlyphScale.h>
#include <AxanGlyphWeight.h> // axan #48

#include <limits>
#include <map>
#include <optional>

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
            Log::Comment(L"The other Segoe UI Historic scripts: U+1681 OGHAM BEITH, U+2C00 GLAGOLITIC AZU, U+1202D CUNEIFORM DINGIR, U+10330 GOTHIC AHSA");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x1681"), historic);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x2C00"), historic);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD808\xDC2D"), historic);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD800\xDF30"), historic);
        }

        TEST_METHOD(EverythingElseIsOne)
        {
            VERIFY_ARE_EQUAL(S::ScaleFor(L""), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"a"), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"robot"), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"C:\\icons\\x.png"), 1.0);
            Log::Comment(L"A Segoe Fluent private-use glyph");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xE99A"), 1.0);
            VERIFY_ARE_EQUAL(std::wstring{ S::FontForGlyph(L"\xE99A") }, std::wstring{ L"Segoe Fluent Icons" });
            Log::Comment(L"U+1F680 ROCKET, an emoji");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD83D\xDE80"), 1.0);
            Log::Comment(L"Just outside the Ogham + Runic run (U+1680-16FF)");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x167F"), 1.0);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x1700"), 1.0);
        }

        TEST_METHOD(OverridesWinAndAreClamped)
        {
            // A lookup standing in for sidebar.iconFontScale.
            const auto lookup = [](std::wstring_view font) -> std::optional<double> {
                if (font == L"Segoe UI Historic")
                {
                    return 1.5;
                }
                if (font == L"Segoe UI Emoji")
                {
                    return 0.1; // below the floor
                }
                if (font == L"Segoe Fluent Icons")
                {
                    return 9.0; // above the ceiling
                }
                if (font == L"Segoe UI")
                {
                    return std::numeric_limits<double>::quiet_NaN();
                }
                return std::nullopt;
            };
            Log::Comment(L"A user value replaces the built-in one");
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"Segoe UI Historic", lookup), 1.5);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD80C\xDDA3", lookup), 1.5);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x16A0", lookup), 1.5);
            Log::Comment(L"Out-of-range values clamp to [0.5, 3]; NaN reads as 1");
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD83D\xDE80", lookup), S::kMinScale);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xE99A", lookup), S::kMaxScale);
            VERIFY_ARE_EQUAL(S::ScaleFor(L"a", lookup), 1.0);
            Log::Comment(L"No user value: the built-in table");
            const auto none = [](std::wstring_view) -> std::optional<double> { return std::nullopt; };
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD80C\xDDA3", none), 1.35);
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"No Such Font", none), 1.0);
            VERIFY_ARE_EQUAL(S::ClampScale(0.5), 0.5);
            VERIFY_ARE_EQUAL(S::ClampScale(3.0), 3.0);
            VERIFY_ARE_EQUAL(S::ClampScale(1.25), 1.25);
        }

        TEST_METHOD(FamilyNamesAreCaseInsensitive)
        {
            Log::Comment(L"A sidebar.iconFontScale key in any case is the built-in family's row");
            VERIFY_ARE_EQUAL(std::wstring{ S::KnownFamily(L"segoe ui historic") }, std::wstring{ L"Segoe UI Historic" });
            VERIFY_ARE_EQUAL(std::wstring{ S::CanonicalFamily(L"SEGOE FLUENT ICONS") }, std::wstring{ L"Segoe Fluent Icons" });
            VERIFY_ARE_EQUAL(S::FontScaleFor(L"segoe ui historic"), S::FontScaleFor(L"Segoe UI Historic"));
            Log::Comment(L"An unknown family keeps its spelling");
            VERIFY_IS_TRUE(S::KnownFamily(L"Noto Sans Egyptian Hieroglyphs").empty());
            VERIFY_ARE_EQUAL(std::wstring{ S::CanonicalFamily(L"Noto Sans Egyptian Hieroglyphs") }, std::wstring{ L"Noto Sans Egyptian Hieroglyphs" });
            Log::Comment(L"A FamilyLess map finds a key whatever case it was written in");
            std::map<std::wstring, double, S::FamilyLess> scales;
            scales.emplace(L"segoe ui historic", 2.0);
            VERIFY_IS_FALSE(scales.emplace(L"Segoe UI Historic", 1.0).second);
            const auto lookup = [&](std::wstring_view font) -> std::optional<double> {
                if (const auto it = scales.find(font); it != scales.end())
                {
                    return it->second;
                }
                return std::nullopt;
            };
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\x16A0", lookup), 2.0);
            VERIFY_IS_TRUE(S::SameFamily(L"Segoe UI", L"segoe ui"));
            VERIFY_IS_FALSE(S::SameFamily(L"Segoe UI", L"Segoe UI Emoji"));
        }

        TEST_METHOD(FamilyForIcon)
        {
            Log::Comment(L"Glyph names, builtin aliases and private-use glyphs: Segoe Fluent Icons");
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"robot"), std::wstring{ L"Segoe Fluent Icons" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"git"), std::wstring{ L"Segoe Fluent Icons" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\xE756"), std::wstring{ L"Segoe Fluent Icons" });
            Log::Comment(L"Emoji, with and without a presentation selector or joiners");
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\xD83D\xDE80"), std::wstring{ L"Segoe UI Emoji" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\x2764\xFE0F"), std::wstring{ L"Segoe UI Emoji" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\x2122\xFE0F"), std::wstring{ L"Segoe UI Emoji" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\xD83D\xDC68\x200D\xD83D\xDCBB"), std::wstring{ L"Segoe UI Emoji" });
            Log::Comment(L"Hieroglyphs (literal and as code-point text) and runes: Segoe UI Historic");
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\xD80C\xDDA3"), std::wstring{ L"Segoe UI Historic" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"U+131A3"), std::wstring{ L"Segoe UI Historic" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\x16A0"), std::wstring{ L"Segoe UI Historic" });
            Log::Comment(L"Other single characters: Segoe UI");
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"a"), std::wstring{ L"Segoe UI" });
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\x03BB"), std::wstring{ L"Segoe UI" });
            Log::Comment(L"Image paths, unknown names and empty: none");
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L""), std::wstring{});
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"C:\\icons\\x.png"), std::wstring{});
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"ms-appx:///ProfileIcons/pwsh.png"), std::wstring{});
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"no-such-glyph-name"), std::wstring{});
            VERIFY_ARE_EQUAL(S::FamilyForIcon(L"\x00E9.png"), std::wstring{});
        }

        TEST_METHOD(LoneSurrogatesAreOne)
        {
            VERIFY_ARE_EQUAL(S::FirstCodepoint(L"\xD80C"), static_cast<char32_t>(0));
            VERIFY_ARE_EQUAL(S::FirstCodepoint(L"\xDDA3"), static_cast<char32_t>(0));
            VERIFY_ARE_EQUAL(S::ScaleFor(L"\xD80Cx"), 1.0);
        }

        // axan #48: sidebar.iconFontWeight values (src/inc/AxanGlyphWeight.h).
        TEST_METHOD(WeightNamesParse)
        {
            namespace W = Axan::GlyphWeight;
            VERIFY_ARE_EQUAL(W::ParseWeight(L"normal").value_or(uint16_t{ 0 }), W::kNormal);
            VERIFY_ARE_EQUAL(W::ParseWeight(L"semibold").value_or(uint16_t{ 0 }), W::kSemiBold);
            VERIFY_ARE_EQUAL(W::ParseWeight(L"bold").value_or(uint16_t{ 0 }), W::kBold);
            VERIFY_ARE_EQUAL(W::kNormal, static_cast<uint16_t>(400));
            VERIFY_ARE_EQUAL(W::kSemiBold, static_cast<uint16_t>(600));
            VERIFY_ARE_EQUAL(W::kBold, static_cast<uint16_t>(700));

            Log::Comment(L"Case-insensitive; a space, '-' or '_' inside the name is ignored");
            VERIFY_ARE_EQUAL(W::ParseWeight(L"BOLD").value_or(uint16_t{ 0 }), W::kBold);
            VERIFY_ARE_EQUAL(W::ParseWeight(L"Semi-Bold").value_or(uint16_t{ 0 }), W::kSemiBold);
            VERIFY_ARE_EQUAL(W::ParseWeight(L"semi bold").value_or(uint16_t{ 0 }), W::kSemiBold);
            VERIFY_ARE_EQUAL(W::ParseWeight(L"SEMI_BOLD").value_or(uint16_t{ 0 }), W::kSemiBold);

            Log::Comment(L"Anything else is not a weight");
            VERIFY_IS_FALSE(W::ParseWeight(L"").has_value());
            VERIFY_IS_FALSE(W::ParseWeight(L"-").has_value());
            VERIFY_IS_FALSE(W::ParseWeight(L"700").has_value());
            VERIFY_IS_FALSE(W::ParseWeight(L"bolder").has_value());
            VERIFY_IS_FALSE(W::ParseWeight(L"bol").has_value());
            VERIFY_IS_FALSE(W::ParseWeight(L"light").has_value());
            VERIFY_IS_FALSE(W::ParseWeight(L"b\x00F6ld").has_value());
        }

        TEST_METHOD(WeightNamesCanonicalise)
        {
            namespace W = Axan::GlyphWeight;
            VERIFY_ARE_EQUAL(std::wstring{ W::CanonicalWeightName(L"Semi-Bold") }, std::wstring{ L"semibold" });
            VERIFY_ARE_EQUAL(std::wstring{ W::CanonicalWeightName(L"BOLD") }, std::wstring{ L"bold" });
            VERIFY_ARE_EQUAL(std::wstring{ W::CanonicalWeightName(L"Normal") }, std::wstring{ L"normal" });
            VERIFY_ARE_EQUAL(std::wstring{ W::CanonicalWeightName(L"heavy") }, std::wstring{});
            Log::Comment(L"WeightName round-trips the three weights; any other value reads as normal");
            VERIFY_ARE_EQUAL(std::wstring{ W::WeightName(W::kBold) }, std::wstring{ L"bold" });
            VERIFY_ARE_EQUAL(std::wstring{ W::WeightName(W::kSemiBold) }, std::wstring{ L"semibold" });
            VERIFY_ARE_EQUAL(std::wstring{ W::WeightName(W::kNormal) }, std::wstring{ L"normal" });
            VERIFY_ARE_EQUAL(std::wstring{ W::WeightName(900) }, std::wstring{ L"normal" });
            static_assert(W::ParseWeight(L"bold").value() == W::kBold, "ParseWeight is constexpr");
        }
    };
}

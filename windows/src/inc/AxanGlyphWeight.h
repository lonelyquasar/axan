// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanGlyphWeight — a per-font weight for single-character icons (axan #48).
//
// sidebar.iconFontWeight ({ "Segoe UI Historic": "bold" }) names the weight a font family's
// glyph icons are drawn at, next to sidebar.iconFontScale (AxanGlyphScale.h). Thin fallback
// fonts read poorly when scaled up: Segoe UI Historic ships only a Regular face, and asking
// for SemiBold or Bold makes DirectWrite synthesise an emboldened one, which is enough to make
// its hieroglyphs legible. A family absent from the map draws at Normal.
//
// The values are names, not numbers: "normal", "semibold", "bold" (case-insensitive; also
// "semi-bold" / "semi bold"). ParseWeight maps a name to the DirectWrite/XAML weight value
// (400 / 600 / 700); an unknown name is nullopt and the caller ignores the entry. WeightName
// gives the canonical spelling the settings editor writes back.
//
// Pure (no WinRT) and header-only in the shared src/inc, like AxanGlyphScale.h, so the icon
// converter, the settings editor and the unit tests share it.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Axan::GlyphWeight
{
    // The weights offered, as DirectWrite / Windows.UI.Text.FontWeight values.
    inline constexpr uint16_t kNormal = 400;
    inline constexpr uint16_t kSemiBold = 600;
    inline constexpr uint16_t kBold = 700;

    // The canonical names, as stored in settings.
    inline constexpr std::wstring_view kNormalName{ L"normal" };
    inline constexpr std::wstring_view kSemiBoldName{ L"semibold" };
    inline constexpr std::wstring_view kBoldName{ L"bold" };

    // ASCII-only case fold; a space, '-' or '_' is dropped so "Semi-Bold" reads as "semibold".
    constexpr bool SameWeightName(std::wstring_view text, std::wstring_view canonical) noexcept
    {
        size_t j = 0;
        for (const auto raw : text)
        {
            if (raw == L' ' || raw == L'-' || raw == L'_')
            {
                continue;
            }
            const auto ch = (raw >= L'A' && raw <= L'Z') ? static_cast<wchar_t>(raw - L'A' + L'a') : raw;
            if (j >= canonical.size() || ch != canonical[j])
            {
                return false;
            }
            ++j;
        }
        return j == canonical.size();
    }

    // The weight a sidebar.iconFontWeight value names; nullopt for anything else (an empty
    // string included).
    constexpr std::optional<uint16_t> ParseWeight(std::wstring_view name) noexcept
    {
        if (SameWeightName(name, kNormalName))
        {
            return kNormal;
        }
        if (SameWeightName(name, kSemiBoldName))
        {
            return kSemiBold;
        }
        if (SameWeightName(name, kBoldName))
        {
            return kBold;
        }
        return std::nullopt;
    }

    // The canonical name of a weight; anything that is not SemiBold or Bold reads as "normal".
    constexpr std::wstring_view WeightName(uint16_t weight) noexcept
    {
        switch (weight)
        {
        case kSemiBold:
            return kSemiBoldName;
        case kBold:
            return kBoldName;
        default:
            return kNormalName;
        }
    }

    // The canonical spelling of a value ("Semi-Bold" -> "semibold"), "" when it names no weight.
    constexpr std::wstring_view CanonicalWeightName(std::wstring_view name) noexcept
    {
        const auto weight = ParseWeight(name);
        return weight ? WeightName(*weight) : std::wstring_view{};
    }
}

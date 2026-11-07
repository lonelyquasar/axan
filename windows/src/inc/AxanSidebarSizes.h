// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanSidebarSizes: the ranges and defaults of the sidebar row size settings, and the one
// rule both the Settings UI and the live sidebar apply to a stored value.
//
// Pure (no WinRT) and header-only in the shared src/inc, so the settings editor, TerminalApp
// and the unit tests share it.

#pragma once

namespace Axan::SidebarSizes
{
    // sidebar.labelFontSize, stateLineFontSize, iconSize, stateGlyphSize; the defaults match
    // MTSMSettings.h.
    struct SizeRange
    {
        double fallback; // the setting's default
        double min;
        double max;
    };

    // The settings page's NumberBox ranges.
    inline constexpr SizeRange kLabelFontSize{ 14.0, 8.0, 32.0 };
    inline constexpr SizeRange kStateLineFontSize{ 11.0, 8.0, 32.0 };
    inline constexpr SizeRange kIconSize{ 16.0, 8.0, 48.0 };
    inline constexpr SizeRange kStateGlyphSize{ 12.0, 8.0, 48.0 };
    // axan #47: sidebar.idleLabelFontSize and sidebar.idleRowHeight (px), the absolute sizes
    // the "smaller" idle effect gives a row.
    inline constexpr SizeRange kIdleLabelFontSize{ 12.0, 8.0, 32.0 };
    inline constexpr SizeRange kIdleRowHeight{ 24.0, 16.0, 64.0 };

    // A size as the row may use it: NaN (a cleared NumberBox, or JSON null) reads as the
    // default, then the value is clamped to the range. std::clamp alone passes NaN through,
    // and a NaN FontSize throws in XAML.
    constexpr double SanitizeSize(double value, const SizeRange& range) noexcept
    {
        if (!(value == value))
        {
            return range.fallback;
        }
        return value < range.min ? range.min : (value > range.max ? range.max : value);
    }
}

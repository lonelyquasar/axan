// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanCheckBoxWrap — a VariableSizedWrapGrid of CheckBoxes whose labels show in full.
//
// VariableSizedWrapGrid gives every cell the size of its FIRST child unless ItemWidth is
// set, so a row of state checkboxes led by a short label ("idle") clipped every longer one
// to a stub ("needs permission" -> "nee"). FitItemWidth sizes the cells to the widest label
// instead: the boxes then sit in equal columns and wrap onto as many rows as the width
// needs. Used by the hoist-state checkboxes on the Settings Sidebar page and the Startup
// sessions page's Hoisted group; the sidebar's node card can use it too.
//
// Code-built UI only: needs the Windows.UI.Xaml projection (Controls, Media) the including
// project's pch provides.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace Axan::Ui::CheckBoxWrap
{
    // The CheckBox template's box (20) and the gap before its label (the default Padding's
    // left 8), plus the space kept between columns.
    inline constexpr double kBoxAndPadding = 28;
    inline constexpr double kColumnGap = 16;

    // Call after the boxes are in panel.Children() with their final string Content. Each
    // label is measured as a TextBlock in the box's font size (text scaling applies), so the
    // widest one fits whatever the display scale.
    inline void FitItemWidth(const winrt::Windows::UI::Xaml::Controls::VariableSizedWrapGrid& panel)
    {
        namespace Controls = winrt::Windows::UI::Xaml::Controls;
        if (!panel)
        {
            return;
        }
        double widest = 0;
        for (const auto& child : panel.Children())
        {
            const auto box = child.try_as<Controls::CheckBox>();
            if (!box)
            {
                continue;
            }
            Controls::TextBlock probe;
            probe.Text(winrt::unbox_value_or<winrt::hstring>(box.Content(), winrt::hstring{}));
            probe.FontSize(box.FontSize());
            constexpr auto inf = std::numeric_limits<float>::infinity();
            probe.Measure(winrt::Windows::Foundation::Size{ inf, inf });
            widest = std::max(widest, static_cast<double>(probe.DesiredSize().Width));
            // The cell carries the gap; a box margin would only eat into it.
            box.Margin(winrt::Windows::UI::Xaml::Thickness{ 0, 0, 0, 0 });
            box.MinWidth(0);
        }
        if (widest > 0)
        {
            panel.ItemWidth(std::ceil(widest + kBoxAndPadding + kColumnGap));
        }
    }
}

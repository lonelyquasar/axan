// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanCheckBoxWrap — a VariableSizedWrapGrid of CheckBoxes whose labels show in full.
//
// VariableSizedWrapGrid gives every cell the size of its FIRST child unless ItemWidth is
// set, so a row of state checkboxes led by a short label ("idle") clipped every longer one
// to a stub ("needs permission" -> "nee"). FitItemWidth sizes the cells to the widest label
// instead: the boxes then sit in equal columns and wrap onto as many rows as the width
// needs. BuildNameBoxes builds such a row from a name vocabulary: used by the Settings Sidebar
// page (hoist states, idle effects) and the Startup sessions page's Hoisted group; the
// sidebar's node card uses FitItemWidth directly.
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

    // Fills `panel` (cleared first) with one CheckBox per name, in the order of `names`, then
    // runs FitItemWidth. Per name:
    //   label(name)          the box's text (anything winrt::hstring is constructible from);
    //   setup(box, name)     the site's extras before the box is appended: initial
    //                        IsChecked, tooltip, automation help, Tag, ... (may do nothing);
    //   onToggle(key, on)    on Click, i.e. for the user only, never for a programmatic
    //                        IsChecked; key is the name as an hstring, on the new state.
    // onToggle is copied into every box's handler, so capture weak references in it.
    template<typename Names, typename Label, typename Setup, typename OnToggle>
    void BuildNameBoxes(const winrt::Windows::UI::Xaml::Controls::VariableSizedWrapGrid& panel,
                        const Names& names,
                        Label&& label,
                        Setup&& setup,
                        const OnToggle& onToggle)
    {
        namespace Controls = winrt::Windows::UI::Xaml::Controls;
        if (!panel)
        {
            return;
        }
        panel.Children().Clear();
        for (const auto& name : names)
        {
            Controls::CheckBox box;
            const winrt::hstring key{ name };
            box.Content(winrt::box_value(winrt::hstring{ label(name) }));
            setup(box, name);
            box.Click([onToggle, key](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                if (const auto b = sender.try_as<Controls::CheckBox>())
                {
                    onToggle(key, b.IsChecked() && b.IsChecked().Value());
                }
            });
            panel.Children().Append(box);
        }
        // Columns as wide as the longest label; the boxes wrap onto more rows as needed.
        FitItemWidth(panel);
    }
}

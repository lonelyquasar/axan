// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan: the settings editor's icon picker flyout body, shared by the Startup page's per-row
// icon button (#438) and the Sidebar page's state glyphs (sidebar.stateGlyphs). Code-built
// rather than a control: the Flyout content is rebuilt on each Opening (the classic-Binding
// limitation noted on the Startup page's profile combo applies here too). Lives in the shared
// src/inc (#38) next to AxanEntryIdentityEditor.h; it needs only the Windows.UI.Xaml
// projection the including project's pch already provides.
//
// Layout: a row of special cells (optional leading cells such as "Default", then "No
// icon"), the searchable Segoe Fluent grid shared with the identity editor (AxanGlyphGrid.h,
// #39: the whole font, a pick yields the glyph's name), a "Browse for an image" button, and a
// free-text row for an emoji, a builtin:NAME token or any literal icon string (Enter applies).

#include <AxanGlyphGrid.h>
#include <AxanIconRegistry.h>

#include <functional>
#include <vector>

namespace Axan::Ui::IconPicker
{
    struct Cell
    {
        winrt::hstring glyph; // what the cell shows (a Segoe glyph)
        winrt::hstring name; // Narrator name + tooltip
        std::function<void()> onClick; // the caller applies (and hides the flyout)
    };

    struct Options
    {
        std::vector<Cell> leading; // cells before "No icon" (e.g. "Default")
        winrt::hstring noneGlyph{ L"" }; // Cancel
        winrt::hstring noneName{ L"No icon" };
        winrt::hstring currentText; // the free-text box's initial (portable) value
        winrt::hstring placeholder{ L"emoji or builtin:NAME — Enter applies" };
        // A grid cell or the free text picked this NATIVE icon string ("" = none; a typed
        // builtin:NAME is mapped to its glyph first).
        std::function<void(const winrt::hstring&)> onPick;
        std::function<void()> onBrowse;
    };

    inline winrt::Windows::UI::Xaml::UIElement Build(const Options& options)
    {
        namespace WUX = winrt::Windows::UI::Xaml;
        namespace Controls = winrt::Windows::UI::Xaml::Controls;

        Controls::StackPanel root;
        root.Spacing(8);
        root.Width(300);

        Controls::VariableSizedWrapGrid grid;
        grid.Orientation(Controls::Orientation::Horizontal);
        grid.MaximumRowsOrColumns(6);
        grid.ItemWidth(40);
        grid.ItemHeight(40);

        const auto addCell = [&](const winrt::hstring& glyph, const winrt::hstring& name, std::function<void()> onClick) {
            Controls::Button cell;
            Controls::FontIcon icon;
            icon.FontSize(16);
            icon.Glyph(glyph);
            cell.Content(icon);
            cell.HorizontalAlignment(WUX::HorizontalAlignment::Stretch);
            cell.VerticalAlignment(WUX::VerticalAlignment::Stretch);
            WUX::Automation::AutomationProperties::SetName(cell, name);
            Controls::ToolTipService::SetToolTip(cell, winrt::box_value(name));
            cell.Click([onClick](auto&&, auto&&) {
                if (onClick)
                {
                    onClick();
                }
            });
            grid.Children().Append(cell);
        };

        const auto pick = options.onPick;
        for (const auto& c : options.leading)
        {
            addCell(c.glyph, c.name, c.onClick);
        }
        addCell(options.noneGlyph, options.noneName, [pick]() {
            if (pick)
            {
                pick(winrt::hstring{});
            }
        });
        root.Children().Append(grid);

        // The whole font, searchable. The picker object lives on through the grid's own
        // handlers (it holds only weak references back), so nothing here needs to keep it.
        Axan::Ui::GlyphGrid::Options glyphs;
        bool mapped = false;
        glyphs.initial = winrt::hstring{ Axan::IconRegistry::PortableToNative(options.currentText, mapped) };
        glyphs.showNone = false; // "No icon" is in the row above, beside the leading cells
        glyphs.height = 200;
        glyphs.onPick = [pick](const winrt::hstring& name) {
            if (pick)
            {
                pick(name);
            }
        };
        root.Children().Append(Axan::Ui::GlyphGrid::Build(std::move(glyphs)).root);

        Controls::Button browse;
        browse.Content(winrt::box_value(L"Browse for an image…"));
        browse.HorizontalAlignment(WUX::HorizontalAlignment::Stretch);
        browse.Click([onBrowse = options.onBrowse](auto&&, auto&&) {
            if (onBrowse)
            {
                onBrowse();
            }
        });
        root.Children().Append(browse);

        Controls::TextBox freeText;
        freeText.PlaceholderText(options.placeholder);
        freeText.Text(options.currentText);
        WUX::Automation::AutomationProperties::SetName(freeText, L"Icon text");
        freeText.KeyDown([pick](const winrt::Windows::Foundation::IInspectable& s, const WUX::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
            {
                if (const auto box = s.try_as<Controls::TextBox>(); box && pick)
                {
                    bool mapped = false;
                    pick(winrt::hstring{ Axan::IconRegistry::PortableToNative(box.Text(), mapped) });
                }
                e.Handled(true);
            }
        });
        root.Children().Append(freeText);

        return root;
    }
}

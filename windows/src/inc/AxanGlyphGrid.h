// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanGlyphGrid — the searchable Segoe Fluent Icons grid (#39): a search box over a
// virtualized GridView of glyph + name cells covering the whole font (AxanSegoeFluentGlyphs.h,
// ~1,400 glyphs), with an optional leading "No icon" cell.
//
// Shared by the identity editor (AxanEntryIdentityEditor.h: the sidebar's live node card and
// the Settings Startup sessions detail panel) and the icon picker flyout (AxanIconPicker.h:
// the Sidebar page's state glyphs). Code-built, so it needs only the Windows.UI.Xaml
// projection (Controls, Markup) the including project's pch provides.
//
// Behaviour:
//   - A pick stores the glyph's NAME ("robot"); AxanGlyphNames.h resolves it at render
//     time. "No icon" stores "". The callback fires only on a real change of selection.
//   - Virtualized: the GridView's ItemsWrapGrid realizes only the visible cells, which are
//     filled in ContainerContentChanging from a boxed index (no per-glyph elements, no
//     bindings). The grid has a fixed Height so it scrolls inside a bounded host.
//   - Keyboard: arrows move focus without selecting (SingleSelectionFollowsFocus off); Space,
//     Enter or a click picks. In the search box, Down moves into the grid and Enter picks
//     the first match. Each cell's UIA name is the glyph's display name (and its builtin
//     alias, if any).
//   - Lifetime: every handler holds the shared Picker, which holds only weak references to
//     its XAML elements, so the elements own the picker and nothing cycles. Hosts that need
//     Select() keep the returned shared_ptr too.

#pragma once

#include <AxanGlyphNames.h>

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Axan::Ui::GlyphGrid
{
    struct Strings
    {
        winrt::hstring searchPlaceholder{ L"Search icons by name" };
        winrt::hstring searchName{ L"Search icons" };
        winrt::hstring gridName{ L"Icons" };
        winrt::hstring noIconName{ L"No icon" };
        winrt::hstring iconNamePrefix{ L"Icon: " }; // + the glyph's display name, for Narrator
    };

    struct Options
    {
        winrt::hstring initial; // the current icon in any stored form ("" = none)
        double height{ 176 }; // fixed; the grid scrolls inside it
        bool showNone{ true }; // the leading "No icon" cell (only while the search is empty)
        Strings strings;
        std::function<void(const winrt::hstring&)> onPick; // a glyph name, or "" for "No icon"
    };

    inline constexpr int32_t kNoneItem = -1;

    // The icon's grid item: kNoneItem for "", the font index for anything that resolves to a
    // named glyph, nullopt for what the grid can't show (an emoji, a path, an unnamed PUA).
    inline std::optional<int32_t> ItemForIcon(std::wstring_view icon)
    {
        if (icon.empty())
        {
            return kNoneItem;
        }
        if (const auto cp = Axan::GlyphNames::ResolveCodepoint(icon))
        {
            if (const auto g = Axan::GlyphNames::FindByCodepoint(*cp))
            {
                return static_cast<int32_t>(g - Axan::GlyphNames::All().data());
            }
        }
        return std::nullopt;
    }

    class Picker : public std::enable_shared_from_this<Picker>
    {
    public:
        explicit Picker(Options options) :
            _options{ std::move(options) }, _current{ _options.initial } {}

        // Re-highlight for an icon set from outside (Browse, a text box). Never calls onPick.
        void Select(const winrt::hstring& icon)
        {
            _current = icon;
            _syncSelection(false);
        }

        // Drop the highlight without changing the stored icon (Browse pending).
        void ClearSelection()
        {
            if (const auto grid = _grid.get())
            {
                _syncing = true;
                grid.SelectedIndex(-1);
                _syncing = false;
            }
        }

        // Use Build(); public only so Build can reach it after make_shared.
        winrt::Windows::UI::Xaml::Controls::StackPanel BuildRoot()
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;

            Controls::TextBox search;
            search.PlaceholderText(_options.strings.searchPlaceholder);
            search.IsSpellCheckEnabled(false);
            WUX::Automation::AutomationProperties::SetName(search, _options.strings.searchName);

            Controls::GridView grid;
            grid.Height(_options.height);
            grid.SelectionMode(Controls::ListViewSelectionMode::Single);
            grid.SingleSelectionFollowsFocus(false);
            grid.IsItemClickEnabled(true);
            grid.Padding(WUX::Thickness{ 0, 4, 0, 0 });
            WUX::Automation::AutomationProperties::SetName(grid, _options.strings.gridName);
            grid.ItemTemplate(WUX::Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
  <StackPanel Width="60" Height="52" Spacing="3" Padding="0,7,0,0">
    <FontIcon FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="20" HorizontalAlignment="Center" />
    <TextBlock FontSize="10" MaxWidth="58" HorizontalAlignment="Center" TextAlignment="Center" TextTrimming="CharacterEllipsis" Opacity="0.8" />
  </StackPanel>
</DataTemplate>)")
                                  .as<WUX::DataTemplate>());

            _search = winrt::make_weak(search);
            _grid = winrt::make_weak(grid);

            const auto self = shared_from_this();
            grid.ContainerContentChanging([self](const auto&, const Controls::ContainerContentChangingEventArgs& args) {
                if (args.InRecycleQueue())
                {
                    return;
                }
                self->_fillCell(args.ItemContainer(), winrt::unbox_value_or<int32_t>(args.Item(), kNoneItem));
                args.Handled(true);
            });
            grid.ItemClick([self](const auto&, const Controls::ItemClickEventArgs& args) {
                self->_pick(winrt::unbox_value_or<int32_t>(args.ClickedItem(), kNoneItem));
            });
            grid.SelectionChanged([self](const auto&, const auto&) {
                if (self->_syncing)
                {
                    return;
                }
                if (const auto g = self->_grid.get())
                {
                    if (const auto item = g.SelectedItem())
                    {
                        self->_pick(winrt::unbox_value_or<int32_t>(item, kNoneItem));
                    }
                    else
                    {
                        self->_syncSelection(false); // a Ctrl+click deselect: the icon is unchanged
                    }
                }
            });
            grid.Loaded([self](const auto&, const auto&) {
                self->_syncSelection(true); // ScrollIntoView needs a laid-out list
            });

            search.TextChanged([self](const auto&, const auto&) {
                self->_applyFilter();
            });
            search.KeyDown([self](const auto&, const WUX::Input::KeyRoutedEventArgs& e) {
                const auto key = e.Key();
                if (key == winrt::Windows::System::VirtualKey::Down)
                {
                    if (const auto g = self->_grid.get())
                    {
                        g.Focus(WUX::FocusState::Keyboard);
                        e.Handled(true);
                    }
                }
                else if (key == winrt::Windows::System::VirtualKey::Enter)
                {
                    if (self->_items && self->_items.Size() > 0)
                    {
                        self->_pick(winrt::unbox_value_or<int32_t>(self->_items.GetAt(0), kNoneItem));
                    }
                    e.Handled(true);
                }
            });

            _applyFilter();

            Controls::StackPanel root;
            root.Spacing(4);
            root.Children().Append(search);
            root.Children().Append(grid);
            return root;
        }

    private:
        void _fillCell(const winrt::Windows::UI::Xaml::Controls::Primitives::SelectorItem& container, int32_t item) const
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            const auto panel = container.ContentTemplateRoot().try_as<Controls::StackPanel>();
            if (!panel || panel.Children().Size() < 2)
            {
                return;
            }
            const auto icon = panel.Children().GetAt(0).try_as<Controls::FontIcon>();
            const auto text = panel.Children().GetAt(1).try_as<Controls::TextBlock>();
            winrt::hstring glyph;
            winrt::hstring display;
            winrt::hstring tip;
            if (item == kNoneItem)
            {
                glyph = winrt::hstring{ L"\xE711" }; // Cancel
                display = _options.strings.noIconName;
                tip = display;
            }
            else
            {
                const auto& g = Axan::GlyphNames::All()[static_cast<size_t>(item)];
                glyph = winrt::hstring{ std::wstring(1, g.codepoint) };
                display = winrt::hstring{ g.displayName };
                std::wstring t{ g.displayName };
                t += L" (";
                t += g.key;
                if (const auto alias = Axan::GlyphNames::AliasFor(g.codepoint); !alias.empty() && alias != g.key)
                {
                    t += L", ";
                    t += alias;
                }
                t += L")";
                tip = winrt::hstring{ t };
            }
            if (icon)
            {
                icon.Glyph(glyph);
            }
            if (text)
            {
                text.Text(display);
            }
            WUX::Automation::AutomationProperties::SetName(container, winrt::hstring{ std::wstring{ _options.strings.iconNamePrefix } + std::wstring{ display } });
            Controls::ToolTipService::SetToolTip(container, winrt::box_value(tip));
        }

        void _pick(int32_t item)
        {
            winrt::hstring value;
            if (item != kNoneItem && item >= 0 && static_cast<size_t>(item) < Axan::GlyphNames::All().size())
            {
                value = winrt::hstring{ Axan::GlyphNames::All()[static_cast<size_t>(item)].key };
            }
            const auto changed = value != _current;
            _current = value;
            _syncSelection(false);
            if (changed && _options.onPick)
            {
                _options.onPick(value);
            }
        }

        void _applyFilter()
        {
            const auto grid = _grid.get();
            if (!grid)
            {
                return;
            }
            const auto search = _search.get();
            const auto query = search ? search.Text() : winrt::hstring{};
            std::vector<winrt::Windows::Foundation::IInspectable> items;
            const auto hits = Axan::GlyphNames::Search(query);
            items.reserve(hits.size() + 1);
            if (_options.showNone && Axan::GlyphNames::NormalizeName(query).empty())
            {
                items.push_back(winrt::box_value(kNoneItem));
            }
            for (const auto i : hits)
            {
                items.push_back(winrt::box_value(static_cast<int32_t>(i)));
            }
            _items = winrt::single_threaded_vector<winrt::Windows::Foundation::IInspectable>(std::move(items));
            _syncing = true;
            grid.ItemsSource(_items);
            _syncing = false;
            _syncSelection(false);
        }

        void _syncSelection(bool scrollIntoView)
        {
            const auto grid = _grid.get();
            if (!grid || !_items)
            {
                return;
            }
            int32_t index = -1;
            if (const auto want = ItemForIcon(_current))
            {
                const auto size = _items.Size();
                for (uint32_t i = 0; i < size; ++i)
                {
                    if (winrt::unbox_value_or<int32_t>(_items.GetAt(i), INT32_MIN) == *want)
                    {
                        index = static_cast<int32_t>(i);
                        break;
                    }
                }
            }
            _syncing = true;
            grid.SelectedIndex(index);
            _syncing = false;
            if (scrollIntoView && index >= 0)
            {
                grid.ScrollIntoView(_items.GetAt(static_cast<uint32_t>(index)));
            }
        }

        Options _options;
        winrt::hstring _current;
        bool _syncing{ false };
        winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable> _items{ nullptr };
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::TextBox> _search;
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::GridView> _grid;
    };

    struct Built
    {
        winrt::Windows::UI::Xaml::Controls::StackPanel root{ nullptr }; // search box over grid
        std::shared_ptr<Picker> picker;
    };

    // Builds the search box + grid. Never calls onPick during Build.
    inline Built Build(Options options)
    {
        auto picker = std::make_shared<Picker>(std::move(options));
        auto root = picker->BuildRoot();
        return { root, picker };
    }
}

// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanGlyphGrid — the searchable icon grid (#39): a search box and a character-set selector
// over a virtualized GridView of glyph + name cells, with an optional leading "No icon"
// cell. The sets come from AxanGlyphSets.h: "Segoe Fluent Icons" (the whole font, ~1,400
// glyphs; the default) and "Letters and digits"; a new set there appears in the selector
// with no change here or in the hosts.
//
// Shared by the identity editor (AxanEntryIdentityEditor.h: the sidebar's live node card and
// the Settings Startup sessions detail panel) and the icon picker flyout (AxanIconPicker.h:
// the Sidebar page's state glyphs). Code-built, so it needs only the Windows.UI.Xaml
// projection (Controls, Markup, Media) the including project's pch provides.
//
// Behaviour:
//   - The grid shows one set; the search filters within it. It opens on the set holding the
//     current icon (else the first). A pick stores the set's form: a Segoe glyph's NAME
//     ("robot", resolved at render time by AxanGlyphNames.h), a letter's character ("a").
//     "No icon" stores "". The callback fires only on a real change of selection.
//   - A code point in the search box (U+131A3, 0x131A3, 131A3) filters to its cell when the
//     set has it; otherwise one "direct" cell leads the grid, labelled U+131A3, drawn as text
//     so font fallback finds a font for it, and storing the literal character.
//   - Virtualized: the GridView's ItemsWrapGrid realizes only the visible cells, which are
//     filled in ContainerContentChanging from a boxed index (no per-glyph elements, no
//     bindings). The grid has a fixed Height so it scrolls inside a bounded host.
//   - Fits a 300 px flyout: the selector is capped beside a star-sized search box.
//   - Keyboard: arrows move focus without selecting (SingleSelectionFollowsFocus off); Space,
//     Enter or a click picks. In the search box, Down moves into the grid and Enter picks
//     the first match. Each cell's UIA name is the glyph's display name; the tooltip adds
//     its search name (and builtin alias, if any).
//   - Lifetime: every handler holds the shared Picker, which holds only weak references to
//     its XAML elements, so the elements own the picker and nothing cycles. Hosts that need
//     Select() keep the returned shared_ptr too.

#pragma once

#include <AxanGlyphSets.h>

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
        winrt::hstring searchPlaceholder{ L"Name or U+code" };
        winrt::hstring searchName{ L"Search icons by name or Unicode code point" };
        winrt::hstring gridName{ L"Icons" };
        winrt::hstring noIconName{ L"No icon" };
        winrt::hstring iconNamePrefix{ L"Icon: " }; // + the glyph's display name, for Narrator
        winrt::hstring setSelectorName{ L"Character set" };
        winrt::hstring anyFontName{ L"any font" }; // the direct code-point cell's tooltip
    };

    struct Options
    {
        winrt::hstring initial; // the current icon in any stored form ("" = none)
        double height{ 176 }; // fixed; the grid scrolls inside it
        bool showNone{ true }; // the leading "No icon" cell (only while the search is empty)
        Strings strings;
        std::function<void(const winrt::hstring&)> onPick; // the stored form, or "" for "No icon"
    };

    inline constexpr int32_t kNoneItem = -1;
    inline constexpr int32_t kDirectItem = -2; // the code-point cell; other items index the set
    inline constexpr double kSetSelectorMaxWidth = 136;

    class Picker : public std::enable_shared_from_this<Picker>
    {
    public:
        explicit Picker(Options options) :
            _options{ std::move(options) }, _current{ _options.initial }
        {
            _setIndex = Axan::GlyphSets::SetForIcon(_current).value_or(0);
        }

        // Re-highlight for an icon set from outside (Browse, a text box), switching to the
        // set that holds it if the shown one doesn't. Never calls onPick.
        void Select(const winrt::hstring& icon)
        {
            _current = icon;
            if (!_set().Find(icon))
            {
                if (const auto other = Axan::GlyphSets::SetForIcon(icon))
                {
                    _showSet(*other);
                    return;
                }
            }
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

            const auto& sets = Axan::GlyphSets::Sets();
            for (const auto set : sets)
            {
                _setFonts.push_back(WUX::Media::FontFamily{ winrt::hstring{ set->FontFamily() } });
            }
            _symbolFont = WUX::Media::FontFamily{ winrt::hstring{ Axan::GlyphSets::kSymbolFont } };
            _textFont = WUX::Media::FontFamily{ winrt::hstring{ Axan::GlyphSets::kTextFont } };

            Controls::TextBox search;
            search.PlaceholderText(_options.strings.searchPlaceholder);
            search.IsSpellCheckEnabled(false);
            WUX::Automation::AutomationProperties::SetName(search, _options.strings.searchName);

            // The set selector: the sets' names, trimmed to fit beside the search box.
            Controls::ComboBox setBox;
            setBox.MaxWidth(kSetSelectorMaxWidth);
            setBox.VerticalAlignment(WUX::VerticalAlignment::Stretch);
            setBox.ItemTemplate(WUX::Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
  <TextBlock Text="{Binding}" TextTrimming="CharacterEllipsis" />
</DataTemplate>)")
                                    .as<WUX::DataTemplate>());
            {
                std::vector<winrt::Windows::Foundation::IInspectable> names;
                for (const auto set : sets)
                {
                    names.push_back(winrt::box_value(winrt::hstring{ set->Name() }));
                }
                setBox.ItemsSource(winrt::single_threaded_vector<winrt::Windows::Foundation::IInspectable>(std::move(names)));
            }
            setBox.SelectedIndex(static_cast<int32_t>(_setIndex));
            WUX::Automation::AutomationProperties::SetName(setBox, _options.strings.setSelectorName);
            Controls::ToolTipService::SetToolTip(setBox, winrt::box_value(_options.strings.setSelectorName));

            Controls::Grid header;
            header.ColumnSpacing(4);
            {
                Controls::ColumnDefinition c0, c1;
                c0.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
                c1.Width(WUX::GridLengthHelper::FromValueAndType(0, WUX::GridUnitType::Auto));
                header.ColumnDefinitions().Append(c0);
                header.ColumnDefinitions().Append(c1);
            }
            Controls::Grid::SetColumn(search, 0);
            Controls::Grid::SetColumn(setBox, 1);
            header.Children().Append(search);
            header.Children().Append(setBox);

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
            _setBox = winrt::make_weak(setBox);
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

            setBox.SelectionChanged([self](const auto&, const auto&) {
                if (self->_syncing)
                {
                    return;
                }
                if (const auto box = self->_setBox.get(); box && box.SelectedIndex() >= 0)
                {
                    self->_showSet(static_cast<size_t>(box.SelectedIndex()));
                }
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
            root.Children().Append(header);
            root.Children().Append(grid);
            return root;
        }

    private:
        const Axan::GlyphSets::Set& _set() const
        {
            return *Axan::GlyphSets::Sets()[_setIndex];
        }

        // Switch the shown set (the selector follows) and re-filter.
        void _showSet(size_t index)
        {
            if (index >= Axan::GlyphSets::Sets().size())
            {
                return;
            }
            _setIndex = index;
            if (const auto box = _setBox.get(); box && box.SelectedIndex() != static_cast<int32_t>(index))
            {
                _syncing = true;
                box.SelectedIndex(static_cast<int32_t>(index));
                _syncing = false;
            }
            _applyFilter();
            _syncSelection(true);
        }

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
            WUX::Media::FontFamily font{ nullptr };
            if (item == kNoneItem)
            {
                glyph = winrt::hstring{ L"\xE711" }; // Cancel
                display = _options.strings.noIconName;
                tip = display;
                font = _symbolFont;
            }
            else if (item == kDirectItem && _direct)
            {
                const auto label = Axan::GlyphSets::FormatCodepoint(*_direct);
                glyph = winrt::hstring{ Axan::GlyphSets::ToUtf16(*_direct) };
                display = winrt::hstring{ label };
                tip = winrt::hstring{ label + L" (" + std::wstring{ _options.strings.anyFontName } + L")" };
                font = _textFont; // a text font, so DirectWrite falls back per character
            }
            else if (item >= 0 && static_cast<size_t>(item) < _set().Size())
            {
                const auto cell = _set().At(static_cast<size_t>(item));
                glyph = winrt::hstring{ cell.glyph };
                display = winrt::hstring{ cell.displayName };
                std::wstring t{ cell.displayName };
                t += L" (";
                t += cell.key;
                if (!cell.alias.empty() && cell.alias != cell.key)
                {
                    t += L", ";
                    t += cell.alias;
                }
                t += L")";
                tip = winrt::hstring{ t };
                font = _setFonts[_setIndex];
            }
            if (icon)
            {
                // Containers are recycled across sets and the special cells: set the font each time.
                if (font)
                {
                    icon.FontFamily(font);
                }
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
            if (item == kDirectItem && _direct)
            {
                value = winrt::hstring{ Axan::GlyphSets::ToUtf16(*_direct) };
            }
            else if (item >= 0 && static_cast<size_t>(item) < _set().Size())
            {
                value = winrt::hstring{ _set().At(static_cast<size_t>(item)).stored };
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
            const std::wstring_view q{ query };
            const auto& set = _set();
            auto hits = set.Search(q);

            std::vector<winrt::Windows::Foundation::IInspectable> items;
            items.reserve(hits.size() + 2);
            if (_options.showNone && q.find_first_not_of(L" \t") == std::wstring_view::npos)
            {
                items.push_back(winrt::box_value(kNoneItem));
            }
            // A code point leads: its own cell when the set has it, else the direct cell.
            _direct = Axan::GlyphSets::ParseCodepoint(q);
            if (_direct)
            {
                if (const auto own = set.FindCodepoint(*_direct))
                {
                    hits.erase(std::remove(hits.begin(), hits.end(), *own), hits.end());
                    items.push_back(winrt::box_value(static_cast<int32_t>(*own)));
                }
                else
                {
                    items.push_back(winrt::box_value(kDirectItem));
                }
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

        // The grid item showing the current icon, if any.
        std::optional<int32_t> _itemForCurrent() const
        {
            if (_current.empty())
            {
                return kNoneItem;
            }
            if (const auto index = _set().Find(_current))
            {
                return static_cast<int32_t>(*index);
            }
            if (_direct && _current == Axan::GlyphSets::ToUtf16(*_direct))
            {
                return kDirectItem;
            }
            return std::nullopt;
        }

        void _syncSelection(bool scrollIntoView)
        {
            const auto grid = _grid.get();
            if (!grid || !_items)
            {
                return;
            }
            int32_t index = -1;
            if (const auto want = _itemForCurrent())
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
        size_t _setIndex{ 0 };
        std::optional<char32_t> _direct; // the code point the search box names, if any
        bool _syncing{ false };
        winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable> _items{ nullptr };
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::TextBox> _search;
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::ComboBox> _setBox;
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::GridView> _grid;
        std::vector<winrt::Windows::UI::Xaml::Media::FontFamily> _setFonts;
        winrt::Windows::UI::Xaml::Media::FontFamily _symbolFont{ nullptr };
        winrt::Windows::UI::Xaml::Media::FontFamily _textFont{ nullptr };
    };

    struct Built
    {
        winrt::Windows::UI::Xaml::Controls::StackPanel root{ nullptr }; // search + selector over grid
        std::shared_ptr<Picker> picker;
    };

    // Builds the search box, set selector and grid. Never calls onPick during Build.
    inline Built Build(Options options)
    {
        auto picker = std::make_shared<Picker>(std::move(options));
        auto root = picker->BuildRoot();
        return { root, picker };
    }
}

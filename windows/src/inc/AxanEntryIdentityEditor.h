// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanEntryIdentityEditor — the shared "identity" section of a session editor (#38): the
// name / label template field with its variables help, an icon preview, the searchable
// Segoe Fluent icon grid + "Browse to icon file…" (#39), the palette colour swatches plus a
// custom hex / rgb() swatch (#50), and the colour target radio (icon / text / both).
//
// Two hosts build it: TerminalApp's live node editor (the sidebar's "Edit session node"
// card, staged behind Save/Cancel) and the Settings Editor's Startup sessions detail panel
// (instant commit to the settings clone). They differ in data source, commit model and
// file picker, so this is a code-built builder, not a XAML control: each host seeds
// Options::initial, receives every user edit through onChanged, and owns Browse (it calls
// Editor::SetIcon with the picked file). A field added to the identity section is added
// here once (plus the hosts' persistence: _rowFromModel / _modelFromRow and
// _PersistNodeToEntry).
//
// Contracts:
//   - Build never calls onChanged. Every handler compares against the current Fields and
//     only notifies on a real change, which also absorbs the TextBox's deferred initial
//     TextChanged and any Checked raised for the pre-checked radio.
//   - The host keeps the returned shared_ptr alive for as long as the UI is up; the XAML
//     handlers hold only a weak_ptr (no Editor <-> element reference cycle), so once the
//     host drops it the controls go inert.
//   - Strings are passed in (each project resolves its own resw; this header must not call
//     RS_). The defaults are the live node editor's English literals.
//   - Needs only the Windows.UI.Xaml projection the including project's pch provides.

#pragma once

#include <AxanGlyphGrid.h>
#include <AxanGlyphNames.h>
#include <AxanIconRegistry.h>
#include <AxanLog.h>
#include <AxanSessionPalette.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Axan::Ui::IdentityEditor
{
    // The values the section edits. Stored forms, exactly as the hosts persist them:
    // name = the label template ("" = the default name); icon = a NATIVE icon string (a
    // glyph name, a literal glyph, a file path, an emoji; "" = no override, the session's own icon); color = a
    // palette name or "#hex" ("" = none); colorTarget = "icon" | "text" | "both".
    struct Fields
    {
        winrt::hstring name;
        winrt::hstring icon;
        winrt::hstring color;
        winrt::hstring colorTarget{ L"both" };
    };

    struct Strings
    {
        winrt::hstring nameLabel{ L"Session name" };
        winrt::hstring namePlaceholder{ L"$dir — $shell" };
        winrt::hstring helpButtonName{ L"Session name variables help" };
        winrt::hstring helpHeading{ L"Session name variables — type any of these and axan substitutes the live value; mix with plain text." };
        winrt::hstring helpExample{ L"Example:  $dir — $shell  →  axan — pwsh" };
        winrt::hstring iconLabel{ L"Icon" };
        winrt::hstring noIconName{ L"No icon" };
        winrt::hstring iconNamePrefix{ L"Icon: " }; // + the glyph's display name
        winrt::hstring currentPrefix{ L"Current: " }; // + the icon's display name or value
        winrt::hstring clearText{ L"Clear" };
        winrt::hstring clearName{ L"Clear icon" };
        winrt::hstring searchPlaceholder{ L"Name or U+code" };
        winrt::hstring searchName{ L"Search icons by name or Unicode code point" };
        winrt::hstring gridName{ L"Icons" };
        winrt::hstring browseText{ L"Browse to icon file…" };
        winrt::hstring browseName{ L"Browse to icon file" };
        winrt::hstring colorLabel{ L"Color" };
        // axan #50: the custom swatch after the palette and its hex / RGB entry.
        winrt::hstring customName{ L"Custom color" };
        winrt::hstring customTooltip{ L"Custom color: hex or RGB" };
        winrt::hstring customBoxName{ L"Custom color value" };
        winrt::hstring customPlaceholder{ L"#RRGGBB or rgb(r, g, b)" };
        winrt::hstring customError{ L"Use #RRGGBB or rgb(r, g, b)" };
        winrt::hstring applyLabel{ L"Apply color to" };
        winrt::hstring applyHint{ L"recolors glyph icons; image files keep their own colors" };
        winrt::hstring targetIcon{ L"Icon" };
        winrt::hstring targetText{ L"Text" };
        winrt::hstring targetBoth{ L"Both" };
    };

    // The label-template variables listed in the "?" help flyout (AxanLabelTemplate.h's
    // vocabulary; the code column is literal syntax, not localisable).
    struct TemplateVariable
    {
        const wchar_t* code;
        const wchar_t* description;
    };
    inline constexpr std::array<TemplateVariable, 11> kTemplateVariables{ {
        { L"$shell", L"Shell name — pwsh, cmd, bash…" },
        { L"$dir", L"Current folder name only" },
        { L"$cwd  $pwd", L"Full working-directory path" },
        { L"$~", L"Home-relative working directory" },
        { L"$branch", L"Active git branch, if any" },
        { L"$repo", L"Git repository name" },
        { L"$user", L"Logged-in user" },
        { L"$host", L"Machine hostname" },
        { L"$title", L"Terminal title (OSC 0/2)" },
        { L"$cmd", L"Foreground command" },
        { L"${file:path}", L"First line of the file at path" },
    } };

    // Selection highlight brushes: the system accent (with the live editor's fallback) and a
    // translucent grey for unselected cells.
    inline winrt::Windows::UI::Xaml::Media::SolidColorBrush DefaultAccentBrush()
    {
        winrt::Windows::UI::Color accent{ 255, 0x4c, 0xc2, 0xff };
        if (const auto a = winrt::Windows::UI::Xaml::Application::Current().Resources().TryLookup(winrt::box_value(winrt::hstring{ L"SystemAccentColor" })))
        {
            accent = winrt::unbox_value_or<winrt::Windows::UI::Color>(a, accent);
        }
        return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ accent };
    }

    inline winrt::Windows::UI::Xaml::Media::SolidColorBrush DefaultNeutralBrush()
    {
        return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 0x40, 0x80, 0x80, 0x80 } };
    }

    struct Options
    {
        Fields initial;
        // The theme the swatches (and, via the host's renderPreview, the preview) resolve
        // palette names against: a swatch STORES a name and FILLS with that name's hex in
        // the active set (WYSIWYG, #422).
        bool isDark{ true };
        Strings strings;
        bool showTemplateHelp{ true }; // the "?" variables flyout beside the name label
        double iconGridHeight{ 176 }; // the glyph grid's fixed height (about three rows)
        winrt::Windows::UI::Xaml::Media::Brush accentBrush{ nullptr }; // null = DefaultAccentBrush()
        winrt::Windows::UI::Xaml::Media::Brush neutralBrush{ nullptr }; // null = DefaultNeutralBrush()
        // Renders the 40x40 preview box's content (sized to 24x24 here) from the current
        // fields. Re-run on Build and after every icon / colour / target change (not per
        // name keystroke). Null leaves the box empty.
        std::function<winrt::Windows::UI::Xaml::UIElement(const Fields&)> renderPreview;
        // Every user edit, with the full current Fields. Never called during Build.
        std::function<void(const Fields&)> onChanged;
        // "Browse to icon file…" was clicked (after the grid's highlight is cleared). The
        // host opens its picker and, on a pick, calls Editor::SetIcon(path).
        std::function<void()> onBrowseIcon;
    };

    class Editor : public std::enable_shared_from_this<Editor>
    {
    public:
        // The section's root panel. Its children stack with Spacing(2), so hosting it inside
        // a StackPanel of the same spacing is layout-neutral.
        winrt::Windows::UI::Xaml::Controls::StackPanel Root() const { return _root; }
        winrt::Windows::UI::Xaml::Controls::TextBox NameBox() const { return _nameBox; }
        const Fields& Current() const noexcept { return _fields; }

        // Sets the icon from outside the grid (a Browse result, Clear, a host text box).
        // Re-highlights the grid (no cell matches a file path or an emoji), refreshes the
        // caption and the preview, and notifies onChanged.
        void SetIcon(const winrt::hstring& icon)
        {
            if (icon == _fields.icon)
            {
                return;
            }
            _fields.icon = icon;
            if (_glyphs)
            {
                _glyphs->Select(icon);
            }
            _refreshCurrentIcon();
            RefreshPreview();
            _notify();
        }

        void RefreshPreview()
        {
            if (!_preview)
            {
                return;
            }
            winrt::Windows::UI::Xaml::UIElement el{ nullptr };
            if (_options.renderPreview)
            {
                el = _options.renderPreview(_fields);
            }
            if (const auto fe = el ? el.try_as<winrt::Windows::UI::Xaml::FrameworkElement>() : nullptr)
            {
                fe.Width(24);
                fe.Height(24);
            }
            _preview.Content(el);
        }

        // Use Build() (or Create); public only so std::make_shared can reach it.
        explicit Editor(Options options) :
            _options{ std::move(options) }, _fields{ _options.initial }
        {
            if (_fields.colorTarget.empty())
            {
                _fields.colorTarget = L"both";
            }
            if (!_options.accentBrush)
            {
                _options.accentBrush = DefaultAccentBrush();
            }
            if (!_options.neutralBrush)
            {
                _options.neutralBrush = DefaultNeutralBrush();
            }
        }

        static std::shared_ptr<Editor> Create(Options options)
        {
            auto editor = std::make_shared<Editor>(std::move(options));
            editor->_build(); // needs the owning shared_ptr: handlers capture weak_from_this()
            return editor;
        }

    private:

        static winrt::Windows::UI::Xaml::Controls::TextBlock _sectionLabel(const winrt::hstring& text)
        {
            winrt::Windows::UI::Xaml::Controls::TextBlock t;
            t.Text(text);
            t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            t.Margin(winrt::Windows::UI::Xaml::Thickness{ 0, 10, 0, 4 });
            return t;
        }

        void _notify() const
        {
            if (_options.onChanged)
            {
                _options.onChanged(_fields);
            }
        }

        template<typename T>
        void _setSelected(const T& el, bool selected) const
        {
            el.BorderBrush(selected ? _options.accentBrush : _options.neutralBrush);
            el.BorderThickness(selected ? winrt::Windows::UI::Xaml::Thickness{ 2, 2, 2, 2 } : winrt::Windows::UI::Xaml::Thickness{ 1, 1, 1, 1 });
        }

        void _highlightSwatch() const
        {
            for (const auto& [token, border] : _swatches)
            {
                _setSelected(border, token == _fields.color);
            }
            if (_customSwatch)
            {
                _setSelected(_customSwatch, _isCustomColor(_fields.color));
            }
        }

        // axan #50: a stored "#hex" is the custom swatch's; a palette name (or "") is not.
        static bool _isCustomColor(const winrt::hstring& color) noexcept
        {
            return !color.empty() && color.front() == L'#';
        }

        // Paints the custom swatch: filled with _customColor when it parses, otherwise the
        // hollow box with the pencil glyph.
        void _paintCustomSwatch() const
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            if (!_customSwatch)
            {
                return;
            }
            if (const auto c = Axan::SessionPalette::ParseHexColor(_customColor))
            {
                _customSwatch.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ c->a, c->r, c->g, c->b } });
                _customSwatch.Child(nullptr);
                Controls::ToolTipService::SetToolTip(_customSwatchButton, winrt::box_value(winrt::hstring{ std::wstring{ _options.strings.customName } + L": " + std::wstring{ _customColor } }));
            }
            else
            {
                _customSwatch.Background(nullptr);
                Controls::FontIcon fi;
                fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
                fi.Glyph(winrt::hstring{ L"\xE70F" } /* Edit */);
                fi.FontSize(14);
                fi.Opacity(0.75);
                _customSwatch.Child(fi);
                Controls::ToolTipService::SetToolTip(_customSwatchButton, winrt::box_value(_options.strings.customTooltip));
            }
        }

        // axan #50: validates the custom box on Enter / focus loss. Valid -> the canonical
        // "#rrggbb" becomes the colour (swatch filled and selected); invalid -> the previous
        // colour stays and the inline error shows. Never throws. explicitCommit = Enter (the
        // user asked for this value); false = focus loss, which must never undo a preset.
        void _commitCustomColor(bool explicitCommit)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            if (!_customBox)
            {
                return;
            }
            const auto text = _customBox.Text();
            std::wstring_view trimmed{ text };
            while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t'))
            {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t'))
            {
                trimmed.remove_suffix(1);
            }
            if (trimmed.empty())
            {
                _customError.Visibility(WUX::Visibility::Collapsed);
                return;
            }
            // The last committed custom value unchanged (which may be a "#AARRGGBB" from JSON
            // that the box doesn't itself accept, or a value left in the box after a preset
            // was picked). On focus loss: nothing to commit, so a preset is never undone. On
            // Enter: re-select it verbatim (no re-parse, so "#AARRGGBB" survives).
            if (trimmed == std::wstring_view{ _customColor })
            {
                _customError.Visibility(WUX::Visibility::Collapsed);
                if (explicitCommit && _customColor != _fields.color)
                {
                    Axan::Log::Debug("IdentityEditor", "custom color reselected", { { "issue", "50" }, { "stored", winrt::to_string(_customColor) } });
                    _fields.color = _customColor;
                    _highlightSwatch();
                    RefreshPreview();
                    _notify();
                }
                return;
            }
            const auto parsed = Axan::SessionPalette::ParseCustomColor(trimmed);
            if (!parsed)
            {
                _customError.Visibility(WUX::Visibility::Visible);
                Axan::Log::Debug("IdentityEditor", "custom color rejected", { { "issue", "50" }, { "input", winrt::to_string(text) }, { "kept", winrt::to_string(_fields.color) } });
                return;
            }
            _customError.Visibility(WUX::Visibility::Collapsed);
            const winrt::hstring value{ *parsed };
            if (_customBox.Text() != value)
            {
                _customBox.Text(value); // show the stored spelling
            }
            Axan::Log::Debug("IdentityEditor", "custom color accepted", { { "issue", "50" }, { "input", winrt::to_string(text) }, { "stored", winrt::to_string(value) } });
            _customColor = value;
            _paintCustomSwatch();
            if (value != _fields.color)
            {
                _fields.color = value;
                _highlightSwatch();
                RefreshPreview();
                _notify();
            }
        }

        winrt::Windows::UI::Xaml::Controls::Button _buildHelpButton() const
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            Controls::Button helpBtn;
            helpBtn.Content(winrt::box_value(winrt::hstring{ L"?" }));
            helpBtn.Width(22);
            helpBtn.Height(22);
            helpBtn.Padding(WUX::Thickness{ 0, 0, 0, 0 });
            // axan #429: "?" alone is a useless Narrator name.
            WUX::Automation::AutomationProperties::SetName(helpBtn, _options.strings.helpButtonName);

            const auto mono = WUX::Media::FontFamily{ L"Cascadia Mono, Consolas, monospace" };
            Controls::StackPanel vars;
            vars.Spacing(3);
            vars.MaxWidth(330);
            Controls::TextBlock head;
            head.Text(_options.strings.helpHeading);
            head.TextWrapping(WUX::TextWrapping::Wrap);
            head.Margin(WUX::Thickness{ 0, 0, 0, 6 });
            vars.Children().Append(head);
            for (const auto& v : kTemplateVariables)
            {
                Controls::StackPanel row;
                row.Orientation(Controls::Orientation::Horizontal);
                row.Spacing(8);
                Controls::TextBlock c;
                c.Text(v.code);
                c.FontFamily(mono);
                c.Width(92);
                Controls::TextBlock d;
                d.Text(v.description);
                d.TextWrapping(WUX::TextWrapping::Wrap);
                row.Children().Append(c);
                row.Children().Append(d);
                vars.Children().Append(row);
            }
            Controls::TextBlock eg;
            eg.Text(_options.strings.helpExample);
            eg.Margin(WUX::Thickness{ 0, 8, 0, 0 });
            eg.FontFamily(mono);
            vars.Children().Append(eg);
            Controls::Flyout helpFlyout;
            helpFlyout.Content(vars);
            helpBtn.Flyout(helpFlyout);
            return helpBtn;
        }

        // [caption (+ "?")] over [preview box | name field].
        winrt::Windows::UI::Xaml::Controls::StackPanel _buildNameSection()
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            Controls::StackPanel labelRow;
            labelRow.Orientation(Controls::Orientation::Horizontal);
            labelRow.Spacing(6);
            labelRow.Margin(WUX::Thickness{ 0, 0, 0, 5 });
            {
                Controls::TextBlock t;
                t.Text(_options.strings.nameLabel);
                t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                t.VerticalAlignment(WUX::VerticalAlignment::Center);
                labelRow.Children().Append(t);
            }
            if (_options.showTemplateHelp)
            {
                labelRow.Children().Append(_buildHelpButton());
            }

            _preview = Controls::ContentPresenter{};
            _preview.HorizontalAlignment(WUX::HorizontalAlignment::Center);
            _preview.VerticalAlignment(WUX::VerticalAlignment::Center);
            Controls::Border previewBox;
            previewBox.Width(40);
            previewBox.Height(40);
            previewBox.CornerRadius(WUX::CornerRadius{ 5, 5, 5, 5 });
            previewBox.BorderThickness(WUX::Thickness{ 1, 1, 1, 1 });
            previewBox.BorderBrush(_options.neutralBrush);
            previewBox.Child(_preview);

            _nameBox = Controls::TextBox{};
            _nameBox.Text(_fields.name);
            _nameBox.PlaceholderText(_options.strings.namePlaceholder);
            _nameBox.AcceptsReturn(false);
            _nameBox.Height(40);
            _nameBox.VerticalContentAlignment(WUX::VerticalAlignment::Center);
            _nameBox.TextChanged([weak = weak_from_this()](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    // The initial Text raises a deferred TextChanged; it equals the seed.
                    const auto text = self->_nameBox.Text();
                    if (text != self->_fields.name)
                    {
                        self->_fields.name = text;
                        self->_notify();
                    }
                }
            });

            Controls::Grid nameRow;
            {
                Controls::ColumnDefinition c0;
                c0.Width(WUX::GridLengthHelper::FromValueAndType(0, WUX::GridUnitType::Auto));
                Controls::ColumnDefinition c1;
                c1.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
                nameRow.ColumnDefinitions().Append(c0);
                nameRow.ColumnDefinitions().Append(c1);
                nameRow.ColumnSpacing(8);
                Controls::Grid::SetColumn(previewBox, 0);
                Controls::Grid::SetColumn(_nameBox, 1);
                nameRow.Children().Append(previewBox);
                nameRow.Children().Append(_nameBox);
            }

            Controls::StackPanel section;
            section.Spacing(2);
            section.Children().Append(labelRow);
            section.Children().Append(nameRow);
            return section;
        }

        // The current icon's caption: a named glyph reads as its display name, an unnamed
        // symbol as U+XXXX, anything else (emoji, image path) as the literal value.
        winrt::hstring _currentIconText() const
        {
            const std::wstring_view icon{ _fields.icon };
            std::wstring text{ _options.strings.currentPrefix };
            if (icon.empty())
            {
                text += _options.strings.noIconName;
            }
            else if (const auto cp = Axan::GlyphNames::ResolveCodepoint(icon))
            {
                if (const auto g = Axan::GlyphNames::FindByCodepoint(*cp))
                {
                    text += g->displayName;
                }
                else
                {
                    wchar_t buf[8]{};
                    swprintf_s(buf, L"U+%04X", static_cast<unsigned>(*cp));
                    text += buf;
                }
            }
            else
            {
                text += icon;
            }
            return winrt::hstring{ text };
        }

        void _refreshCurrentIcon() const
        {
            if (_currentIcon)
            {
                _currentIcon.Text(_currentIconText());
            }
            if (_clearIcon)
            {
                _clearIcon.IsEnabled(!_fields.icon.empty());
            }
        }

        // The icon section (#39): [current icon | Clear | Browse] over the searchable,
        // virtualized grid of the whole Segoe Fluent Icons font (AxanGlyphGrid.h). The grid
        // has a fixed height so the live node card stays bounded; a pick stores the glyph's
        // name ("robot"), which renders anywhere via AxanGlyphNames.h.
        winrt::Windows::UI::Xaml::Controls::StackPanel _buildIconSection()
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            const auto symbols = WUX::Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" };

            _currentIcon = Controls::TextBlock{};
            _currentIcon.VerticalAlignment(WUX::VerticalAlignment::Center);
            _currentIcon.TextTrimming(WUX::TextTrimming::CharacterEllipsis);

            _clearIcon = Controls::Button{};
            _clearIcon.Content(winrt::box_value(_options.strings.clearText));
            WUX::Automation::AutomationProperties::SetName(_clearIcon, _options.strings.clearName);
            _clearIcon.VerticalAlignment(WUX::VerticalAlignment::Center);
            _clearIcon.Click([weak = weak_from_this()](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->SetIcon(winrt::hstring{});
                }
            });

            Controls::Button browse;
            {
                Controls::StackPanel bc;
                bc.Orientation(Controls::Orientation::Horizontal);
                bc.Spacing(7);
                Controls::FontIcon fi;
                fi.FontFamily(symbols);
                fi.Glyph(winrt::hstring{ L"\xE8B7" } /* OpenFolderHorizontal */);
                fi.FontSize(14);
                Controls::TextBlock t;
                t.Text(_options.strings.browseText);
                bc.Children().Append(fi);
                bc.Children().Append(t);
                browse.Content(bc);
            }
            // axan #429: the StackPanel content yields no automatic UIA name.
            WUX::Automation::AutomationProperties::SetName(browse, _options.strings.browseName);
            browse.VerticalAlignment(WUX::VerticalAlignment::Center);
            browse.Click([weak = weak_from_this()](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    // As before: the grid's highlight clears now; the icon only changes if
                    // the host's picker returns a file (SetIcon).
                    if (self->_glyphs)
                    {
                        self->_glyphs->ClearSelection();
                    }
                    if (self->_options.onBrowseIcon)
                    {
                        self->_options.onBrowseIcon();
                    }
                }
            });

            Controls::Grid top;
            {
                Controls::ColumnDefinition c0;
                c0.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
                Controls::ColumnDefinition c1;
                c1.Width(WUX::GridLengthHelper::FromValueAndType(0, WUX::GridUnitType::Auto));
                Controls::ColumnDefinition c2;
                c2.Width(WUX::GridLengthHelper::FromValueAndType(0, WUX::GridUnitType::Auto));
                top.ColumnDefinitions().Append(c0);
                top.ColumnDefinitions().Append(c1);
                top.ColumnDefinitions().Append(c2);
                top.ColumnSpacing(8);
                Controls::Grid::SetColumn(_currentIcon, 0);
                Controls::Grid::SetColumn(_clearIcon, 1);
                Controls::Grid::SetColumn(browse, 2);
                top.Children().Append(_currentIcon);
                top.Children().Append(_clearIcon);
                top.Children().Append(browse);
            }
            _refreshCurrentIcon();

            Axan::Ui::GlyphGrid::Options grid;
            grid.initial = _fields.icon;
            grid.height = _options.iconGridHeight;
            grid.strings.searchPlaceholder = _options.strings.searchPlaceholder;
            grid.strings.searchName = _options.strings.searchName;
            grid.strings.gridName = _options.strings.gridName;
            grid.strings.noIconName = _options.strings.noIconName;
            grid.strings.iconNamePrefix = _options.strings.iconNamePrefix;
            grid.onPick = [weak = weak_from_this()](const winrt::hstring& value) {
                if (const auto self = weak.lock())
                {
                    if (value != self->_fields.icon)
                    {
                        self->_fields.icon = value;
                        self->_refreshCurrentIcon();
                        self->RefreshPreview();
                        self->_notify();
                    }
                }
            };
            auto built = Axan::Ui::GlyphGrid::Build(std::move(grid));
            _glyphs = built.picker;

            Controls::StackPanel section;
            section.Spacing(6);
            section.Children().Append(top);
            section.Children().Append(built.root);
            return section;
        }

        // "No color" + the six AxanSessionPalette names + the custom swatch (#50), over the
        // custom entry. A palette swatch stores the name and fills with the active theme's
        // hex (#422); the custom swatch stores a literal "#rrggbb".
        winrt::Windows::UI::Xaml::Controls::StackPanel _buildSwatchRow()
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            Controls::StackPanel row;
            row.Orientation(Controls::Orientation::Horizontal);
            row.Spacing(8);

            const auto addSwatch = [&](const winrt::hstring& token) {
                Controls::Border el;
                el.Width(34);
                el.Height(34);
                el.CornerRadius(WUX::CornerRadius{ 5, 5, 5, 5 });
                _setSelected(el, token == _fields.color);
                if (token.empty())
                {
                    Controls::FontIcon fi;
                    fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
                    fi.Glyph(winrt::hstring{ L"\xE894" } /* Clear */);
                    fi.FontSize(14);
                    el.Child(fi);
                }
                else if (const auto c = Axan::SessionPalette::ParseHexColor(Axan::SessionPalette::ResolveToken(token, _options.isDark)))
                {
                    el.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ c->a, c->r, c->g, c->b } });
                    Controls::ToolTipService::SetToolTip(el, winrt::box_value(token));
                }
                el.Tapped([weak = weak_from_this(), token](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        // axan #50: a preset pick discards uncommitted custom-box text, so a
                        // later focus loss can't re-apply it over the preset.
                        if (self->_customBox && self->_customBox.Text() != self->_customColor)
                        {
                            self->_customBox.Text(self->_customColor);
                            self->_customError.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
                        }
                        if (token != self->_fields.color)
                        {
                            self->_fields.color = token;
                            self->_highlightSwatch();
                            self->RefreshPreview();
                            self->_notify();
                        }
                    }
                });
                _swatches.emplace_back(token, el);
                row.Children().Append(el);
            };
            addSwatch(winrt::hstring{});
            for (const auto& sw : Axan::SessionPalette::kDark) // names only; order = palette order
            {
                addSwatch(winrt::hstring{ sw.name });
            }

            // axan #50: the custom swatch. A Button (so it takes Tab focus and Enter/Space,
            // unlike the Tapped-only palette Borders) whose content is a Border of the same
            // size: hollow with a pencil glyph until a custom colour exists, then filled with
            // it. Clicking it re-applies that colour (if any) and opens the entry below.
            _customSwatch = Controls::Border{};
            _customSwatch.Width(34);
            _customSwatch.Height(34);
            _customSwatch.CornerRadius(WUX::CornerRadius{ 5, 5, 5, 5 });
            _customSwatchButton = Controls::Button{};
            _customSwatchButton.Padding(WUX::Thickness{ 0, 0, 0, 0 });
            _customSwatchButton.BorderThickness(WUX::Thickness{ 0, 0, 0, 0 });
            _customSwatchButton.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 0, 0, 0, 0 } });
            _customSwatchButton.MinWidth(0);
            _customSwatchButton.MinHeight(0);
            _customSwatchButton.Width(34);
            _customSwatchButton.Height(34);
            _customSwatchButton.CornerRadius(WUX::CornerRadius{ 5, 5, 5, 5 });
            _customSwatchButton.Content(_customSwatch);
            WUX::Automation::AutomationProperties::SetName(_customSwatchButton, _options.strings.customName);
            _customSwatchButton.Click([weak = weak_from_this()](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_customEntry.Visibility(WUX::Visibility::Visible);
                    if (!self->_customColor.empty() && self->_customColor != self->_fields.color)
                    {
                        self->_fields.color = self->_customColor;
                        self->_customBox.Text(self->_customColor);
                        self->_highlightSwatch();
                        self->RefreshPreview();
                        self->_notify();
                    }
                    // The entry may have been Collapsed a moment ago; if it can't take focus
                    // until the next layout pass, retry once from the dispatcher.
                    if (self->_customBox.Focus(WUX::FocusState::Programmatic))
                    {
                        self->_customBox.SelectAll();
                    }
                    else
                    {
                        self->_customBox.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weak]() {
                            if (const auto s = weak.lock())
                            {
                                s->_customBox.Focus(WUX::FocusState::Programmatic);
                                s->_customBox.SelectAll();
                            }
                        });
                    }
                }
            });
            row.Children().Append(_customSwatchButton);

            // The entry: [hex / rgb() box | live preview chip] over the inline error.
            _customBox = Controls::TextBox{};
            _customBox.Width(180);
            _customBox.AcceptsReturn(false);
            _customBox.PlaceholderText(_options.strings.customPlaceholder);
            _customBox.VerticalAlignment(WUX::VerticalAlignment::Center);
            WUX::Automation::AutomationProperties::SetName(_customBox, _options.strings.customBoxName);
            _customChip = Controls::Border{};
            _customChip.Width(24);
            _customChip.Height(24);
            _customChip.CornerRadius(WUX::CornerRadius{ 4, 4, 4, 4 });
            _customChip.BorderThickness(WUX::Thickness{ 1, 1, 1, 1 });
            _customChip.BorderBrush(_options.neutralBrush);
            _customChip.VerticalAlignment(WUX::VerticalAlignment::Center);
            _customBox.TextChanged([weak = weak_from_this()](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    // Live preview only; nothing is stored until commit.
                    const auto parsed = Axan::SessionPalette::ParseCustomColor(self->_customBox.Text());
                    const auto c = parsed ? Axan::SessionPalette::ParseHexColor(*parsed) : Axan::SessionPalette::ParseHexColor(self->_customBox.Text());
                    self->_customChip.Background(c ? WUX::Media::Brush{ WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ c->a, c->r, c->g, c->b } } } : WUX::Media::Brush{ nullptr });
                    self->_customError.Visibility(WUX::Visibility::Collapsed);
                }
            });
            _customBox.KeyDown([weak = weak_from_this()](auto&&, const WUX::Input::KeyRoutedEventArgs& e) {
                if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
                {
                    if (const auto self = weak.lock())
                    {
                        self->_commitCustomColor(true);
                    }
                    e.Handled(true); // don't let Enter reach the host (e.g. a dialog default)
                }
            });
            _customBox.LostFocus([weak = weak_from_this()](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_commitCustomColor(false);
                }
            });

            _customError = Controls::TextBlock{};
            _customError.Text(_options.strings.customError);
            _customError.Visibility(WUX::Visibility::Collapsed);
            _customError.TextWrapping(WUX::TextWrapping::Wrap);
            {
                winrt::Windows::UI::Xaml::Media::Brush err{ WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 255, 0xe8, 0x11, 0x23 } } };
                if (const auto b = WUX::Application::Current().Resources().TryLookup(winrt::box_value(winrt::hstring{ L"SystemControlErrorTextForegroundBrush" })))
                {
                    if (const auto brush = b.try_as<WUX::Media::Brush>())
                    {
                        err = brush;
                    }
                }
                _customError.Foreground(err);
            }
            // Narrator announces the error when it appears.
            WUX::Automation::AutomationProperties::SetLiveSetting(_customError, WUX::Automation::Peers::AutomationLiveSetting::Assertive);

            Controls::StackPanel entryRow;
            entryRow.Orientation(Controls::Orientation::Horizontal);
            entryRow.Spacing(8);
            entryRow.Children().Append(_customBox);
            entryRow.Children().Append(_customChip);

            _customEntry = Controls::StackPanel{};
            _customEntry.Spacing(4);
            _customEntry.Margin(WUX::Thickness{ 0, 6, 0, 0 });
            _customEntry.Children().Append(entryRow);
            _customEntry.Children().Append(_customError);

            // Seed: an existing "#hex" colour pre-fills and selects the custom swatch and
            // opens the entry with that value; a palette name or "" leaves it collapsed.
            if (_isCustomColor(_fields.color))
            {
                _customColor = _fields.color;
                _customBox.Text(_fields.color);
                _customEntry.Visibility(WUX::Visibility::Visible);
            }
            else
            {
                _customEntry.Visibility(WUX::Visibility::Collapsed);
            }
            _paintCustomSwatch();
            _setSelected(_customSwatch, _isCustomColor(_fields.color));

            Controls::StackPanel section;
            section.Spacing(0);
            section.Children().Append(row);
            section.Children().Append(_customEntry);
            return section;
        }

        // [caption + hint] over the Icon / Text / Both radios.
        winrt::Windows::UI::Xaml::Controls::StackPanel _buildColorTargetSection()
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            // A per-instance group, so two live editors (the node card and a Settings tab
            // can share one window's tree) never steal each other's selection.
            static std::atomic<uint32_t> s_instance{ 0 };
            const winrt::hstring group{ L"axanApplyColorTo" + std::to_wstring(++s_instance) };

            Controls::StackPanel radios;
            radios.Orientation(Controls::Orientation::Horizontal);
            radios.Spacing(18);
            const auto addRadio = [&](const winrt::hstring& text, const wchar_t* value) {
                Controls::RadioButton rb;
                rb.Content(winrt::box_value(text));
                rb.GroupName(group);
                const winrt::hstring v{ value };
                rb.IsChecked(_fields.colorTarget == v);
                rb.Checked([weak = weak_from_this(), v](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        if (v != self->_fields.colorTarget)
                        {
                            self->_fields.colorTarget = v;
                            self->RefreshPreview();
                            self->_notify();
                        }
                    }
                });
                radios.Children().Append(rb);
            };
            addRadio(_options.strings.targetIcon, L"icon");
            addRadio(_options.strings.targetText, L"text");
            addRadio(_options.strings.targetBoth, L"both");

            Controls::StackPanel labelRow;
            labelRow.Orientation(Controls::Orientation::Horizontal);
            labelRow.Spacing(6);
            labelRow.Margin(WUX::Thickness{ 0, 10, 0, 4 });
            {
                Controls::TextBlock t;
                t.Text(_options.strings.applyLabel);
                t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                t.VerticalAlignment(WUX::VerticalAlignment::Bottom);
                Controls::TextBlock h;
                h.Text(_options.strings.applyHint);
                h.Opacity(0.6);
                h.VerticalAlignment(WUX::VerticalAlignment::Bottom);
                h.TextWrapping(WUX::TextWrapping::Wrap);
                labelRow.Children().Append(t);
                labelRow.Children().Append(h);
            }

            Controls::StackPanel section;
            section.Spacing(2);
            section.Children().Append(labelRow);
            section.Children().Append(radios);
            return section;
        }

        void _build()
        {
            _root = winrt::Windows::UI::Xaml::Controls::StackPanel{};
            _root.Spacing(2);
            _root.Children().Append(_buildNameSection());
            _root.Children().Append(_sectionLabel(_options.strings.iconLabel));
            _root.Children().Append(_buildIconSection());
            _root.Children().Append(_sectionLabel(_options.strings.colorLabel));
            _root.Children().Append(_buildSwatchRow());
            _root.Children().Append(_buildColorTargetSection());
            RefreshPreview();
            Axan::Log::Debug("IdentityEditor", "built identity editor", { { "icon", winrt::to_string(_fields.icon) }, { "color", winrt::to_string(_fields.color) }, { "colorTarget", winrt::to_string(_fields.colorTarget) } });
        }

        Options _options;
        Fields _fields;
        winrt::Windows::UI::Xaml::Controls::StackPanel _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _nameBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ContentPresenter _preview{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _currentIcon{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _clearIcon{ nullptr };
        std::shared_ptr<Axan::Ui::GlyphGrid::Picker> _glyphs; // holds only weak XAML refs
        std::vector<std::pair<winrt::hstring, winrt::Windows::UI::Xaml::Controls::Border>> _swatches;
        // axan #50: the custom swatch and its entry. _customColor is the last committed custom
        // "#hex" (kept while a preset is selected, so clicking the swatch re-applies it).
        winrt::hstring _customColor;
        winrt::Windows::UI::Xaml::Controls::Button _customSwatchButton{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _customSwatch{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _customEntry{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _customBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _customChip{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _customError{ nullptr };
    };

    // Builds the section. Never calls options.onChanged. Hold the result while the UI is up.
    inline std::shared_ptr<Editor> Build(Options options)
    {
        return Editor::Create(std::move(options));
    }
}

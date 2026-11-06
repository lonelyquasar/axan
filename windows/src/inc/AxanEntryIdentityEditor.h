// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanEntryIdentityEditor — the shared "identity" section of a session editor (#38): the
// name / label template field with its variables help, an icon preview, the icon grid +
// "Browse to icon file…", the palette colour swatches, and the colour target radio
// (icon / text / both).
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
    // glyph, a file path, an emoji; "" = no override, the session's own icon); color = a
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
        winrt::hstring iconNamePrefix{ L"Icon: " }; // + the builtin's portable name
        winrt::hstring browseText{ L"Browse to icon file…" };
        winrt::hstring browseName{ L"Browse to icon file" };
        winrt::hstring colorLabel{ L"Color" };
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

        // Sets the icon from outside the grid (a Browse result). Re-highlights the grid (no
        // cell matches a file path), refreshes the preview and notifies onChanged.
        void SetIcon(const winrt::hstring& icon)
        {
            if (icon == _fields.icon)
            {
                return;
            }
            _fields.icon = icon;
            _highlightIcon();
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

        void _highlightIcon() const
        {
            for (const auto& [value, button] : _iconCells)
            {
                _setSelected(button, value == _fields.icon);
            }
        }

        void _clearIconHighlight() const
        {
            for (const auto& cell : _iconCells)
            {
                _setSelected(cell.second, false);
            }
        }

        void _highlightSwatch() const
        {
            for (const auto& [token, border] : _swatches)
            {
                _setSelected(border, token == _fields.color);
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

        // The icon grid ("No icon" + AxanIconRegistry's 11 builtins, 6 per row, so every
        // pickable glyph round-trips to a portable builtin:NAME token) beside the Browse button.
        winrt::Windows::UI::Xaml::Controls::Grid _buildIconRow()
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace Controls = winrt::Windows::UI::Xaml::Controls;
            const auto mdl2 = WUX::Media::FontFamily{ L"Segoe MDL2 Assets" };

            Controls::VariableSizedWrapGrid grid;
            grid.Orientation(Controls::Orientation::Horizontal);
            grid.MaximumRowsOrColumns(6);
            grid.ItemWidth(42);
            grid.ItemHeight(42);

            const auto addCell = [&](const winrt::hstring& glyph, const winrt::hstring& name, const winrt::hstring& storedValue) {
                Controls::Button b;
                b.Width(36);
                b.Height(36);
                b.Padding(WUX::Thickness{ 0, 0, 0, 0 });
                _setSelected(b, storedValue == _fields.icon);
                // axan #429/#436: icon-only buttons; the registry's name names them.
                WUX::Automation::AutomationProperties::SetName(b, winrt::hstring{ std::wstring{ _options.strings.iconNamePrefix } + std::wstring{ name } });
                Controls::ToolTipService::SetToolTip(b, winrt::box_value(name));
                Controls::FontIcon fi;
                fi.FontFamily(mdl2);
                fi.Glyph(glyph);
                b.Content(fi);
                b.Click([weak = weak_from_this(), storedValue](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        if (storedValue != self->_fields.icon)
                        {
                            self->_fields.icon = storedValue;
                            self->_highlightIcon();
                            self->RefreshPreview();
                            self->_notify();
                        }
                        else
                        {
                            self->_highlightIcon(); // re-assert after a Browse cleared it
                        }
                    }
                });
                _iconCells.emplace_back(storedValue, b);
                grid.Children().Append(b);
            };
            // "No icon" clears the override back to the session's own icon. The name stays
            // "Icon: No icon" for Narrator, as the live editor always announced it.
            addCell(winrt::hstring{ L"\xE711" } /* Cancel */, _options.strings.noIconName, winrt::hstring{});
            for (const auto& builtin : Axan::IconRegistry::Builtins())
            {
                const winrt::hstring glyph{ std::wstring(1, builtin.glyph) };
                addCell(glyph, winrt::hstring{ builtin.name }, glyph);
            }

            Controls::Button browse;
            {
                Controls::StackPanel bc;
                bc.Orientation(Controls::Orientation::Horizontal);
                bc.Spacing(7);
                Controls::FontIcon fi;
                fi.FontFamily(mdl2);
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
                    self->_clearIconHighlight();
                    if (self->_options.onBrowseIcon)
                    {
                        self->_options.onBrowseIcon();
                    }
                }
            });

            Controls::Grid row;
            Controls::ColumnDefinition c0;
            c0.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
            Controls::ColumnDefinition c1;
            c1.Width(WUX::GridLengthHelper::FromValueAndType(0, WUX::GridUnitType::Auto));
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            row.ColumnSpacing(8);
            Controls::Grid::SetColumn(grid, 0);
            Controls::Grid::SetColumn(browse, 1);
            row.Children().Append(grid);
            row.Children().Append(browse);
            return row;
        }

        // "No color" + the six AxanSessionPalette names. A swatch stores the name and fills
        // with the active theme's hex (#422).
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
            return row;
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
            _root.Children().Append(_buildIconRow());
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
        std::vector<std::pair<winrt::hstring, winrt::Windows::UI::Xaml::Controls::Button>> _iconCells;
        std::vector<std::pair<winrt::hstring, winrt::Windows::UI::Xaml::Controls::Border>> _swatches;
    };

    // Builds the section. Never calls options.onChanged. Hold the result while the UI is up.
    inline std::shared_ptr<Editor> Build(Options options)
    {
        return Editor::Create(std::move(options));
    }
}

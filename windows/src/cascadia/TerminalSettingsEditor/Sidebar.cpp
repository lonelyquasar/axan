// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// axan #38: Settings > Sessions > Sidebar. The code-behind moved here from GlobalAppearance.

#include "pch.h"
#include "Sidebar.h"
#include "Sidebar.g.cpp"
#include "SidebarViewModel.h" // IdleEffectLabel / IdleEffectHelp

#include <LibraryResources.h>
#include "..\WinRTUtils\inc\Utils.h" // OpenImagePicker
#include <AxanIconPicker.h> // the shared icon picker flyout body
#include <AxanStateGlyphs.h> // sidebar.stateGlyphs defaults
#include <AxanCheckBoxWrap.h> // full-label columns for the hoist-state checkboxes
#include <AxanHoistRule.h> // axan #34: sidebar.hoistStates vocabulary
#include <AxanIdleEffects.h> // axan: sidebar.idleEffects vocabulary
#include <AxanGlyphScale.h> // axan: sidebar.iconFontScale range

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    Sidebar::Sidebar()
    {
        InitializeComponent();
    }

    void Sidebar::OnNavigatedTo(const NavigationEventArgs& e)
    {
        _ViewModel = e.Parameter().as<Editor::SidebarViewModel>();
        _BuildHoistStateBoxes(); // axan #34
        _BuildIdleEffectBoxes();
        _BuildIconFontRows();
    }

    // axan: sidebar.iconFontScale. One row per family: its name, an inline spin box (0.5-3,
    // steps of 0.05, two decimals) and a Reset button shown while the family has an override.
    // Each change writes the map at once, like the rest of the page.
    void Sidebar::_BuildIconFontRows()
    {
        namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;
        namespace WUX = winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::Globalization::NumberFormatting;
        const auto panel = IconFontsPanel();
        const auto vm = _ViewModel;
        if (!panel || !vm)
        {
            return;
        }
        panel.Children().Clear();
        for (const auto& family : vm.SidebarIconFontFamilies())
        {
            Grid row;
            row.ColumnSpacing(12);
            for (const auto width : { WUX::GridLength{ 1, WUX::GridUnitType::Star }, WUX::GridLength{ 1, WUX::GridUnitType::Auto }, WUX::GridLength{ 1, WUX::GridUnitType::Auto } })
            {
                ColumnDefinition column;
                column.Width(width);
                row.ColumnDefinitions().Append(column);
            }

            TextBlock name;
            name.Text(family);
            name.VerticalAlignment(WUX::VerticalAlignment::Center);
            name.TextTrimming(WUX::TextTrimming::CharacterEllipsis);
            row.Children().Append(name);

            MUXC::NumberBox box;
            box.Minimum(Axan::GlyphScale::kMinScale);
            box.Maximum(Axan::GlyphScale::kMaxScale);
            box.SmallChange(0.05);
            box.LargeChange(0.25);
            box.SpinButtonPlacementMode(MUXC::NumberBoxSpinButtonPlacementMode::Inline);
            DecimalFormatter formatter;
            formatter.IntegerDigits(1);
            formatter.FractionDigits(2);
            IncrementNumberRounder rounder;
            rounder.Increment(0.01);
            formatter.NumberRounder(rounder);
            box.NumberFormatter(formatter);
            box.Value(vm.SidebarIconFontScale(family));
            Automation::AutomationProperties::SetName(box, family);
            Grid::SetColumn(box, 1);
            row.Children().Append(box);

            Button reset;
            reset.Content(winrt::box_value(RS_(L"Globals_SidebarIconFontScaleReset")));
            reset.VerticalAlignment(WUX::VerticalAlignment::Center);
            reset.Visibility(vm.IsSidebarIconFontScaleSet(family) ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
            Automation::AutomationProperties::SetName(reset, RS_fmt(L"Globals_SidebarIconFontScaleResetName", std::wstring_view{ family }));
            Grid::SetColumn(reset, 2);
            row.Children().Append(reset);

            box.ValueChanged([vm, family, weakReset = winrt::make_weak(reset)](const MUXC::NumberBox&, const MUXC::NumberBoxValueChangedEventArgs& args) {
                const auto value = args.NewValue();
                if (!(value == value))
                {
                    return; // a cleared box; NumberBox puts the last value back
                }
                vm.SetSidebarIconFontScale(family, value);
                if (const auto r = weakReset.get())
                {
                    r.Visibility(vm.IsSidebarIconFontScaleSet(family) ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
                }
            });
            reset.Click([vm, family, weakBox = winrt::make_weak(box)](const IInspectable& sender, const RoutedEventArgs&) {
                vm.ResetSidebarIconFontScale(family);
                if (const auto b = weakBox.get())
                {
                    b.Value(vm.SidebarIconFontScale(family));
                }
                if (const auto r = sender.try_as<Button>())
                {
                    r.Visibility(WUX::Visibility::Collapsed);
                }
            });
            panel.Children().Append(row);
        }
    }

    // axan: one checkbox per idle effect, canonical order, each with its help as the tooltip
    // and the accessible help text. Each toggle writes sidebar.idleEffects at once.
    void Sidebar::_BuildIdleEffectBoxes()
    {
        const auto panel = IdleEffectsPanel();
        const auto vm = _ViewModel;
        if (!panel || !vm)
        {
            return;
        }
        panel.Children().Clear();
        for (const auto name : Axan::IdleEffects::kNames)
        {
            CheckBox box;
            const winrt::hstring key{ name };
            const auto help = implementation::SidebarViewModel::IdleEffectHelp(name);
            box.Content(winrt::box_value(implementation::SidebarViewModel::IdleEffectLabel(name)));
            ToolTipService::SetToolTip(box, winrt::box_value(help));
            Automation::AutomationProperties::SetHelpText(box, help);
            box.IsChecked(vm.SidebarIdleEffectEnabled(key));
            box.Click([vm, key](const IInspectable& s, const RoutedEventArgs&) {
                if (const auto b = s.try_as<CheckBox>())
                {
                    vm.SetSidebarIdleEffectEnabled(key, b.IsChecked() && b.IsChecked().Value());
                }
            });
            panel.Children().Append(box);
        }
        Axan::Ui::CheckBoxWrap::FitItemWidth(panel);
    }

    void Sidebar::IdleEffectsReset_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (_ViewModel)
        {
            _ViewModel.ResetSidebarIdleEffects();
            _BuildIdleEffectBoxes();
        }
    }

    // axan #34: one checkbox per hoist state name, labelled with the words the row's state
    // slot uses. Each toggle writes sidebar.hoistStates at once.
    void Sidebar::_BuildHoistStateBoxes()
    {
        const auto panel = HoistStatesPanel();
        const auto vm = _ViewModel;
        if (!panel || !vm)
        {
            return;
        }
        panel.Children().Clear();
        for (const auto name : Axan::HoistRule::kDisplayOrder)
        {
            CheckBox box;
            const winrt::hstring key{ name };
            box.Content(winrt::box_value(winrt::hstring{ Axan::HoistRule::DisplayName(name) }));
            box.IsChecked(vm.SidebarHoistStateEnabled(key));
            box.Click([vm, key](const IInspectable& s, const RoutedEventArgs&) {
                if (const auto b = s.try_as<CheckBox>())
                {
                    vm.SetSidebarHoistStateEnabled(key, b.IsChecked() && b.IsChecked().Value());
                }
            });
            panel.Children().Append(box);
        }
        // Columns as wide as the longest label; the boxes wrap onto more rows as needed.
        Axan::Ui::CheckBoxWrap::FitItemWidth(panel);
    }

    void Sidebar::HoistStatesReset_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (_ViewModel)
        {
            _ViewModel.ResetSidebarHoistStates();
            _BuildHoistStateBoxes();
        }
    }

    // The implementer doc for OSC 7501 (the Program Status Protocol) and its fallbacks,
    // which the status glyphs below reflect. docs/program-status-osc-7501.md on main; the
    // README's "Session state in the sidebar" section is the short form and links there.
    static constexpr std::wstring_view kStatusGlyphsDocsUrl{ L"https://github.com/lonelyquasar/axan/blob/main/docs/program-status-osc-7501.md" };

    safe_void_coroutine Sidebar::StatusGlyphsInfo_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        auto lifetime = get_strong();
        const winrt::Windows::Foundation::Uri uri{ kStatusGlyphsDocsUrl };
        co_await winrt::Windows::System::Launcher::LaunchUriAsync(uri);
    }

    // axan: the same picker the Startup page's icon button opens, led by a "Default" cell
    // (the state's built-in glyph; removes the key). "No icon" stores "" (show nothing for
    // that state). Every choice applies immediately and dismisses.
    void Sidebar::StateGlyphFlyout_Opening(const IInspectable& sender, const IInspectable& /*args*/)
    {
        const auto flyout = sender.try_as<Flyout>();
        const auto vm = _ViewModel;
        if (!flyout || !vm)
        {
            return;
        }
        const auto anchor = flyout.Target() ? flyout.Target().try_as<FrameworkElement>() : nullptr;
        const auto key = anchor ? winrt::unbox_value_or<winrt::hstring>(anchor.Tag(), winrt::hstring{}) : winrt::hstring{};
        if (!Axan::StateGlyphs::IsKey(key))
        {
            return;
        }
        // The Default cell shows the state's default glyph, or Undo when the default is none.
        const auto fallback = Axan::StateGlyphs::DefaultFor(key);
        const winrt::hstring defaultGlyph{ fallback.empty() ? std::wstring_view{ L"\uE7A7" } : fallback };
        auto weakFlyout = winrt::make_weak(flyout);
        const auto hide = [weakFlyout]() {
            if (const auto f = weakFlyout.get())
            {
                f.Hide();
            }
        };

        Axan::Ui::IconPicker::Options options;
        options.leading.push_back({ defaultGlyph, RS_(L"Globals_SidebarStateGlyphDefault"), [vm, key, hide]() {
                                       vm.ResetSidebarStateGlyph(key);
                                       hide();
                                   } });
        options.noneName = RS_(L"Globals_SidebarStateGlyphNone");
        options.currentText = vm.SidebarStateGlyphText(key);
        options.onPick = [vm, key, hide](const winrt::hstring& icon) {
            vm.SetSidebarStateGlyph(key, icon);
            hide();
        };
        options.onBrowse = [weak = get_weak(), key, weakFlyout]() {
            if (const auto self = weak.get())
            {
                self->_PickStateGlyphImage(key, weakFlyout);
            }
        };
        flyout.Content(Axan::Ui::IconPicker::Build(options));
    }

    safe_void_coroutine Sidebar::_PickStateGlyphImage(winrt::hstring key, winrt::weak_ref<Flyout> weakFlyout)
    {
        auto lifetime = get_strong();

        // OpenImagePicker is the shared image-file dialog the Profiles page's icon Browse uses.
        const auto parentHwnd{ ::GetActiveWindow() };
        const auto file = co_await OpenImagePicker(parentHwnd);
        if (!file.empty() && _ViewModel)
        {
            _ViewModel.SetSidebarStateGlyph(key, file);
        }
        if (const auto f = weakFlyout.get())
        {
            f.Hide();
        }
    }
}

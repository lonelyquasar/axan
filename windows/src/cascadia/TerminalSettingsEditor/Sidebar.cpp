// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// axan #38: Settings > Sessions > Sidebar. The code-behind moved here from GlobalAppearance.

#include "pch.h"
#include "Sidebar.h"
#include "Sidebar.g.cpp"

#include <LibraryResources.h>
#include "..\WinRTUtils\inc\Utils.h" // OpenImagePicker
#include <AxanIconPicker.h> // the shared icon picker flyout body
#include <AxanStateGlyphs.h> // sidebar.stateGlyphs defaults
#include <AxanCheckBoxWrap.h> // full-label columns for the hoist-state checkboxes
#include <AxanHoistRule.h> // axan #34: sidebar.hoistStates vocabulary

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

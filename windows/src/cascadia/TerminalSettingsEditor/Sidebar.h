// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// axan #38: Settings > Sessions > Sidebar, the sidebar.* globals (moved from Appearance).

#pragma once

#include "Sidebar.g.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct Sidebar : public HasScrollViewer<Sidebar>, SidebarT<Sidebar>
    {
    public:
        Sidebar();

        void OnNavigatedTo(const winrt::Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        // sidebar.stateGlyphs. Each row's button carries its state key in Tag; the flyout
        // body (AxanIconPicker.h) is rebuilt on each Opening.
        void StateGlyphFlyout_Opening(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args);

        // axan #34: sidebar.hoistStates. The checkboxes are built in code-behind (one per
        // AxanHoistRule.h name) and re-synced after a reset.
        void HoistStatesReset_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& args);

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(Editor::SidebarViewModel, ViewModel, PropertyChanged.raise, nullptr);

    private:
        void _BuildHoistStateBoxes();
        safe_void_coroutine _PickStateGlyphImage(winrt::hstring key, winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Flyout> weakFlyout);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(Sidebar);
}

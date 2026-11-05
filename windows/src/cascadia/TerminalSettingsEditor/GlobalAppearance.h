// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GlobalAppearance.g.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct GlobalAppearance : public HasScrollViewer<GlobalAppearance>, GlobalAppearanceT<GlobalAppearance>
    {
    public:
        GlobalAppearance();

        void OnNavigatedTo(const winrt::Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        // axan: sidebar.stateGlyphs. Each row's button carries its key ("working" / "running"
        // / "presence") in Tag; the flyout body (AxanIconPicker.h) is rebuilt on each Opening.
        void StateGlyphFlyout_Opening(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args);

        // axan #34: sidebar.hoistStates. The checkboxes are built in code-behind (one per
        // AxanHoistRule.h name) and re-synced after a reset.
        void HoistStatesReset_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& args);

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(Editor::GlobalAppearanceViewModel, ViewModel, PropertyChanged.raise, nullptr);

    private:
        void _BuildHoistStateBoxes();
        safe_void_coroutine _PickStateGlyphImage(winrt::hstring key, winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Flyout> weakFlyout);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(GlobalAppearance);
}

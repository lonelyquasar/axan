#pragma once

#include "IconPathConverter.g.h"

namespace winrt::Microsoft::Terminal::UI::implementation
{
    struct IconPathConverter
    {
        IconPathConverter() = default;

        static Windows::UI::Xaml::Controls::IconElement IconWUX(const winrt::hstring& iconPath);
        static Windows::UI::Xaml::Controls::IconSource IconSourceWUX(const winrt::hstring& iconPath);
        static Microsoft::UI::Xaml::Controls::IconSource IconSourceMUX(const winrt::hstring& iconPath, bool convertToGrayscale);
        static uint64_t SetIconFontScales(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, double>& scales);
        static uint64_t SetIconFontWeights(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring>& weights);
    };
}

namespace winrt::Microsoft::Terminal::UI::factory_implementation
{
    BASIC_FACTORY(IconPathConverter);
}

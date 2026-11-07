// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "App.h"
#include "App.g.cpp"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Navigation;

namespace xaml = ::winrt::Windows::UI::Xaml;

namespace winrt::TerminalApp::implementation
{
    App::App()
    {
        Initialize();

        // Disable XAML's automatic backplating of text when in High Contrast
        // mode: we want full control of and responsibility for the foreground
        // and background colors that we draw in XAML.
        HighContrastAdjustment(::winrt::Windows::UI::Xaml::ApplicationHighContrastAdjustment::None);

        // axan: backstop for clipboard failures escaping XAML event handlers (a
        // selectable TextBlock's built-in Ctrl+C, a search box's Ctrl+C, ...).
        // CLIPBRD_E_CANT_OPEN just means another process held the clipboard at
        // that instant; unhandled, XAML turns it into a fail-fast of the whole
        // process. The copy is lost, the app is not. Everything else stays fatal.
        UnhandledException([](const IInspectable&, const UnhandledExceptionEventArgs& e) {
            const auto hr = static_cast<HRESULT>(e.Exception());
            if (hr >= CLIPBRD_E_FIRST && hr <= CLIPBRD_E_LAST)
            {
                LOG_HR_MSG(hr, "Swallowed unhandled clipboard error");
                e.Handled(true);
            }
        });
    }

    void App::Initialize()
    {
        // LOAD BEARING
        AddOtherProvider(winrt::Microsoft::Terminal::Control::XamlMetaDataProvider{});
        AddOtherProvider(winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider{});

        const auto dispatcherQueue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
        if (!dispatcherQueue)
        {
            _windowsXamlManager = xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        }
        else
        {
            FAIL_FAST_MSG("Terminal is not intended to run as a Universal Windows Application");
        }
    }

    AppLogic App::Logic()
    {
        static AppLogic logic;
        return logic;
    }

    /// <summary>
    /// Invoked when the application is launched normally by the end user.  Other entry points
    /// will be used such as when the application is launched to open a specific file.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched(const LaunchActivatedEventArgs& /*e*/)
    {
        // We used to support a pure UWP version of the Terminal. This method
        // was only ever used to do UWP-specific setup of our App.
    }

    void App::PrepareForSettingsUI()
    {
        if (!std::exchange(_preparedForSettingsUI, true))
        {
            AddOtherProvider(winrt::Microsoft::Terminal::Settings::Editor::XamlMetaDataProvider{});
        }
    }
}

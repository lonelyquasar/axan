// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "StartupSessions.h"
#include "StartupSessions.g.cpp"

#include <LibraryResources.h>
#include "..\WinRTUtils\inc\Utils.h"
#include "../../types/inc/utils.hpp" // GuidToString
#include <AxanIconRegistry.h> // builtin glyph vocabulary for the icon picker flyout (#438)

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Navigation;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    StartupSessions::StartupSessions()
    {
        InitializeComponent();
    }

    void StartupSessions::OnNavigatedTo(const NavigationEventArgs& e)
    {
        _ViewModel = e.Parameter().as<Editor::StartupSessionsViewModel>();
    }

    // Each per-row button carries its LaunchEntryViewModel in Tag, so the handler targets that row.
    Editor::LaunchEntryViewModel StartupSessions::_entryFromSender(const IInspectable& sender)
    {
        if (const auto element = sender.try_as<FrameworkElement>())
        {
            return element.Tag().try_as<Editor::LaunchEntryViewModel>();
        }
        return nullptr;
    }

    void StartupSessions::AddEntry_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (_ViewModel)
        {
            _ViewModel.AddEntry();
        }
    }

    void StartupSessions::DeleteEntry_Click(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        if (const auto vm = _entryFromSender(sender); vm && _ViewModel)
        {
            _ViewModel.DeleteEntry(vm);
        }
    }

    void StartupSessions::DuplicateEntry_Click(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        if (const auto vm = _entryFromSender(sender); vm && _ViewModel)
        {
            _ViewModel.DuplicateEntry(vm);
        }
    }

    void StartupSessions::IndentEntry_Click(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        if (const auto vm = _entryFromSender(sender); vm && _ViewModel)
        {
            _ViewModel.IndentEntry(vm);
        }
    }

    void StartupSessions::OutdentEntry_Click(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        if (const auto vm = _entryFromSender(sender); vm && _ViewModel)
        {
            _ViewModel.OutdentEntry(vm);
        }
    }

    safe_void_coroutine StartupSessions::Import_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        auto lifetime = get_strong();

        static constexpr COMDLG_FILTERSPEC supportedFileTypes[] = {
            { L"axan profile (*.toml)", L"*.toml" },
            { L"All Files (*.*)", L"*.*" }
        };
        static constexpr winrt::guid clientGuidTomlImport{ 0x6b6f3a21, 0x4f2c, 0x4a5e, { 0x9c, 0x1d, 0x2a, 0x7b, 0x55, 0x90, 0x3e, 0x44 } };

        // The settings window is this thread's active window — a valid file-dialog owner.
        const auto parentHwnd{ ::GetActiveWindow() };
        const auto path = co_await OpenFilePicker(parentHwnd, [](auto&& dialog) {
            THROW_IF_FAILED(dialog->SetClientGuid(clientGuidTomlImport));
            THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(supportedFileTypes), supportedFileTypes));
            THROW_IF_FAILED(dialog->SetFileTypeIndex(1));
            THROW_IF_FAILED(dialog->SetDefaultExtension(L"toml"));
        });

        if (!path.empty() && _ViewModel)
        {
            // Surface a failed import inline (#435 item 2): the VM logs the detailed
            // reason to axan.log; the bar tells the user it happened and that the list
            // was left untouched. A successful import dismisses a stale bar.
            const auto ok = _ViewModel.ImportFromToml(path);
            ImportFailureBar().IsOpen(!ok);
        }
    }

    // axan #438: the icon is picked, not typed. Build the flyout's content fresh on each open
    // (in code — the XAML-side classic-Binding limitation noted on the profile combo applies
    // here too): the builtin-glyph vocabulary from AxanIconRegistry (everything it offers
    // round-trips to a portable builtin:NAME token), a none/clear cell, an image-file browse,
    // and a free-text row for emoji or any literal icon string.
    void StartupSessions::IconFlyout_Opening(const IInspectable& sender, const IInspectable& /*args*/)
    {
        const auto flyout = sender.try_as<Controls::Flyout>();
        if (!flyout)
        {
            return;
        }
        const auto anchor = flyout.Target() ? flyout.Target().try_as<FrameworkElement>() : nullptr;
        const auto vm = anchor ? anchor.Tag().try_as<Editor::LaunchEntryViewModel>() : nullptr;
        if (!vm)
        {
            return;
        }
        auto weakFlyout = winrt::make_weak(flyout);
        const auto hide = [weakFlyout]() {
            if (const auto f = weakFlyout.get())
            {
                f.Hide();
            }
        };

        Controls::StackPanel root;
        root.Spacing(8);
        root.MaxWidth(260);

        // Row 1: the glyph grid — a "none" cell, then the 11 builtins.
        Controls::VariableSizedWrapGrid grid;
        grid.Orientation(Controls::Orientation::Horizontal);
        grid.MaximumRowsOrColumns(6);
        grid.ItemWidth(40);
        grid.ItemHeight(40);

        const auto addCell = [&](const winrt::hstring& glyph, const winrt::hstring& name, const winrt::hstring& storedValue) {
            Controls::Button cell;
            Controls::FontIcon icon;
            icon.FontSize(16);
            icon.Glyph(glyph);
            cell.Content(icon);
            cell.HorizontalAlignment(HorizontalAlignment::Stretch);
            cell.VerticalAlignment(VerticalAlignment::Stretch);
            Automation::AutomationProperties::SetName(cell, name);
            Controls::ToolTipService::SetToolTip(cell, winrt::box_value(name));
            cell.Click([vm, storedValue, hide](auto&&, auto&&) {
                vm.Icon(storedValue);
                hide();
            });
            grid.Children().Append(cell);
        };

        addCell(L"\uE711", L"No icon", L"" /* clear -> the neutral placeholder */);
        for (const auto& builtin : Axan::IconRegistry::Builtins())
        {
            const winrt::hstring glyph{ std::wstring_view{ &builtin.glyph, 1 } };
            addCell(glyph, winrt::hstring{ builtin.name }, glyph);
        }
        root.Children().Append(grid);

        // Row 2: browse for an image file.
        Controls::Button browse;
        browse.Content(winrt::box_value(L"Browse for an image…"));
        browse.HorizontalAlignment(HorizontalAlignment::Stretch);
        browse.Click([weak = get_weak(), vm, weakFlyout](auto&&, auto&&) {
            if (const auto self = weak.get())
            {
                self->_PickIconImage(vm, weakFlyout);
            }
        });
        root.Children().Append(browse);

        // Row 3: free text (emoji, a builtin:NAME token, or any literal path). Enter applies.
        Controls::TextBox freeText;
        freeText.PlaceholderText(L"emoji or builtin:NAME — Enter applies");
        freeText.Text(vm.IconPortable());
        Automation::AutomationProperties::SetName(freeText, L"Icon text");
        freeText.KeyDown([vm, hide](const IInspectable& s, const Input::KeyRoutedEventArgs& e) {
            if (e.Key() == Windows::System::VirtualKey::Enter)
            {
                if (const auto box = s.try_as<Controls::TextBox>())
                {
                    vm.IconPortable(box.Text());
                }
                e.Handled(true);
                hide();
            }
        });
        root.Children().Append(freeText);

        flyout.Content(root);
    }

    safe_void_coroutine StartupSessions::_PickIconImage(Editor::LaunchEntryViewModel vm, winrt::weak_ref<Controls::Flyout> weakFlyout)
    {
        auto lifetime = get_strong();

        // Same owner-window choice as Import_Click above; OpenImagePicker is the shared
        // image-file dialog the Profiles page's icon Browse uses.
        const auto parentHwnd{ ::GetActiveWindow() };
        const auto file = co_await OpenImagePicker(parentHwnd);
        if (!file.empty() && vm)
        {
            vm.Icon(file);
        }
        if (const auto f = weakFlyout.get())
        {
            f.Hide();
        }
    }

    // The ComboBox shows Model::Profile objects; the row stores a GUID string. On load, select the
    // item whose GUID matches the row's Profile so the current shell shows.
    void StartupSessions::ProfileCombo_Loaded(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        const auto combo = sender.try_as<ComboBox>();
        if (!combo || !_ViewModel)
        {
            return;
        }
        const auto vm = combo.Tag().try_as<Editor::LaunchEntryViewModel>();
        const auto profiles = _ViewModel.AvailableProfiles();
        if (!vm || !profiles)
        {
            return;
        }
        // Populate here, not in markup: the XAML's old {Binding ElementName=RootPage}
        // ItemsSource silently resolved to null (a C++/WinRT page has no
        // ICustomPropertyProvider for classic Binding paths), leaving every combo empty.
        _profileComboLoading = true;
        combo.ItemsSource(profiles);
        _profileComboLoading = false;
        // An empty Profile means "the default profile" — show that selection. Setting
        // SelectedItem fires SelectionChanged synchronously; the loading flag keeps that
        // programmatic selection from being committed as if the user picked it. Without
        // it, merely visiting the page would rewrite every follow-the-default entry
        // (empty Profile, a real D19 state) to a pinned copy of the CURRENT default's
        // GUID — silently breaking its tracking of future default changes (#435 item 1).
        const auto target = vm.Profile().empty() ? _ViewModel.DefaultProfileGuid() : vm.Profile();
        for (const auto& p : profiles)
        {
            if (winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(p.Guid()) } == target)
            {
                _profileComboLoading = true;
                combo.SelectedItem(p);
                _profileComboLoading = false;
                break;
            }
        }
    }

    void StartupSessions::ProfileCombo_SelectionChanged(const IInspectable& sender, const Controls::SelectionChangedEventArgs& /*args*/)
    {
        if (_profileComboLoading)
        {
            // The select-on-load sync in ProfileCombo_Loaded, not a user pick — never a
            // commit. (A user can't "pin" the default by re-selecting it: the combo
            // already shows it, so an identical selection fires no SelectionChanged.)
            return;
        }
        const auto combo = sender.try_as<ComboBox>();
        if (!combo)
        {
            return;
        }
        const auto vm = combo.Tag().try_as<Editor::LaunchEntryViewModel>();
        if (!vm)
        {
            return;
        }
        if (const auto p = combo.SelectedItem().try_as<winrt::Microsoft::Terminal::Settings::Model::Profile>())
        {
            const auto g = winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(p.Guid()) };
            // Only write on a real change. NB: this guard alone does NOT cover the
            // select-on-load case — a follow-the-default row stores "" while the combo
            // selects the default's GUID — hence the loading flag above.
            if (g != vm.Profile())
            {
                vm.Profile(g);
            }
        }
    }
}

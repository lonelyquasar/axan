// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "StartupSessions.h"
#include "StartupSessions.g.cpp"

#include <LibraryResources.h>
#include "..\WinRTUtils\inc\Utils.h"
#include "../../types/inc/utils.hpp" // GuidToString
#include <AxanEntryIdentityEditor.h> // axan #38: the identity section shared with the sidebar's node card
#include <AxanIconRegistry.h> // builtin:NAME <-> glyph for the icon text box
#include <AxanLog.h>
#include <AxanSessionPalette.h> // resolve the preview's colour token

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Navigation;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    namespace IdentityEditor = Axan::Ui::IdentityEditor;

    StartupSessions::StartupSessions()
    {
        InitializeComponent();

        // axan #38: accelerators are global by default; scope them to the list so Delete,
        // Ctrl+D and Alt+arrows typed in the detail panel's fields never act on the rows.
        const auto list = EntriesList();
        for (const auto& accelerator : list.KeyboardAccelerators())
        {
            accelerator.ScopeOwner(list);
        }
    }

    void StartupSessions::OnNavigatedTo(const NavigationEventArgs& e)
    {
        _ViewModel = e.Parameter().as<Editor::StartupSessionsViewModel>();
        _vmChangedRevoker = _ViewModel.PropertyChanged(winrt::auto_revoke, { get_weak(), &StartupSessions::_onViewModelPropertyChanged });
        // The list and the detail panel sync on EntriesList_Loaded: x:Bind hasn't set the
        // ListView's ItemsSource yet.
    }

    Editor::LaunchEntryViewModel StartupSessions::_selected() const
    {
        return _ViewModel ? _ViewModel.SelectedEntry() : nullptr;
    }

    void StartupSessions::_onViewModelPropertyChanged(const IInspectable& /*sender*/, const Data::PropertyChangedEventArgs& args)
    {
        // Raised on every pick and after every structural op (even for the same row).
        if (args.PropertyName() == L"SelectedEntry")
        {
            _syncListSelection();
            _rebuildDetail();
        }
    }

    template<typename F>
    auto StartupSessions::_runOp(F&& op)
    {
        _ignoreListSelection = true;
        const auto restore = wil::scope_exit([&]() noexcept { _ignoreListSelection = false; });
        return op();
    }

    // ---- list <-> selection ----

    void StartupSessions::EntriesList_Loaded(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        _syncListSelection();
        _rebuildDetail();
    }

    void StartupSessions::EntriesList_SelectionChanged(const IInspectable& /*sender*/, const SelectionChangedEventArgs& args)
    {
        if (_ignoreListSelection || !_ViewModel)
        {
            return;
        }
        if (const auto item = EntriesList().SelectedItem().try_as<Editor::LaunchEntryViewModel>())
        {
            if (item != _ViewModel.SelectedEntry())
            {
                _ViewModel.SelectedEntry(item);
            }
            return;
        }
        // Nothing selected in the list. Only an explicit deselection of the view-model's row
        // (Ctrl+click) clears it; anything else (a container recycle) leaves it alone.
        const auto current = _ViewModel.SelectedEntry();
        if (!current)
        {
            return;
        }
        for (const auto& removed : args.RemovedItems())
        {
            if (removed.try_as<Editor::LaunchEntryViewModel>() == current)
            {
                _ViewModel.SelectedEntry(nullptr);
                return;
            }
        }
    }

    void StartupSessions::_syncListSelection()
    {
        const auto list = EntriesList();
        const auto selected = _selected();
        const auto previous = std::exchange(_ignoreListSelection, true);
        list.SelectedItem(selected);
        _ignoreListSelection = previous;
        if (selected)
        {
            list.ScrollIntoView(selected);
        }
    }

    void StartupSessions::_focusSelectedRow()
    {
        const auto list = EntriesList();
        const auto selected = _selected();
        if (!selected)
        {
            list.Focus(FocusState::Keyboard);
            return;
        }
        list.UpdateLayout(); // realize the re-inserted row's container
        if (const auto container = list.ContainerFromItem(selected).try_as<Windows::UI::Xaml::Controls::Control>())
        {
            container.Focus(FocusState::Keyboard);
        }
    }

    // ---- page commands ----

    void StartupSessions::AddEntry_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (_ViewModel)
        {
            _runOp([&] { _ViewModel.AddEntry(); });
        }
    }

    void StartupSessions::AddSeparator_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (_ViewModel)
        {
            _runOp([&] { _ViewModel.AddSeparator(); });
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
            // Surface a failed import inline (#435 item 2): the VM logs the detailed reason
            // to axan.log; the bar says it happened and that the list was left untouched.
            const auto ok = _runOp([&] { return _ViewModel.ImportFromToml(path); });
            ImportFailureBar().IsOpen(!ok);
        }
    }

    // axan #14: the confirm inside the "Save current as startup" flyout — replace the list
    // with the live window snapshot. 0 sessions captured leaves the list untouched and
    // surfaces the info bar; a success dismisses a stale bar.
    void StartupSessions::CaptureConfirm_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        CaptureConfirmFlyout().Hide();
        if (_ViewModel)
        {
            const auto count = _runOp([&] { return _ViewModel.CaptureLiveStartup(); });
            CaptureFailureBar().IsOpen(count == 0);
        }
    }

    // ---- selection toolbar ----

    void StartupSessions::DeleteEntry_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (const auto row = _selected())
        {
            _runOp([&] { _ViewModel.DeleteEntry(row); });
        }
    }

    void StartupSessions::DuplicateEntry_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (const auto row = _selected())
        {
            _runOp([&] { _ViewModel.DuplicateEntry(row); });
        }
    }

    void StartupSessions::IndentEntry_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (const auto row = _selected())
        {
            _runOp([&] { _ViewModel.IndentEntry(row); });
        }
    }

    void StartupSessions::OutdentEntry_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (const auto row = _selected())
        {
            _runOp([&] { _ViewModel.OutdentEntry(row); });
        }
    }

    void StartupSessions::MoveEntryUp_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (const auto row = _selected())
        {
            _runOp([&] { _ViewModel.MoveEntryUp(row); });
        }
    }

    void StartupSessions::MoveEntryDown_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        if (const auto row = _selected())
        {
            _runOp([&] { _ViewModel.MoveEntryDown(row); });
        }
    }

    // ---- keyboard (scoped to the list) ----
    // Each marks the key handled even when the op can't apply, so Alt+Left doesn't fall
    // through to back-navigation and Alt+Up/Down doesn't move the list's selection.

    void StartupSessions::MoveUpAccelerator_Invoked(const Input::KeyboardAccelerator& /*sender*/, const Input::KeyboardAcceleratorInvokedEventArgs& args)
    {
        args.Handled(true);
        if (_ViewModel && _ViewModel.CanMoveUp())
        {
            MoveEntryUp_Click(nullptr, nullptr);
            _focusSelectedRow();
        }
    }

    void StartupSessions::MoveDownAccelerator_Invoked(const Input::KeyboardAccelerator& /*sender*/, const Input::KeyboardAcceleratorInvokedEventArgs& args)
    {
        args.Handled(true);
        if (_ViewModel && _ViewModel.CanMoveDown())
        {
            MoveEntryDown_Click(nullptr, nullptr);
            _focusSelectedRow();
        }
    }

    void StartupSessions::OutdentAccelerator_Invoked(const Input::KeyboardAccelerator& /*sender*/, const Input::KeyboardAcceleratorInvokedEventArgs& args)
    {
        args.Handled(true);
        if (_ViewModel && _ViewModel.CanOutdent())
        {
            OutdentEntry_Click(nullptr, nullptr);
            _focusSelectedRow();
        }
    }

    void StartupSessions::IndentAccelerator_Invoked(const Input::KeyboardAccelerator& /*sender*/, const Input::KeyboardAcceleratorInvokedEventArgs& args)
    {
        args.Handled(true);
        if (_ViewModel && _ViewModel.CanIndent())
        {
            IndentEntry_Click(nullptr, nullptr);
            _focusSelectedRow();
        }
    }

    void StartupSessions::DuplicateAccelerator_Invoked(const Input::KeyboardAccelerator& /*sender*/, const Input::KeyboardAcceleratorInvokedEventArgs& args)
    {
        args.Handled(true);
        if (_ViewModel && _ViewModel.HasSelection())
        {
            DuplicateEntry_Click(nullptr, nullptr);
            _focusSelectedRow();
        }
    }

    void StartupSessions::DeleteAccelerator_Invoked(const Input::KeyboardAccelerator& /*sender*/, const Input::KeyboardAcceleratorInvokedEventArgs& args)
    {
        args.Handled(true);
        if (_ViewModel && _ViewModel.HasSelection())
        {
            DeleteEntry_Click(nullptr, nullptr);
            _focusSelectedRow();
        }
    }

    // ---- detail panel ----

    void StartupSessions::_rebuildDetail()
    {
        const auto row = _selected();
        if (row != _detailFor)
        {
            _detailFor = row;
            // Drop the old editor first: its controls go inert (they hold only a weak_ptr).
            _identity.reset();
            IdentityHost().Content(nullptr);

            if (row && row.IsSession())
            {
                const bool isDark = ActualTheme() == ElementTheme::Dark;

                IdentityEditor::Options options;
                options.initial.name = row.Name();
                options.initial.icon = row.Icon(); // native form, as the editor stores it
                options.initial.color = row.Color();
                options.initial.colorTarget = row.ColorTarget(); // "" -> "both" in the editor
                options.isDark = isDark;
                // The preview matches the list row: the icon (the neutral terminal glyph
                // when none is set), tinted when the colour target paints the icon.
                options.renderPreview = [isDark](const IdentityEditor::Fields& f) -> UIElement {
                    const auto icon = Microsoft::Terminal::UI::IconPathConverter::IconWUX(f.icon.empty() ? winrt::hstring{ L"" } : f.icon);
                    if (icon && f.colorTarget != L"text")
                    {
                        if (const auto c = Axan::SessionPalette::ParseHexColor(Axan::SessionPalette::ResolveToken(f.color, isDark)))
                        {
                            icon.Foreground(Media::SolidColorBrush{ Windows::UI::Color{ c->a, c->r, c->g, c->b } });
                        }
                    }
                    return icon;
                };
                // Every user edit lands on the row immediately (the row commits to the
                // settings clone). Only the changed field is written.
                options.onChanged = [weakThis = get_weak(), weakRow = winrt::make_weak(row)](const IdentityEditor::Fields& f) {
                    const auto r = weakRow.get();
                    if (!r)
                    {
                        return;
                    }
                    if (r.Name() != f.name)
                    {
                        r.Name(f.name);
                    }
                    if (r.Icon() != f.icon)
                    {
                        r.Icon(f.icon);
                    }
                    if (r.Color() != f.color)
                    {
                        r.Color(f.color);
                    }
                    const auto target = r.ColorTarget().empty() ? winrt::hstring{ L"both" } : r.ColorTarget();
                    if (target != f.colorTarget)
                    {
                        r.ColorTarget(f.colorTarget);
                    }
                    if (const auto self = weakThis.get())
                    {
                        self->_syncIconText();
                    }
                };
                options.onBrowseIcon = [weakThis = get_weak(), weakRow = winrt::make_weak(row)]() {
                    if (const auto self = weakThis.get())
                    {
                        if (const auto r = weakRow.get())
                        {
                            self->_PickIconImage(r);
                        }
                    }
                };
                _identity = IdentityEditor::Build(std::move(options));
                IdentityHost().Content(_identity->Root());
                Axan::Log::Debug("StartupSessions", "detail panel rebuilt for the selected row", { { "id", winrt::to_string(row.Id()) } });
            }
        }
        _syncIconText();
        _syncProfileCombo();
    }

    // The icon text box shows the row's icon in the portable vocabulary (a builtin glyph
    // reads as builtin:NAME). Skipped when the box already denotes the row's icon, so a
    // round trip through the editor never rewrites what the user is typing.
    void StartupSessions::_syncIconText()
    {
        const auto row = _selected();
        if (!row || !row.IsSession())
        {
            return;
        }
        const auto box = IconTextBox();
        bool mapped = false;
        if (hstring{ Axan::IconRegistry::PortableToNative(box.Text(), mapped) } == row.Icon())
        {
            return;
        }
        _iconTextSyncing = true;
        box.Text(row.IconPortable());
        _iconTextSyncing = false;
    }

    void StartupSessions::IconTextBox_TextChanged(const IInspectable& /*sender*/, const TextChangedEventArgs& /*args*/)
    {
        if (_iconTextSyncing || !_identity)
        {
            return;
        }
        // Through the editor, so its preview and grid highlight follow; it no-ops when the
        // text already denotes the current icon (e.g. the deferred TextChanged of a sync).
        bool mapped = false;
        _identity->SetIcon(hstring{ Axan::IconRegistry::PortableToNative(IconTextBox().Text(), mapped) });
    }

    safe_void_coroutine StartupSessions::_PickIconImage(Editor::LaunchEntryViewModel row)
    {
        auto lifetime = get_strong();

        // OpenImagePicker is the shared image-file dialog the Profiles page's icon Browse uses.
        const auto parentHwnd{ ::GetActiveWindow() };
        const auto file = co_await OpenImagePicker(parentHwnd);
        if (file.empty() || !row)
        {
            co_return;
        }
        if (_identity && _detailFor == row)
        {
            _identity->SetIcon(file); // writes the row through onChanged
        }
        else
        {
            row.Icon(file); // the selection moved while the dialog was up
        }
    }

    safe_void_coroutine StartupSessions::BrowseDirectory_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        auto lifetime = get_strong();
        const auto row = _selected();
        if (!row)
        {
            co_return;
        }
        const auto parentHwnd{ ::GetActiveWindow() };
        const auto folder = co_await OpenFilePicker(parentHwnd, [](auto&& dialog) {
            static constexpr winrt::guid clientGuidFolderPicker{ 0x3f1c9a52, 0x7d4e, 0x4b8a, { 0x9e, 0x21, 0x5c, 0x6a, 0x0d, 0x83, 0xb4, 0x17 } };
            THROW_IF_FAILED(dialog->SetClientGuid(clientGuidFolderPicker));
            DWORD flags{};
            THROW_IF_FAILED(dialog->GetOptions(&flags));
            THROW_IF_FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS)); // folders only
        });
        if (!folder.empty())
        {
            row.Directory(folder); // the row picked from, even if the selection moved
        }
    }

    // The ComboBox shows Model::Profile objects; the row stores a GUID string ("" = follow
    // the default profile). Select the item matching the selected row.
    void StartupSessions::_syncProfileCombo()
    {
        const auto row = _selected();
        const auto profiles = _ViewModel ? _ViewModel.AvailableProfiles() : nullptr;
        if (!row || !row.IsSession() || !profiles)
        {
            return;
        }
        const auto combo = ProfileCombo();
        // Programmatic: SelectionChanged must not commit (#435 item 1 — without this,
        // selecting a follow-the-default row would pin it to the current default's GUID).
        _profileComboLoading = true;
        const auto restore = wil::scope_exit([&]() noexcept { _profileComboLoading = false; });
        if (!combo.ItemsSource())
        {
            // Populated here, not in markup: a classic {Binding ElementName=RootPage}
            // resolves to null on a C++/WinRT page.
            combo.ItemsSource(profiles);
        }
        const auto target = row.Profile().empty() ? _ViewModel.DefaultProfileGuid() : row.Profile();
        for (const auto& p : profiles)
        {
            if (winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(p.Guid()) } == target)
            {
                combo.SelectedItem(p);
                return;
            }
        }
        combo.SelectedItem(nullptr);
    }

    void StartupSessions::ProfileCombo_SelectionChanged(const IInspectable& sender, const SelectionChangedEventArgs& /*args*/)
    {
        if (_profileComboLoading)
        {
            return; // _syncProfileCombo's programmatic selection, not a user pick
        }
        const auto combo = sender.try_as<ComboBox>();
        const auto row = _selected();
        if (!combo || !row)
        {
            return;
        }
        if (const auto p = combo.SelectedItem().try_as<winrt::Microsoft::Terminal::Settings::Model::Profile>())
        {
            const auto g = winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(p.Guid()) };
            // Only on a real change (a follow-the-default row stores "" while the combo
            // shows the default's GUID; re-picking the default fires no SelectionChanged).
            if (g != row.Profile())
            {
                row.Profile(g);
            }
        }
    }

    // ---- row-template formatting ----

    hstring StartupSessions::RowLabel(const hstring& name, bool isSeparator, const hstring& separatorStyle, double height)
    {
        if (!name.empty())
        {
            return name;
        }
        if (!isSeparator)
        {
            return L"(default name)";
        }
        const std::wstring_view kind = separatorStyle == L"space" ? L"Space" : L"Separator";
        if (height == 1.0)
        {
            return hstring{ kind };
        }
        return hstring{ fmt::format(FMT_COMPILE(L"{} · {:g} rows"), kind, height) };
    }

    double StartupSessions::RowLabelOpacity(const hstring& name)
    {
        return name.empty() ? 0.6 : 1.0;
    }

    Windows::UI::Text::FontWeight StartupSessions::RowLabelWeight(const hstring& name, bool isSeparator)
    {
        return (isSeparator && !name.empty()) ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal();
    }

    hstring StartupSessions::SeparatorChevron(const hstring& name, bool collapsed)
    {
        if (name.empty())
        {
            return {};
        }
        return collapsed ? hstring{ L"" } : hstring{ L"" }; // ChevronRight / ChevronDown
    }

    Visibility StartupSessions::SeparatorRuleVisibility(const hstring& separatorStyle)
    {
        return separatorStyle == L"space" ? Visibility::Collapsed : Visibility::Visible;
    }

    hstring StartupSessions::RowAccessibleName(const hstring& name, bool isSeparator, int32_t depth)
    {
        if (isSeparator)
        {
            return name.empty() ? hstring{ L"Separator" } : hstring{ L"Separator: " + std::wstring{ name } };
        }
        const std::wstring_view shown = name.empty() ? std::wstring_view{ L"Session with the default name" } : std::wstring_view{ name };
        return hstring{ fmt::format(FMT_COMPILE(L"{}, level {}"), shown, depth + 1) };
    }

    Automation::Peers::AutomationHeadingLevel StartupSessions::RowHeadingLevel(bool isSeparator)
    {
        return isSeparator ? Automation::Peers::AutomationHeadingLevel::Level2 : Automation::Peers::AutomationHeadingLevel::None;
    }
}

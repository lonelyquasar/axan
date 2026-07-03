// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <ThrottledFunc.h>

#include "TerminalPage.g.h"
#include "Tab.h"
#include "AppKeyBindings.h"
#include "AppCommandlineArgs.h"
#include "RenameWindowRequestedArgs.g.h"
#include "RequestMoveContentArgs.g.h"
#include "LaunchPositionRequest.g.h"
#include "Toast.h"

#include "WindowsPackageManagerFactory.h"

#define DECLARE_ACTION_HANDLER(action) void _Handle##action(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

namespace TerminalAppLocalTests
{
    class TabTests;
    class SettingsTests;
}

namespace Microsoft::Terminal::Core
{
    class ControlKeyStates;
}

namespace winrt::TerminalApp::implementation
{
    struct TerminalSettingsCache;

    inline constexpr uint32_t DefaultRowsToScroll{ 3 };
    inline constexpr std::wstring_view TabletInputServiceKey{ L"TabletInputService" };

    enum StartupState : int
    {
        NotInitialized = 0,
        InStartup = 1,
        Initialized = 2
    };

    enum ScrollDirection : int
    {
        ScrollUp = 0,
        ScrollDown = 1
    };

    struct RenameWindowRequestedArgs : RenameWindowRequestedArgsT<RenameWindowRequestedArgs>
    {
        WINRT_PROPERTY(winrt::hstring, ProposedName);

    public:
        RenameWindowRequestedArgs(const winrt::hstring& name) :
            _ProposedName{ name } {};
    };

    struct RequestMoveContentArgs : RequestMoveContentArgsT<RequestMoveContentArgs>
    {
        WINRT_PROPERTY(winrt::hstring, Window);
        WINRT_PROPERTY(winrt::hstring, Content);
        WINRT_PROPERTY(uint32_t, TabIndex);
        WINRT_PROPERTY(Windows::Foundation::IReference<Windows::Foundation::Point>, WindowPosition);

    public:
        RequestMoveContentArgs(const winrt::hstring window, const winrt::hstring content, uint32_t tabIndex) :
            _Window{ window },
            _Content{ content },
            _TabIndex{ tabIndex } {};
    };

    struct LaunchPositionRequest : LaunchPositionRequestT<LaunchPositionRequest>
    {
        LaunchPositionRequest() = default;

        til::property<winrt::Microsoft::Terminal::Settings::Model::LaunchPosition> Position;
    };

    struct WinGetSearchParams
    {
        winrt::Microsoft::Management::Deployment::PackageMatchField Field;
        winrt::Microsoft::Management::Deployment::PackageFieldMatchOption MatchOption;
    };

    struct TerminalPage : TerminalPageT<TerminalPage>
    {
    public:
        TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager);

        // This implements shobjidl's IInitializeWithWindow, but due to a XAML Compiler bug we cannot
        // put it in our inheritance graph. https://github.com/microsoft/microsoft-ui-xaml/issues/3331
        STDMETHODIMP Initialize(HWND hwnd);

        void SetSettings(Microsoft::Terminal::Settings::Model::CascadiaSettings settings, bool needRefreshUI);

        void Create();
        Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer();

        bool ShouldImmediatelyHandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings) const;
        void HandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        hstring Title();
        // axan: the two halves of the titlebar's centered title (defined in
        // AxanSessionTreeView.cpp — primary reads the focused node's sidebar label).
        hstring TitlebarPrimaryText();
        hstring TitlebarSecondaryText();

        void TitlebarClicked();
        void WindowVisibilityChanged(const bool showOrHide);

        float CalcSnappedDimension(const bool widthOrHeight, const float dimension) const;

        winrt::hstring ApplicationDisplayName();
        winrt::hstring ApplicationVersion();

        CommandPalette LoadCommandPalette();
        SuggestionsControl LoadSuggestionsUI();

        safe_void_coroutine RequestQuit();
        safe_void_coroutine CloseWindow();
        void PersistState(bool serializeBuffer);

        void ToggleFocusMode();
        void ToggleFullscreen();
        void ToggleAlwaysOnTop();
        bool FocusMode() const;
        bool Fullscreen() const;
        bool AlwaysOnTop() const;
        bool ShowTabsFullscreen() const;
        void SetShowTabsFullscreen(bool newShowTabsFullscreen);
        void SetFullscreen(bool);
        void SetFocusMode(const bool inFocusMode);
        void Maximized(bool newMaximized);
        void RequestSetMaximized(bool newMaximized);

        void SetStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions);
        void SetStartupNodeMetadata(std::vector<winrt::hstring> templates, std::vector<int32_t> parentIndices, std::vector<winrt::hstring> iconOverrides, std::vector<winrt::hstring> iconColors, std::vector<winrt::hstring> colorTargets, std::vector<winrt::hstring> entryIds); // axan M5/M6/M13/#422
        void SetStartupConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        static std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> ConvertExecuteCommandlineToActions(const Microsoft::Terminal::Settings::Model::ExecuteCommandlineArgs& args);

        winrt::TerminalApp::IDialogPresenter DialogPresenter() const;
        void DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter);

        winrt::TerminalApp::TaskbarState TaskbarState() const;

        void ShowKeyboardServiceWarning() const;
        winrt::hstring KeyboardServiceDisabledText();

        void IdentifyWindow();
        void ActionSaved(winrt::hstring input, winrt::hstring name, winrt::hstring keyChord);
        void ActionSaveFailed(winrt::hstring message);
        void ShowTerminalWorkingDirectory();

        safe_void_coroutine ProcessStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                                  const winrt::hstring cwd = winrt::hstring{},
                                                  const winrt::hstring env = winrt::hstring{});
        safe_void_coroutine CreateTabFromConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        TerminalApp::WindowProperties WindowProperties() const noexcept { return _WindowProperties; };

        bool CanDragDrop() const noexcept;
        bool IsRunningElevated() const noexcept;

        void OpenSettingsUI();
        void WindowActivated(const bool activated);

        bool OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down);

        void AttachContent(Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> args, uint32_t tabIndex);
        void SendContentToOther(winrt::TerminalApp::RequestReceiveContentArgs args);

        uint32_t NumberOfTabs() const;

        til::property_changed_event PropertyChanged;

        // -------------------------------- WinRT Events ---------------------------------
        til::typed_event<IInspectable, IInspectable> TitleChanged;
        til::typed_event<IInspectable, IInspectable> CloseWindowRequested;
        til::typed_event<IInspectable, winrt::Windows::UI::Xaml::UIElement> SetTitleBarContent;
        til::typed_event<IInspectable, IInspectable> FocusModeChanged;
        til::typed_event<IInspectable, IInspectable> FullscreenChanged;
        til::typed_event<IInspectable, IInspectable> ChangeMaximizeRequested;
        til::typed_event<IInspectable, IInspectable> AlwaysOnTopChanged;
        til::typed_event<IInspectable, IInspectable> RaiseVisualBell;
        til::typed_event<IInspectable, IInspectable> SetTaskbarProgress;
        til::typed_event<IInspectable, IInspectable> Initialized;
        til::typed_event<IInspectable, IInspectable> IdentifyWindowsRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::RenameWindowRequestedArgs> RenameWindowRequested;
        til::typed_event<IInspectable, IInspectable> SummonWindowRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs> WindowSizeChanged;

        til::typed_event<IInspectable, IInspectable> OpenSystemMenu;
        til::typed_event<IInspectable, IInspectable> QuitRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::ShowWindowArgs> ShowWindowChanged;
        til::typed_event<Windows::Foundation::IInspectable, Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>> ShowLoadWarningsDialog;

        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestMoveContentArgs> RequestMoveContent;
        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestReceiveContentArgs> RequestReceiveContent;

        til::typed_event<IInspectable, winrt::TerminalApp::LaunchPositionRequest> RequestLaunchPosition;

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, TitlebarBrush, PropertyChanged.raise, nullptr);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, FrameBrush, PropertyChanged.raise, nullptr);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionName, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionKeyChord, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionCommandLine, PropertyChanged.raise, L"");

    private:
        friend struct TerminalPageT<TerminalPage>; // for Xaml to bind events
        std::optional<HWND> _hostingHwnd;

        // If you add controls here, but forget to null them either here or in
        // the ctor, you're going to have a bad time. It'll mysteriously fail to
        // activate the app.
        // ALSO: If you add any UIElements as roots here, make sure they're
        // updated in App::_ApplyTheme. The roots currently is _tabRow
        // (which is a root when the tabs are in the titlebar.)
        Microsoft::UI::Xaml::Controls::TabView _tabView{ nullptr };
        TerminalApp::TabRowControl _tabRow{ nullptr };
        Windows::UI::Xaml::Controls::Grid _tabContent{ nullptr };
        Microsoft::UI::Xaml::Controls::SplitButton _newTabButton{ nullptr };
        winrt::TerminalApp::ColorPickupFlyout _tabColorPicker{ nullptr };

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _tabs;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _mruTabs;
        static winrt::com_ptr<Tab> _GetTabImpl(const TerminalApp::Tab& tab);

        void _UpdateTabIndices();

        TerminalApp::Tab _settingsTab{ nullptr };

        bool _isInFocusMode{ false };
        bool _isFullscreen{ false };
        bool _isMaximized{ false };
        bool _isAlwaysOnTop{ false };
        bool _showTabsFullscreen{ false };

        std::optional<uint32_t> _loadFromPersistedLayoutIdx{};

        bool _rearranging{ false };
        std::optional<int> _rearrangeFrom{};
        std::optional<int> _rearrangeTo{};
        bool _removing{ false };
        // axan #428: true while a same-window tab move does its transient RemoveAt+InsertAt of
        // the same tab (_TryMoveTab / _TabDragCompleted). _OnSessionsCollectionChanged skips
        // reconciling the session tree then — the tree is identity-keyed, so a pure reorder of
        // _tabs changes no node; reconciling mid-move would prune the node and mint a blank one.
        bool _suppressSessionTreeReconcile{ false };

        bool _activated{ false };
        bool _visible{ true };

        std::vector<std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>> _previouslyClosedPanesAndTabs{};

        uint32_t _systemRowsToScroll{ DefaultRowsToScroll };

        // use a weak reference to prevent circular dependency with AppLogic
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;

        winrt::com_ptr<AppKeyBindings> _bindings{ winrt::make_self<implementation::AppKeyBindings>() };
        winrt::com_ptr<ShortcutActionDispatch> _actionDispatch{ winrt::make_self<implementation::ShortcutActionDispatch>() };

        winrt::Windows::UI::Xaml::Controls::Grid::LayoutUpdated_revoker _layoutUpdatedRevoker;
        StartupState _startupState{ StartupState::NotInitialized };

        std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> _startupActions;
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _startupConnection{ nullptr };

        // axan M5/M6: startup node metadata loaded from the settings.json startup tree, drained in DFS spawn
        // order (via the cursor) as the startup tabs are created. _pendingStartupParents is
        // index-aligned with _pendingStartupLabelTemplates: each entry is the spawn-order
        // index of that node's parent (-1 = root). _startupNodesBySpawnIndex maps a spawn
        // index to the node created for it (weak — it only matters during startup), so a
        // child node can be reparented under its already-created parent. After M6 the
        // per-node template no longer lives in a vector parallel to _tabs; it rides on each
        // node's SessionNodeViewModel, surviving reorder/reparent and read back on save.
        std::vector<winrt::hstring> _pendingStartupLabelTemplates;
        std::vector<int32_t> _pendingStartupParents;
        // axan M13: index-aligned with the templates above — each node's persisted icon
        // override + recolor, drained by the same cursor and applied to the spawning node's
        // view-model so a picked icon survives a relaunch.
        std::vector<winrt::hstring> _pendingStartupIconOverrides;
        std::vector<winrt::hstring> _pendingStartupIconColors;
        std::vector<winrt::hstring> _pendingStartupColorTargets; // M13
        std::vector<winrt::hstring> _pendingStartupEntryIds; // #422: source entry Id per spawn index
        size_t _startupNodeCursor{ 0 };
        std::vector<winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::TreeViewNode>> _startupNodesBySpawnIndex;

        std::shared_ptr<Toast> _windowIdToast{ nullptr };
        std::shared_ptr<Toast> _actionSavedToast{ nullptr };
        std::shared_ptr<Toast> _actionSaveFailedToast{ nullptr };
        std::shared_ptr<Toast> _windowCwdToast{ nullptr };

        winrt::Windows::UI::Xaml::Controls::TextBox::LayoutUpdated_revoker _renamerLayoutUpdatedRevoker;
        int _renamerLayoutCount{ 0 };
        bool _renamerPressedEnter{ false };

        // axan M13: live state for the "Edit session node" overlay while it's open. The node
        // is weak so the editor never keeps a closed session's node alive; the field/preview
        // refs are rebuilt each open. _nodeEditColor is the chosen recolor; _nodeEditFallbackPath
        // is the icon the node falls back to (profile -> auto) so the preview matches the sidebar
        // before a glyph is picked. The label field is focused on its 2nd LayoutUpdated.
        winrt::weak_ref<Microsoft::UI::Xaml::Controls::TreeViewNode> _nodeEditTarget;
        winrt::Windows::UI::Xaml::Controls::TextBox _nodeEditLabelBox{ nullptr }; // Session name field
        winrt::Windows::UI::Xaml::Controls::ContentPresenter _nodeEditPreview{ nullptr };
        // axan M13 (design handoff): the icon is chosen via the button row / Browse (no text
        // field), so the current choice + recolor + recolor-target are tracked as strings.
        winrt::hstring _nodeEditIconOverride;
        winrt::hstring _nodeEditColor;
        winrt::hstring _nodeEditColorTarget{ L"both" };
        winrt::hstring _nodeEditFallbackPath;
        // axan #422: the dialog's resolved theme (sidebar ActualTheme), captured on open. The
        // color swatches store palette NAMES but fill with the theme-resolved hex (WYSIWYG), and
        // the live preview resolves the same way, so both must know which palette set is active.
        bool _nodeEditIsDark{ true };
        winrt::Windows::UI::Xaml::Controls::TextBox::LayoutUpdated_revoker _nodeEditLayoutUpdatedRevoker;
        int _nodeEditLayoutCount{ 0 };

        TerminalApp::WindowProperties _WindowProperties{ nullptr };
        PaneResources _paneResources;

        TerminalApp::ContentManager _manager{ nullptr };

        std::shared_ptr<TerminalSettingsCache> _terminalSettingsCache{};

        struct StashedDragData
        {
            winrt::com_ptr<winrt::TerminalApp::implementation::Tab> draggedTab{ nullptr };
            winrt::Windows::Foundation::Point dragOffset{ 0, 0 };
        } _stashed;

        safe_void_coroutine _NewTerminalByDrop(const Windows::Foundation::IInspectable&, winrt::Windows::UI::Xaml::DragEventArgs e);

        __declspec(noinline) CommandPalette _loadCommandPaletteSlowPath();
        bool _commandPaletteIs(winrt::Windows::UI::Xaml::Visibility visibility);
        __declspec(noinline) SuggestionsControl _loadSuggestionsElementSlowPath();
        bool _suggestionsControlIs(winrt::Windows::UI::Xaml::Visibility visibility);

        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowDialogHelper(const std::wstring_view& name);

        void _ShowAboutDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowQuitDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseReadOnlyDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowMultiLinePasteWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowLargePasteWarningDialog();

        void _CreateNewTabFlyout();
        // axan: the settings / command palette / about tail shared by the new-tab flyout
        // and the titlebar app menu (_CreateAppMenuFlyout), so the two can't drift.
        void _AppendCommonMenuItems(const winrt::Windows::UI::Xaml::Controls::MenuFlyout& flyout);
        std::vector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase> _CreateNewTabFlyoutItems(winrt::Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::NewTabMenuEntry> entries);
        winrt::Windows::UI::Xaml::Controls::IconElement _CreateNewTabFlyoutIcon(const winrt::hstring& icon);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutProfile(const Microsoft::Terminal::Settings::Model::Profile profile, int profileIndex, const winrt::hstring& iconPathOverride);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride);

        void _OpenNewTabDropdown();
        HRESULT _OpenNewTab(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs);
        TerminalApp::Tab _CreateNewTabFromPane(std::shared_ptr<Pane> pane, uint32_t insertPosition = -1);

        std::wstring _evaluatePathForCwd(std::wstring_view path);

        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _CreateConnectionFromSettings(Microsoft::Terminal::Settings::Model::Profile profile, Microsoft::Terminal::Settings::Model::TerminalSettings settings, const bool inheritCursor);
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _duplicateConnectionForRestart(const TerminalApp::TerminalPaneContent& paneContent);
        void _restartPaneConnection(const TerminalApp::TerminalPaneContent&, const winrt::Windows::Foundation::IInspectable&);

        safe_void_coroutine _OpenNewWindow(const Microsoft::Terminal::Settings::Model::INewContentArgs newContentArgs);

        void _OpenNewTerminalViaDropdown(const Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        bool _displayingCloseDialog{ false };
        void _SettingsButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _CommandPaletteButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _AboutButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);

        void _KeyDownHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const Tab& tab);
        void _UpdateTabIcon(Tab& tab);
        void _UpdateTabView();
        void _UpdateTabWidthMode();
        void _SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance);

        void _DuplicateFocusedTab();
        void _DuplicateTab(const Tab& tab);

        safe_void_coroutine _ExportTab(const Tab& tab, winrt::hstring filepath);

        winrt::Windows::Foundation::IAsyncAction _HandleCloseTabRequested(winrt::TerminalApp::Tab tab);
        void _CloseTabAtIndex(uint32_t index);
        void _RemoveTab(const winrt::TerminalApp::Tab& tab);
        safe_void_coroutine _RemoveTabs(const std::vector<winrt::TerminalApp::Tab> tabs);

        void _InitializeTab(winrt::com_ptr<Tab> newTabImpl, uint32_t insertPosition = -1);
        void _RegisterTerminalEvents(Microsoft::Terminal::Control::TermControl term);
        void _RegisterTabEvents(Tab& hostingTab);

        void _DismissTabContextMenus();
        void _FocusCurrentTab(const bool focusAlways);
        bool _HasMultipleTabs() const;

        void _SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode);
        bool _SelectTab(uint32_t tabIndex);
        bool _MoveFocus(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _SwapPane(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _MovePane(const Microsoft::Terminal::Settings::Model::MovePaneArgs args);
        bool _MoveTab(winrt::com_ptr<Tab> tab, const Microsoft::Terminal::Settings::Model::MoveTabArgs args);

        std::shared_ptr<ThrottledFunc<>> _adjustProcessPriorityThrottled;
        void _adjustProcessPriority() const;

        template<typename F>
        bool _ApplyToActiveControls(F f) const
        {
            if (const auto tab{ _GetFocusedTabImpl() })
            {
                if (const auto activePane = tab->GetActivePane())
                {
                    activePane->WalkTree([&](auto p) {
                        if (const auto& control{ p->GetTerminalControl() })
                        {
                            f(control);
                        }
                    });

                    return true;
                }
            }
            return false;
        }

        winrt::Microsoft::Terminal::Control::TermControl _GetActiveControl() const;
        std::optional<uint32_t> _GetFocusedTabIndex() const noexcept;
        std::optional<uint32_t> _GetTabIndex(const TerminalApp::Tab& tab) const noexcept;
        TerminalApp::Tab _GetFocusedTab() const noexcept;
        winrt::com_ptr<Tab> _GetFocusedTabImpl() const noexcept;
        TerminalApp::Tab _GetTabByTabViewItem(const IInspectable& tabViewItem) const noexcept;

        void _HandleClosePaneRequested(std::shared_ptr<Pane> pane);
        safe_void_coroutine _SetFocusedTab(const winrt::TerminalApp::Tab tab);
        safe_void_coroutine _CloseFocusedPane();
        void _ClosePanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        winrt::Windows::Foundation::IAsyncOperation<bool> _PaneConfirmCloseReadOnly(std::shared_ptr<Pane> pane);
        void _AddPreviouslyClosedPaneOrTab(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>&& args);

        void _Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll);

        void _SplitPane(const winrt::com_ptr<Tab>& tab,
                        const Microsoft::Terminal::Settings::Model::SplitDirection splitType,
                        const float splitSize,
                        std::shared_ptr<Pane> newPane);
        bool _ResizePane(const Microsoft::Terminal::Settings::Model::ResizeDirection& direction);
        void _ToggleSplitOrientation();

        void _ScrollPage(ScrollDirection scrollDirection);
        void _ScrollToBufferEdge(ScrollDirection scrollDirection);
        void _SetAcceleratorForMenuItem(Windows::UI::Xaml::Controls::MenuFlyoutItem& menuItem, const winrt::Microsoft::Terminal::Control::KeyChord& keyChord);

        safe_void_coroutine _PasteFromClipboardHandler(const IInspectable sender,
                                                       const Microsoft::Terminal::Control::PasteFromClipboardEventArgs eventArgs);

        safe_void_coroutine _OpenHyperlinkHandler(const IInspectable sender, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs);

        // axan M11: input-safety guard — confirm a Ctrl+C that would discard unsubmitted input.
        safe_void_coroutine _ShowCtrlCInputGuardHandler(const IInspectable sender, const IInspectable eventArgs);
        static bool _IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri);
        bool _IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri) const;

        void _ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri);
        bool _CopyText(bool dismissSelection, bool singleLine, bool withControlSequences, Microsoft::Terminal::Control::CopyFormat formats);

        safe_void_coroutine _SetTaskbarProgressHandler(const IInspectable sender, const IInspectable eventArgs);

        void _copyToClipboard(IInspectable, Microsoft::Terminal::Control::WriteToClipboardEventArgs args) const;
        void _PasteText();

        safe_void_coroutine _ControlNoticeRaisedHandler(const IInspectable sender, const Microsoft::Terminal::Control::NoticeEventArgs eventArgs);
        void _ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message);

        safe_void_coroutine _LaunchSettings(const Microsoft::Terminal::Settings::Model::SettingsTarget target);

        void _TabDragStarted(const IInspectable& sender, const IInspectable& eventArgs);
        void _TabDragCompleted(const IInspectable& sender, const IInspectable& eventArgs);

        // BODGY: WinUI's TabView has a broken close event handler:
        // If the close button is disabled, middle-clicking the tab raises no close
        // event. Because that's dumb, we implement our own middle-click handling.
        // `_tabItemMiddleClickHookEnabled` is true whenever the close button is hidden,
        // and that enables all of the rest of this machinery (and this workaround).
        bool _tabItemMiddleClickHookEnabled = false;
        bool _tabItemMiddleClickExited = false;
        PointerEntered_revoker _tabItemMiddleClickPointerEntered;
        PointerExited_revoker _tabItemMiddleClickPointerExited;
        PointerCaptureLost_revoker _tabItemMiddleClickPointerCaptureLost;
        void _OnTabPointerPressed(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        safe_void_coroutine _OnTabPointerReleasedCloseTab(IInspectable sender);

        void _OnTabSelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _OnTabItemsChanged(const IInspectable& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs);
        void _OnTabCloseRequested(const IInspectable& sender, const Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& eventArgs);
        void _OnFirstLayout(const IInspectable& sender, const IInspectable& eventArgs);
        void _UpdatedSelectedTab(const winrt::TerminalApp::Tab& tab);
        void _UpdateBackground(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);

        // axan M2/M6: mirror the session list (_tabs) into the sidebar TreeView and route
        // node activation back into the existing tab-selection path. Since M6's drag
        // reorder/reparent severs the old "RootNodes index == _tabs index" invariant, every
        // node↔session link goes through the node's SessionNodeViewModel (identity), and the
        // tree is kept in sync with _tabs by reconciling on identity rather than position.
        // #436: the session-tree/sidebar member definitions below live in
        // AxanSessionTreeView.cpp (extracted from TabManagement.cpp so the axan block no
        // longer sits inside the file upstream modifies most).
        Microsoft::UI::Xaml::Controls::TreeViewNode _CreateSessionNode(const winrt::TerminalApp::Tab& tab, const winrt::hstring& labelTemplate, const winrt::hstring& iconOverride = {}, const winrt::hstring& iconColor = {}, const winrt::hstring& colorTarget = {}, const winrt::hstring& entryId = {});
        // axan #422/#427: write a node's current label/icon/color/target back to its source
        // global StartupSessions entry (matched by the node's EntryId) and persist settings.json,
        // so a sidebar edit survives a restart. A node with no EntryId (runtime session) is a no-op.
        void _PersistNodeToEntry(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);
        Microsoft::UI::Xaml::Controls::TreeViewNode _FindNodeForTab(const winrt::TerminalApp::Tab& tab);
        void _AddMissingSessionNodes();
        void _PruneRemovedSessionNodes();
        void _RemoveNodeSelfHealing(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);
        // axan M13: re-raise every node's LabelBrush so labels repaint theme-correct on a theme
        // switch (the default text color is white-on-dark / near-black-on-light).
        // #422: themeOverride lets _updateThemeColors pass the just-set requested theme directly,
        // avoiding a stale read of the sidebar's not-yet-settled ActualTheme (which left node
        // colors on the old palette after a theme switch). Default -> read the settled ActualTheme.
        void _RefreshNodeLabelBrushes(Windows::UI::Xaml::ElementTheme themeOverride = Windows::UI::Xaml::ElementTheme::Default);
        void _OnSessionsCollectionChanged(const Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::Tab>& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& args);
        void _OnSessionTreeItemInvoked(const Microsoft::UI::Xaml::Controls::TreeView& sender, const Microsoft::UI::Xaml::Controls::TreeViewItemInvokedEventArgs& args);
        // axan #436 item 3: the one resolve-and-select path shared by the expanded tree
        // (ItemInvoked) and the minimized icon list (ItemClick): VM -> weak TabRef -> Tab ->
        // the existing TabView selection path.
        void _SelectTabForNodeVM(const winrt::TerminalApp::SessionNodeViewModel& vm);
        // axan #436: the sidebar's resolved dark/light, shared by _RefreshNodeLabelBrushes
        // and the node editor's swatch/preview palette choice. themeOverride as in
        // _RefreshNodeLabelBrushes (#422: sidesteps ActualTheme's layout-pass lag).
        bool _SidebarIsDarkTheme(Windows::UI::Xaml::ElementTheme themeOverride = Windows::UI::Xaml::ElementTheme::Default);

        // axan M13: right-click a session node for its actions (rename label, set/clear icon,
        // recolor, duplicate, add child, close, open the profile in WT Settings). The menu is
        // built in code and shown at the right-clicked TreeViewItem (avoids the XAML-compiler
        // WinUI-2 type-set gaps M7/M8 hit, and lets each item capture the target node). The
        // node ops reuse the M6/M7 plumbing rather than a new model.
        void _OnSessionTreeRightTapped(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::RightTappedRoutedEventArgs& args);
        // axan #429: Shift+F10 / the menu key raise ContextRequested, not RightTapped — this
        // handler is the keyboard route to the same per-node menu, anchored to the focused row.
        void _OnSessionTreeContextRequested(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::ContextRequestedEventArgs& args);
        // axan M13/#429: the menu itself, shared by both routes. `position` is item-relative
        // when the gesture reported one (pointer), else the flyout anchors to the row (keyboard).
        void _ShowSessionNodeContextMenu(const Microsoft::UI::Xaml::Controls::TreeViewNode& node, const Microsoft::UI::Xaml::Controls::TreeViewItem& item, const std::optional<Windows::Foundation::Point>& position);
        // axan #429: the currently-open per-node menu — only consulted so a gesture that
        // raises both RightTapped and ContextRequested can't open the menu twice.
        Windows::UI::Xaml::Controls::MenuFlyout _sessionNodeMenu{ nullptr };
        Microsoft::UI::Xaml::Controls::TreeViewNode _NodeUnderPointer(const Windows::Foundation::IInspectable& originalSource);
        void _ApplyNodeIcon(const Microsoft::UI::Xaml::Controls::TreeViewNode& node, const winrt::hstring& iconOverride, const winrt::hstring& iconColor, const winrt::hstring& colorTarget);
        // axan M13 (design handoff): "Browse to icon file…" — pick an image/exe and set it as the
        // editor's current icon override (updates the live preview).
        safe_void_coroutine _BrowseNodeIconFile();
        // axan M13: the combined label + icon "Edit session node" editor as an in-tree overlay
        // (NodeEditOverlay/NodeEditCard in the XAML) — not a ContentDialog (whose TextBox gets no
        // typed characters in Xaml Islands) and not a TeachingTip (which can be orphaned on
        // minimize). In-tree means the TextBoxes get keyboard input and the editor can't be lost.
        // The label field is focused on its 2nd LayoutUpdated. State lives in the members below.
        void _ShowNodeEditPanel(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);
        // axan #436: per-section builders for the node editor card, split out of the original
        // monolithic _ShowNodeEditPanel. Each builds one row/section of the card; the
        // selection brushes are passed in so all sections highlight consistently.
        Windows::UI::Xaml::Controls::StackPanel _BuildNodeEditNameSection(const winrt::hstring& currentTemplate, const Windows::UI::Xaml::Media::SolidColorBrush& neutralBrush);
        Windows::UI::Xaml::Controls::Grid _BuildNodeEditIconRow(const Windows::UI::Xaml::Media::SolidColorBrush& accentBrush, const Windows::UI::Xaml::Media::SolidColorBrush& neutralBrush);
        Windows::UI::Xaml::Controls::StackPanel _BuildNodeEditSwatchRow(const Windows::UI::Xaml::Media::SolidColorBrush& accentBrush, const Windows::UI::Xaml::Media::SolidColorBrush& neutralBrush);
        Windows::UI::Xaml::Controls::StackPanel _BuildNodeEditApplyColorSection();
        Windows::UI::Xaml::Controls::StackPanel _BuildNodeEditFooter();
        void _UpdateNodeEditPreview();
        void _HideNodeEditPanel();
        void _AddChildSession(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);
        void _DuplicateNodeSession(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);
        void _CloseNodeSession(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);
        void _OpenNodeProfileSettings(const Microsoft::UI::Xaml::Controls::TreeViewNode& node);

        // axan: the titlebar app menu (the "axan" dropdown in the upper-left).
        // Mirrors the per-node context menu, targeting the ACTIVE session — each item
        // resolves _FindNodeForTab(_GetFocusedTab()) at click time, never capturing a
        // node — plus the app-level entries (new session, toggle sidebar, and the shared
        // settings / command palette / about tail). Rebuilt on settings reload so the
        // accelerator hints track rebindings; the session-scoped items are enabled or
        // disabled by the Opening refresh (_RefreshAppMenuSessionItems).
        void _CreateAppMenuFlyout();
        void _RefreshAppMenuSessionItems();
        Windows::UI::Xaml::Controls::MenuFlyout _appMenuFlyout{ nullptr };
        std::vector<Windows::UI::Xaml::Controls::MenuFlyoutItem> _appMenuSessionItems;
        // Disabled header row naming the session the items below act on ("Current
        // session: <label>") — unlike the sidebar context menu, this menu has no
        // right-clicked row to make the target obvious. Text set on every open.
        Windows::UI::Xaml::Controls::MenuFlyoutItem _appMenuSessionHeader{ nullptr };

        // axan M8: session-sidebar collapse states. The chevron cycles
        // expanded -> minimized -> collapsed; the collapsed strip jumps back to expanded.
        // _SetSidebarState persists the choice (ApplicationState) and calls _ApplySidebarState,
        // which is the single place that drives the column width and which of the three
        // surfaces (expanded tree / minimized icon list / collapsed strip) is shown.
        // The minimized state can't reuse the TreeView rows: the tree's per-depth indentation
        // pushes icons out of a narrow column, so minimized renders a separate flat ListView
        // of node icons (one per node, hierarchy flattened) — mirroring the Linux design's
        // dedicated minimized cells. _RebuildMinimizedItems flattens the tree into it.
        // _minimizedSidebarWidth maps the size enum to px.
        void _OnSidebarCollapseClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OnSidebarExpandStripClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OnMinimizedItemClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::ItemClickEventArgs& args);
        // axan M8c: right-click flyout to choose the minimized-column width step.
        void _OnSidebarSizeFlyoutOpening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);
        void _OnSidebarSizeClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CycleSidebarState();
        // axan #429: move keyboard focus into the sidebar (the focusSidebar action). Expands
        // a collapsed sidebar first; lands on the selected (or first) row of whichever
        // navigator surface is showing (tree / minimized icon list).
        void _FocusSessionTree();
        void _SetSidebarState(Microsoft::Terminal::Settings::Model::SidebarState state);
        void _ApplySidebarState(Microsoft::Terminal::Settings::Model::SidebarState state);
        void _RebuildMinimizedItems();
        double _minimizedSidebarWidth() const;
        // axan M12: draggable divider for the expanded sidebar. _expandedSidebarWidth reads
        // the persisted width (clamped); the Thumb handlers resize live + persist on release.
        double _expandedSidebarWidth() const;
        void _OnSidebarResizeDelta(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args);
        void _OnSidebarResizeCompleted(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::DragCompletedEventArgs& args);
        // axan M12 fix: show the west-east resize cursor while hovering/dragging the divider.
        void _OnSidebarResizeThumbHover(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _OnSidebarResizeThumbLeave(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        Microsoft::Terminal::Settings::Model::SidebarState _sidebarState{ Microsoft::Terminal::Settings::Model::SidebarState::Expanded };
        Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::SessionNodeViewModel> _minimizedItems{ nullptr };

        void _OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command);
        void _OnCommandLineExecutionRequested(const IInspectable& sender, const winrt::hstring& commandLine);
        void _OnSwitchToTabRequested(const IInspectable& sender, const winrt::TerminalApp::Tab& tab);

        void _Find(const Tab& tab);

        winrt::Microsoft::Terminal::Control::TermControl _CreateNewControlAndContent(const winrt::Microsoft::Terminal::Settings::Model::TerminalSettingsCreateResult& settings,
                                                                                     const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& connection);
        winrt::Microsoft::Terminal::Control::TermControl _SetupControl(const winrt::Microsoft::Terminal::Control::TermControl& term);
        winrt::Microsoft::Terminal::Control::TermControl _AttachControlToContent(const uint64_t& contentGuid);

        TerminalApp::IPaneContent _makeSettingsContent();
        std::shared_ptr<Pane> _MakeTerminalPane(const Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs = nullptr,
                                                const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                                winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);
        std::shared_ptr<Pane> _MakePane(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs = nullptr,
                                        const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);

        void _RefreshUIForSettingsReload();

        void _SetNewTabButtonColor(til::color color, til::color accentColor);
        void _ClearNewTabButtonColor();

        safe_void_coroutine _CompleteInitialization();

        void _FocusActiveControl(IInspectable sender, IInspectable eventArgs);

        void _UnZoomIfNeeded();

        static int _ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll);
        static uint32_t _ReadSystemRowsToScroll();

        void _UpdateMRUTab(const winrt::TerminalApp::Tab& tab);

        void _TryMoveTab(const uint32_t currentTabIndex, const int32_t suggestedNewTabIndex);

        void _PreviewAction(const Microsoft::Terminal::Settings::Model::ActionAndArgs& args);
        void _PreviewActionHandler(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& args);
        void _EndPreview();
        void _RunRestorePreviews();
        void _PreviewColorScheme(const Microsoft::Terminal::Settings::Model::SetColorSchemeArgs& args);
        void _PreviewAdjustOpacity(const Microsoft::Terminal::Settings::Model::AdjustOpacityArgs& args);
        void _PreviewSendInput(const Microsoft::Terminal::Settings::Model::SendInputArgs& args);

        winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs _lastPreviewedAction{ nullptr };
        std::vector<std::function<void()>> _restorePreviewFuncs{};

        HRESULT _OnNewConnection(const winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection& connection);
        void _HandleToggleInboundPty(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

        void _WindowRenamerActionClick(const IInspectable& sender, const IInspectable& eventArgs);
        void _RequestWindowRename(const winrt::hstring& newName);
        void _WindowRenamerKeyDown(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _WindowRenamerKeyUp(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);

        void _UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element);

        winrt::Microsoft::Terminal::Settings::Model::Profile GetClosestProfileForDuplicationOfProfile(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile) const noexcept;

        bool _maybeElevate(const winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs,
                           const winrt::Microsoft::Terminal::Settings::Model::TerminalSettingsCreateResult& controlSettings,
                           const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);
        void _OpenElevatedWT(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        safe_void_coroutine _ConnectionStateChangedHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args);
        void _CloseOnExitInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        void _KeyboardServiceWarningInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        static bool _IsMessageDismissed(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);
        static void _DismissMessage(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);

        void _updateThemeColors();
        void _updateAllTabCloseButtons();
        void _updatePaneResources(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme);

        safe_void_coroutine _ControlCompletionsChangedHandler(const winrt::Windows::Foundation::IInspectable sender, const winrt::Microsoft::Terminal::Control::CompletionsChangedEventArgs args);

        void _OpenSuggestions(const Microsoft::Terminal::Control::TermControl& sender, Windows::Foundation::Collections::IVector<winrt::Microsoft::Terminal::Settings::Model::Command> commandsCollection, winrt::TerminalApp::SuggestionsMode mode, winrt::hstring filterText);

        void _ShowWindowChangedHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::ShowWindowArgs args);
        Windows::Foundation::IAsyncAction _SearchMissingCommandHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::SearchMissingCommandEventArgs args);
        static Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Management::Deployment::MatchResult>> _FindPackageAsync(hstring query);

        void _WindowSizeChanged(const IInspectable sender, const winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs args);
        void _windowPropertyChanged(const IInspectable& sender, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args);

        void _onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& e);
        void _onTabStripDragOver(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::DragEventArgs& e);
        void _onTabStripDrop(winrt::Windows::Foundation::IInspectable sender, winrt::Windows::UI::Xaml::DragEventArgs e);
        void _onTabDroppedOutside(winrt::Windows::Foundation::IInspectable sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabDroppedOutsideEventArgs e);

        void _DetachPaneFromWindow(std::shared_ptr<Pane> pane);
        void _DetachTabFromWindow(const winrt::com_ptr<Tab>& tabImpl);
        void _MoveContent(std::vector<winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs>&& actions,
                          const winrt::hstring& windowName,
                          const uint32_t tabIndex,
                          const std::optional<winrt::Windows::Foundation::Point>& dragPoint = std::nullopt);
        void _sendDraggedTabToWindow(const winrt::hstring& windowId, const uint32_t tabIndex, std::optional<winrt::Windows::Foundation::Point> dragPoint);

        void _PopulateContextMenu(const Microsoft::Terminal::Control::TermControl& control, const Microsoft::UI::Xaml::Controls::CommandBarFlyout& sender, const bool withSelection);
        void _PopulateQuickFixMenu(const Microsoft::Terminal::Control::TermControl& control, const Windows::UI::Xaml::Controls::MenuFlyout& sender);
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _CreateRunAsAdminFlyout(int profileIndex);

        winrt::Microsoft::Terminal::Control::TermControl _senderOrActiveControl(const winrt::Windows::Foundation::IInspectable& sender);
        winrt::com_ptr<Tab> _senderOrFocusedTab(const IInspectable& sender);

        void _activePaneChanged(winrt::TerminalApp::Tab tab, Windows::Foundation::IInspectable args);
        safe_void_coroutine _doHandleSuggestions(Microsoft::Terminal::Settings::Model::SuggestionsArgs realArgs);

#pragma region ActionHandlers
        // These are all defined in AppActionHandlers.cpp
#define ON_ALL_ACTIONS(action) DECLARE_ACTION_HANDLER(action);
        ALL_SHORTCUT_ACTIONS
        INTERNAL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
#pragma endregion

        friend class TerminalAppLocalTests::TabTests;
        friend class TerminalAppLocalTests::SettingsTests;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalPage);
}

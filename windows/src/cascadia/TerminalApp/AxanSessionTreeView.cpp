// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanSessionTreeView — axan's session-tree sidebar: the navigator that replaced the
// tab strip (decision D20).
//
// This file holds the TerminalPage member definitions (plus their file-local static
// helpers) for everything the sidebar does:
//   * the session-tree model: minting a node per live session (_CreateSessionNode),
//     reconciling the tree against _tabs by identity (_AddMissingSessionNodes /
//     _PruneRemovedSessionNodes / _RemoveNodeSelfHealing), and the live templated
//     labels (_RecomputeSessionLabelAsync, off-UI-thread per #430);
//   * node activation, the per-node context menu (mouse + keyboard routes), and the
//     programmatic "Edit session node" editor with its persistence back to the curated
//     startup entries (_PersistNodeToEntry, #422/#427);
//   * the sidebar state machine (expanded / minimized / collapsed), the draggable
//     divider, and the minimized flat icon list.
//
// It is deliberately a separate translation unit of the same TerminalPage class —
// exactly like upstream's TabManagement.cpp — extracted from TabManagement.cpp (#436)
// so rebases onto new Windows Terminal releases don't conflict with ~1,800 lines of
// axan feature code sitting inside the file upstream modifies most. The only axan code
// left in upstream files is the genuinely interleaved touchpoints (the _UpdateTabView
// strip retirement, the #428 reconcile suppression inside _TryMoveTab /
// _TabDragCompleted, and the event registrations in TerminalPage::Create).

#include "pch.h"
#include "TerminalPage.h"
#include "TabRowControl.h"
#include "AxanLog.h"
#include "Utils.h"
#include "../../types/inc/utils.hpp"
#include <LibraryResources.h>

#include "AxanLabelTemplate.h"
#include "AxanSessionState.h" // axan #30
#include <AxanStateGlyphs.h> // axan: sidebar.stateGlyphs
#include <AxanSidebarSizes.h> // axan: sidebar row size ranges and defaults
#include "SessionNodeViewModel.h"
#include <AxanIconRegistry.h>
#include <AxanLaunchEntryWire.h> // axan #3: separator kind/style/placement vocabulary for capture
#include <AxanHoistRule.h> // axan #34: the hoisted group's match rule
#include <AxanIdleEffects.h> // axan: sidebar.idleEffects
#include <AxanCheckBoxWrap.h> // full-label columns for the node card's hoist checkboxes
#include <AxanEntryIdentityEditor.h> // axan #38: the node editor's identity section, shared with Settings

#include <shlobj.h>
#include <array>
#include <deque> // axan #30: stable string owners for the session-state inputs
#include <map> // axan #3: startup-separator insert counters
#include <unordered_set> // axan #14: captured-entry id dedup

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace winrt::TerminalApp::implementation
{
    // axan #436 item 2: the expanded sidebar's width clamp, shared by the persisted-width
    // read (_expandedSidebarWidth) and the divider's live drag clamp (_OnSidebarResizeDelta)
    // — previously declared independently in both. The minimized column's width steps live
    // in _minimizedSidebarWidth's switch below.
    static constexpr double kSidebarExpandedMinWidth = 180.0;
    static constexpr double kSidebarExpandedMaxWidth = 600.0;

    // axan #436 item 2: the theme-default node-label text colors (white-on-dark /
    // near-black-on-light), used by _RefreshNodeLabelBrushes. NOTE: SessionNodeViewModel's
    // LabelBrush fallback (SessionNodeViewModel.cpp) carries its own copy of the same two
    // colors — unifying that copy means a shared header; left as a known duplicate for now.
    static constexpr winrt::Windows::UI::Color kDefaultLabelColorDark{ 255, 255, 255, 255 };
    static constexpr winrt::Windows::UI::Color kDefaultLabelColorLight{ 255, 0x1a, 0x1a, 0x1a };

    // axan M2: add a sidebar tree node mirroring the session (tab) at the given index.
    // The node label tracks the tab's Title (the active/"main" pane's running program and
    // cwd-derived title), updated whenever the tab raises PropertyChanged for "Title".
    // axan M5: the basename of the executable in a profile commandline, used for the
    // $shell template variable. Handles a quoted exe ("C:\path with space\pwsh.exe" -arg)
    // and a bare first token. Returns "" when there's nothing usable.
    static std::wstring _shellBasenameFromCommandline(std::wstring_view cmdline)
    {
        // Trim leading whitespace.
        while (!cmdline.empty() && (cmdline.front() == L' ' || cmdline.front() == L'\t'))
        {
            cmdline.remove_prefix(1);
        }
        if (cmdline.empty())
        {
            return {};
        }
        std::wstring_view exe;
        if (cmdline.front() == L'"')
        {
            const auto close = cmdline.find(L'"', 1);
            exe = cmdline.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1);
        }
        else
        {
            const auto space = cmdline.find_first_of(L" \t");
            exe = cmdline.substr(0, space);
        }
        if (exe.empty())
        {
            return {};
        }
        return std::filesystem::path{ exe }.filename().wstring();
    }

    // axan M6: each session node's Content is a SessionNodeViewModel (its stable identity
    // through drag reorder/reparent). Setting the VM's observable Label refreshes the bound
    // TreeViewItem via INotifyPropertyChanged — this retires the M5 ContainerFromNode poke,
    // which was a workaround for plain-string Content not refreshing a realized item.
    // axan #429: this also keeps the row's UIA name current — the VM is IStringable over
    // Label, and the unbound TreeViewItem peer's GetNameCore falls back to the node
    // Content's string representation (see SessionNodeViewModel.h), so the name is read
    // fresh from the VM on every Narrator query. No per-container SetName poke needed.
    static void _setNodeLabelText(const MUX::Controls::TreeViewNode& node, const winrt::hstring& text)
    {
        if (const auto vm = node.Content().try_as<winrt::TerminalApp::SessionNodeViewModel>())
        {
            vm.Label(text);
        }
    }

    // axan M6: the SessionNodeViewModel behind a node, or nullptr. The impl pointer exposes
    // the identity payload (TabRef / LabelTemplate) that's deliberately not in the IDL.
    static winrt::com_ptr<winrt::TerminalApp::implementation::SessionNodeViewModel> _nodeVM(const MUX::Controls::TreeViewNode& node)
    {
        if (const auto vm = node.Content().try_as<winrt::TerminalApp::SessionNodeViewModel>())
        {
            winrt::com_ptr<winrt::TerminalApp::implementation::SessionNodeViewModel> impl;
            impl.copy_from(winrt::get_self<winrt::TerminalApp::implementation::SessionNodeViewModel>(vm));
            return impl;
        }
        return nullptr;
    }

    // axan M6: the Tab a node stands for (its VM's weak TabRef, resolved), or nullptr.
    static winrt::TerminalApp::Tab _nodeTab(const MUX::Controls::TreeViewNode& node)
    {
        if (const auto vm = _nodeVM(node))
        {
            if (const auto inspectable = vm->TabRef.get())
            {
                return inspectable.try_as<winrt::TerminalApp::Tab>();
            }
        }
        return nullptr;
    }

    // axan #30: the terminal title only reaches session state as the agent name, and only
    // after CleanTerminalTitle strips the leading spinner/status mark. Claude Code rewrites
    // its title several times a second just to animate that spinner, so the Title handler
    // re-derives the node only when the cleaned titles of the tab's panes actually change.
    // Returns true (and records the new value) on a change.
    static bool _cleanedTitlesChanged(const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        const auto tab = _nodeTab(node);
        if (!vm || !tab)
        {
            return false;
        }
        std::wstring key;
        for (const auto& control : winrt::get_self<winrt::TerminalApp::implementation::Tab>(tab)->AxanTerminalControls())
        {
            key += Axan::SessionState::CleanTerminalTitle(control.Title());
            key += L'';
        }
        if (key == vm->LastCleanedTitles)
        {
            return false;
        }
        vm->LastCleanedTitles = std::move(key);
        return true;
    }

    // axan #3: is this node a separator row (a VM with IsSeparator, no session behind it)?
    // Every node-level path that assumes a Tab checks this (or _nodeTab's null) first.
    static bool _nodeIsSeparator(const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        return vm && vm->IsSeparator();
    }

    // axan #3: is this node a separator pinned to the bottom of its sibling scope?
    static bool _nodeIsBottomSeparator(const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        return vm && vm->IsSeparator() && vm->Placement() == Axan::LaunchEntryWire::PlacementBottomW;
    }

    // axan M6: depth-first visit of every node in the tree (roots + all descendants). The
    // callback must not mutate the collection it's walking — callers that remove nodes
    // collect first, then mutate (see _PruneRemovedSessionNodes).
    static void _forEachSessionNode(const Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>& nodes,
                                    const std::function<void(const MUX::Controls::TreeViewNode&)>& fn)
    {
        if (!nodes)
        {
            return;
        }
        for (const auto& node : nodes)
        {
            fn(node);
            _forEachSessionNode(node.Children(), fn);
        }
    }

    // axan M6: find the collection a node lives in (its siblings) and its index there.
    // Searched explicitly rather than via TreeViewNode.Parent() so it doesn't depend on
    // whether a root node's Parent() is null or the TreeView's hidden root. Returns false
    // if the node isn't found anywhere under the roots.
    static bool _locateNodeContainer(const Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>& roots,
                                     const MUX::Controls::TreeViewNode& node,
                                     Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>& outContainer,
                                     uint32_t& outIndex)
    {
        uint32_t index = 0;
        if (roots.IndexOf(node, index))
        {
            outContainer = roots;
            outIndex = index;
            return true;
        }
        for (const auto& candidate : roots)
        {
            if (_locateNodeContainer(candidate.Children(), node, outContainer, outIndex))
            {
                return true;
            }
        }
        return false;
    }

    // axan M5/#430: gather the label context — every input the template expansion needs —
    // from a tab's active control, ON the UI thread. The expansion itself runs on a
    // background thread (_RecomputeSessionLabelAsync below): a git label variable walks the
    // cwd's filesystem, which must never block the UI thread. Free + static so the
    // lifetime-safe recompute lambdas can call it without capturing the page.
    static Axan::LabelContext _GatherSessionLabelContext(const winrt::TerminalApp::Tab& tab)
    {
        Axan::LabelContext ctx;
        const auto tabImpl = winrt::get_self<Tab>(tab);
        if (const auto control = tabImpl->GetActiveTerminalControl())
        {
            ctx.cwd = std::wstring{ control.CurrentWorkingDirectory() };
            ctx.title = std::wstring{ control.Title() };
        }
        else
        {
            // No control yet (very early) — fall back to the tab's own title.
            ctx.title = std::wstring{ tab.Title() };
        }
        if (const auto profile = tabImpl->GetFocusedProfile())
        {
            ctx.shell = _shellBasenameFromCommandline(std::wstring_view{ profile.Commandline() });
        }
        return ctx;
    }

    // axan #430 item 1 (and #427 item 2): recompute a node's label without blocking the UI
    // thread. On the UI thread: read the template FRESH off the VM (an M13 editor rename takes
    // effect immediately — a captured copy made renames revert on the next title/output
    // change), gather the LabelContext, and stamp a generation. On a background thread: expand
    // the template (a git variable can walk the cwd's filesystem — for a hostile OSC 9;9 cwd
    // that walk can stall for an SMB timeout, so it must never run on the UI thread). Back on
    // the UI thread: apply the text iff the node is still alive and the generation is still
    // current, so a stale (slower) expansion never overwrites a newer label. Weak refs
    // throughout — an in-flight recompute extends no lifetime.
    static safe_void_coroutine _RecomputeSessionLabelAsync(winrt::weak_ref<MUX::Controls::TreeViewNode> weakNode,
                                                           winrt::weak_ref<winrt::TerminalApp::Tab> weakTab)
    {
        winrt::hstring tmpl;
        Axan::LabelContext ctx;
        uint64_t generation = 0;
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        {
            const auto node = weakNode.get();
            const auto tab = weakTab.get();
            if (!node || !tab)
            {
                co_return;
            }
            const auto vm = _nodeVM(node);
            if (!vm)
            {
                co_return;
            }
            tmpl = vm->LabelTemplate;
            ctx = _GatherSessionLabelContext(tab);
            generation = ++vm->LabelGeneration;
            dispatcher = node.Dispatcher();
        } // strong refs released before the hop — the background work holds nothing alive
        if (!dispatcher)
        {
            co_return; // can't marshal a result back — skip rather than touch XAML off-thread
        }

        co_await winrt::resume_background();
        const winrt::hstring text{ Axan::ComputeLabel(std::wstring_view{ tmpl }, ctx) };
        co_await wil::resume_foreground(dispatcher);

        if (const auto node = weakNode.get())
        {
            const auto vm = _nodeVM(node);
            if (vm && vm->LabelGeneration.load() == generation)
            {
                _setNodeLabelText(node, text);
            }
        }
    }

    // axan M7: deterministically pick a builtin glyph for a session that has no icon of its
    // own (no override, no profile icon). Keyed on the profile GUID (stable across launches)
    // so the same session always gets the same glyph; falls back to the tab title for a
    // profile-less tab. FNV-1a rather than std::hash because std::hash carries no cross-run
    // stability guarantee, and we want the glyph to survive a relaunch. The glyphs are Segoe
    // Fluent / MDL2 symbols; IconPathConverter renders them as a FontIconSource, which paints
    // with the element foreground — so an auto-assigned glyph recolors with the theme for free.
    // axan #436 item 1: the candidate pool is AxanIconRegistry's builtin vocabulary — the
    // same set the node editor's icon picker offers — so an auto-assigned glyph always
    // exports as a portable `builtin:NAME` token in the TOML interchange (the old parallel
    // 8-glyph table here mostly exported raw PUA chars that Linux renders as nothing).
    static winrt::hstring _autoAssignGlyph(const winrt::TerminalApp::Tab& tab)
    {
        const auto& glyphs = Axan::IconRegistry::Builtins();

        std::wstring key;
        if (tab)
        {
            const auto tabImpl = winrt::get_self<Tab>(tab);
            if (const auto profile = tabImpl->GetFocusedProfile())
            {
                key = ::Microsoft::Console::Utils::GuidToString(profile.Guid());
            }
            else
            {
                key = std::wstring{ tab.Title() };
            }
        }

        uint64_t h = 1469598103934665603ULL; // FNV offset basis
        for (const auto c : key)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL; // FNV prime
        }
        return winrt::hstring{ std::wstring(1, glyphs[static_cast<size_t>(h % glyphs.size())].glyph) };
    }

    // axan M7: resolve a node's icon PATH along the divergence chain:
    // per-node override, else the session's profile icon, else an auto-assigned builtin
    // glyph. Returns the path string — not a built element — because SessionNodeViewModel
    // mints a fresh IconElement from it on every get (a cached element re-parented during a
    // drag reparent crashes; see SessionNodeViewModel.h). The path can be a file (PNG/ICO/SVG),
    // an exe/dll/lnk to extract from, a Segoe glyph, or an emoji — IconWUX handles each.
    static winrt::hstring _resolveNodeIconPath(const winrt::hstring& iconOverride,
                                               const winrt::TerminalApp::Tab& tab)
    {
        winrt::hstring path = iconOverride;
        if (path.empty() && tab)
        {
            const auto tabImpl = winrt::get_self<Tab>(tab);
            if (const auto profile = tabImpl->GetFocusedProfile())
            {
                if (const auto media = profile.Icon())
                {
                    path = media.Resolved();
                }
            }
        }
        if (path.empty())
        {
            path = _autoAssignGlyph(tab);
        }
        return path;
    }

    // axan M5/M6: receive the per-node startup metadata parsed from the settings.json startup tree — each
    // node's label template (M5) and its parent's spawn index (M6, -1 = root) — drained in
    // DFS spawn order as the startup tabs are created. Called before SetStartupActions
    // (TerminalWindow), so the queues are primed before any tab inserts. _startupNodesBySpawnIndex
    // is sized up front so a child node can find its (earlier-spawned) parent on creation.
    void TerminalPage::SetStartupNodeMetadata(std::vector<winrt::hstring> templates, std::vector<int32_t> parentIndices, std::vector<winrt::hstring> iconOverrides, std::vector<winrt::hstring> iconColors, std::vector<winrt::hstring> colorTargets, std::vector<winrt::hstring> entryIds)
    {
        _pendingStartupLabelTemplates = std::move(templates);
        _pendingStartupParents = std::move(parentIndices);
        _pendingStartupIconOverrides = std::move(iconOverrides); // M13
        _pendingStartupIconColors = std::move(iconColors); // M13
        _pendingStartupColorTargets = std::move(colorTargets); // M13
        _pendingStartupEntryIds = std::move(entryIds); // #422
        _startupNodeCursor = 0;
        _startupNodesBySpawnIndex.clear();
        _startupNodesBySpawnIndex.resize(_pendingStartupLabelTemplates.size());
    }

    // axan #3: receive the startup tree's separator rows (TerminalWindow, next to
    // SetStartupNodeMetadata). They spawn no tab, so they aren't drained by the spawn cursor;
    // _RealizeStartupSeparators inserts them in one pass once every startup session node exists.
    void TerminalPage::SetStartupSeparators(std::vector<Axan::StartupSeparator> separators)
    {
        _pendingStartupSeparators = std::move(separators);
        _startupSeparatorsRealized = false;
        Axan::Log::Debug("TerminalPage", "startup separators primed", { { "count", std::to_string(_pendingStartupSeparators.size()) } });
    }

    // axan #3: one session row's height in px — the TreeViewItemMinHeight theme resource the
    // sidebar's item style used to floor every row at (now carried by the template's session
    // Grid instead, so a separator can be shorter). A separator's pixel height is its
    // row-unit Height times this. Falls back to WinUI 2.8's 32 if the lookup fails.
    // axan #32/#31: rows are no longer all one height (a state second line makes a row
    // taller; an idle-de-emphasised row is shorter), so the separator "row unit" is pinned
    // to this value - one full-size, single-line session row - rather than to whatever the
    // neighbouring rows currently measure. A separator therefore keeps its height while the
    // rows around it change state.
    double TerminalPage::_SessionRowHeightPx()
    {
        constexpr double kFallbackRowHeight = 32.0;
        try
        {
            if (const auto res = Application::Current().Resources().TryLookup(winrt::box_value(winrt::hstring{ L"TreeViewItemMinHeight" })))
            {
                if (const auto px = winrt::unbox_value_or<double>(res, 0.0); px > 0.0)
                {
                    return px;
                }
            }
        }
        catch (...)
        {
        }
        Axan::Log::Debug("TerminalPage", "TreeViewItemMinHeight not resolvable; using fallback row height", { { "fallbackPx", std::to_string(kFallbackRowHeight) } });
        return kFallbackRowHeight;
    }

    // axan #3: mint a separator node. Its Content is a SessionNodeViewModel like every other
    // row (one node type, one ItemTemplate), flipped into separator mode: IsSeparator, the
    // normalized style/height/placement, and no TabRef — so every Tab-resolving path sees
    // null and skips it. EntryId is kept so "Save current as startup" round-trips the row. The
    // node isn't parented here; the caller inserts it at its resolved position.
    MUX::Controls::TreeViewNode TerminalPage::_CreateSeparatorNode(const winrt::hstring& entryId, const winrt::hstring& style, double height, const winrt::hstring& placement, const winrt::hstring& label, bool collapsed)
    {
        auto vm = winrt::make<winrt::TerminalApp::implementation::SessionNodeViewModel>();
        const auto vmImpl = winrt::get_self<winrt::TerminalApp::implementation::SessionNodeViewModel>(vm);
        vmImpl->EntryId = entryId;
        vmImpl->ConfigureSeparator(style, height, placement, _SessionRowHeightPx());
        // axan #33: a label makes it a collapsible section header.
        vmImpl->SetSeparatorLabel(label);
        vmImpl->SetCollapsed(collapsed);

        MUX::Controls::TreeViewNode node;
        node.Content(vm);
        vmImpl->OnSectionBadgeActivated = [weakThis{ get_weak() }, weakNode = winrt::make_weak(node)]() {
            if (const auto page{ weakThis.get() })
            {
                if (const auto n = weakNode.get())
                {
                    page->_ActivateSectionBadge(n);
                }
            }
        };
        Axan::Log::Debug("TerminalPage", "created separator node", { { "entryId", winrt::to_string(entryId) }, { "style", winrt::to_string(vmImpl->SeparatorStyle()) }, { "height", std::to_string(vmImpl->SeparatorHeight()) }, { "placement", winrt::to_string(vmImpl->Placement()) }, { "pixelHeight", std::to_string(vmImpl->SeparatorPixelHeight()) } });
        return node;
    }

    // axan #3: insert the startup tree's separator rows once every startup session node has
    // been realized (the spawn cursor has reached the end) — or at once when there were no
    // startup sessions. Each separator resolves its parent container (RootNodes, or the
    // parent session's Children) and lands right after the sibling session it followed in
    // the entry list (afterSpawnIndex); -1 means it led its scope, so it goes first. A
    // referenced node that never spawned (a session that failed to launch) degrades to an
    // append at the end of the scope. Consecutive separators after the same sibling keep
    // their list order via a per-(scope, sibling) insert counter. Ends with the bottom-sink
    // pass so "bottom"-placed separators settle under the sessions that spawned around them.
    void TerminalPage::_RealizeStartupSeparators()
    {
        if (_startupSeparatorsRealized)
        {
            return;
        }
        if (_startupNodeCursor < _pendingStartupLabelTemplates.size())
        {
            return; // startup sessions still spawning — their nodes anchor the separators
        }
        _startupSeparatorsRealized = true;
        if (_pendingStartupSeparators.empty())
        {
            return;
        }

        const auto roots = SessionTree().RootNodes();
        // (scope spawn index, after spawn index) -> how many separators already inserted there,
        // so a run of separators after one sibling keeps its authored order.
        std::map<std::pair<int32_t, int32_t>, uint32_t> insertedAfter;
        size_t placed = 0;
        size_t appended = 0;
        for (const auto& sep : _pendingStartupSeparators)
        {
            MUX::Controls::TreeViewNode parentNode{ nullptr };
            if (sep.parentSpawnIndex >= 0 && static_cast<size_t>(sep.parentSpawnIndex) < _startupNodesBySpawnIndex.size())
            {
                parentNode = _startupNodesBySpawnIndex[static_cast<size_t>(sep.parentSpawnIndex)].get();
            }
            if (!parentNode && sep.parentSpawnIndex >= 0)
            {
                Axan::Log::Warn("TerminalPage", "startup separator: parent session node never spawned; placing at root", { { "entryId", winrt::to_string(sep.id) }, { "parentSpawnIndex", std::to_string(sep.parentSpawnIndex) } });
            }
            const auto container = parentNode ? parentNode.Children() : roots;
            const int32_t scopeKey = parentNode ? sep.parentSpawnIndex : -1;

            uint32_t insertAt = container.Size();
            bool resolved = false;
            if (sep.afterSpawnIndex < 0)
            {
                insertAt = 0;
                resolved = true;
            }
            else if (static_cast<size_t>(sep.afterSpawnIndex) < _startupNodesBySpawnIndex.size())
            {
                if (const auto after = _startupNodesBySpawnIndex[static_cast<size_t>(sep.afterSpawnIndex)].get())
                {
                    uint32_t afterIndex = 0;
                    if (container.IndexOf(after, afterIndex))
                    {
                        insertAt = afterIndex + 1;
                        resolved = true;
                    }
                }
            }
            if (resolved)
            {
                insertAt += insertedAfter[{ scopeKey, sep.afterSpawnIndex }]++;
                insertAt = std::min(insertAt, container.Size());
                ++placed;
            }
            else
            {
                Axan::Log::Warn("TerminalPage", "startup separator: preceding sibling session node not found in scope; appending", { { "entryId", winrt::to_string(sep.id) }, { "afterSpawnIndex", std::to_string(sep.afterSpawnIndex) } });
                ++appended;
            }

            const auto node = _CreateSeparatorNode(sep.id, sep.style, sep.height, sep.placement, sep.label, sep.collapsed);
            container.InsertAt(insertAt, node);
            if (parentNode)
            {
                parentNode.IsExpanded(true);
            }
        }
        _pendingStartupSeparators.clear();
        _SinkBottomSeparators();
        _ApplySeparatorContainerStates();
        Axan::Log::Info("TerminalPage", "realized startup separators", { { "placed", std::to_string(placed) }, { "appended", std::to_string(appended) } });
    }

    // axan #3: called once the startup action loop has run to its end. The spawn cursor only
    // advances per tab that actually landed in _tabs, and a startup action can legitimately
    // create none here: a profile with "elevate": true opens a separate elevated window, a
    // profile that no longer resolves returns S_FALSE, a connection that threw is swallowed by
    // CATCH_RETURN. Left alone, a short cursor would (a) keep _RealizeStartupSeparators waiting
    // forever, so the separators silently never appeared, and (b) hand the leftover startup
    // slot — label, parent, icon, entry id — to the next tab the user opened interactively.
    // Release the unspawned slots and realize now; a separator anchored to a session that
    // never spawned degrades to an append at the end of its scope (see above).
    void TerminalPage::_MarkStartupSpawnComplete()
    {
        const auto expected = _pendingStartupLabelTemplates.size();
        if (_startupNodeCursor < expected)
        {
            Axan::Log::Warn("TerminalPage", "startup spawn finished with fewer tabs than entries; releasing the unspawned startup slots", { { "spawned", std::to_string(_startupNodeCursor) }, { "expected", std::to_string(expected) } });
            _startupNodeCursor = expected;
        }
        _RealizeStartupSeparators();
    }

    // axan #3: separators have no tab, so the _tabs reconcile never prunes them — and nothing
    // else rebuilds the live tree from settings before a relaunch. Without this, a separator
    // deleted on the Startup sessions page (or dropped by a TOML import) stayed in the sidebar
    // with no way to remove it, and the next "Save current as startup" snapshot wrote it
    // straight back. On every settings reload, drop the live separators whose entry id is no
    // longer a separator in startupSessions. Additions are NOT reconciled (their position
    // depends on sessions that may not exist yet) — like sessions, they appear on the next launch.
    void TerminalPage::_ReconcileSeparatorsWithSettings()
    {
        if (!SessionTree())
        {
            return;
        }
        std::set<winrt::hstring> persisted;
        std::map<winrt::hstring, winrt::hstring> labels; // axan #33: live label edits
        if (const auto entries = _settings.GlobalSettings().StartupSessions())
        {
            for (const auto& entry : entries)
            {
                if (entry.IsSeparator() && !entry.Id().empty())
                {
                    persisted.insert(entry.Id());
                    labels[entry.Id()] = entry.Name();
                }
            }
        }
        // axan #33: a header label edited on the Startup sessions page applies live (the
        // collapsed flag is the startup state, so it is left to the next launch).
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            if (const auto vm = _nodeVM(node); vm && vm->IsSeparator() && !vm->EntryId.empty())
            {
                if (const auto it = labels.find(vm->EntryId); it != labels.end())
                {
                    vm->SetSeparatorLabel(it->second);
                }
            }
        });
        std::vector<MUX::Controls::TreeViewNode> stale;
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            const auto vm = _nodeVM(node);
            if (!vm || !vm->IsSeparator() || vm->EntryId.empty())
            {
                return; // live-only separators (no id) have nothing in settings to disagree with
            }
            if (persisted.count(vm->EntryId) == 0)
            {
                stale.push_back(node);
            }
        });
        for (const auto& node : stale)
        {
            _RemoveNodeSelfHealing(node);
        }
        if (!stale.empty())
        {
            Axan::Log::Info("TerminalPage", "settings reload: removed separators no longer in startupSessions", { { "removed", std::to_string(stale.size()) } });
        }
    }

    // axan #3: the bottom-sink pass for one sibling scope, then recursively every child scope.
    // Separators with Placement "bottom" move to the end of their container, keeping their
    // relative order; nothing else moves, and a scope whose bottom separators already form
    // the trailing run is left untouched (no RemoveAt/Append churn, no container recycling).
    static void _sinkBottomSeparatorsIn(const Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>& container, size_t& moved)
    {
        if (!container)
        {
            return;
        }
        std::vector<MUX::Controls::TreeViewNode> bottoms;
        std::vector<MUX::Controls::TreeViewNode> snapshot;
        snapshot.reserve(container.Size());
        for (const auto& node : container)
        {
            snapshot.push_back(node);
            if (_nodeIsBottomSeparator(node))
            {
                bottoms.push_back(node);
            }
        }
        if (!bottoms.empty() && bottoms.size() < snapshot.size())
        {
            const auto n = snapshot.size();
            const auto k = bottoms.size();
            bool inPlace = true;
            for (size_t i = 0; i < k; ++i)
            {
                if (snapshot[n - k + i] != bottoms[i])
                {
                    inPlace = false;
                    break;
                }
            }
            if (!inPlace)
            {
                for (const auto& b : bottoms)
                {
                    uint32_t idx = 0;
                    if (container.IndexOf(b, idx))
                    {
                        container.RemoveAt(idx);
                    }
                }
                for (const auto& b : bottoms)
                {
                    container.Append(b);
                }
                moved += k;
            }
        }
        for (const auto& node : snapshot)
        {
            _sinkBottomSeparatorsIn(node.Children(), moved);
        }
    }

    // axan #3: pin every "bottom"-placed separator to the end of its sibling scope (RootNodes
    // and, recursively, each node's Children). Run after every tree mutation that can put a
    // session below one: startup realization, the _tabs reconcile (new sessions append at the
    // end of their scope), a drag-and-drop, and Add child session.
    void TerminalPage::_SinkBottomSeparators()
    {
        size_t moved = 0;
        _sinkBottomSeparatorsIn(SessionTree().RootNodes(), moved);
        if (moved > 0)
        {
            Axan::Log::Debug("TerminalPage", "sank bottom-placed separators", { { "moved", std::to_string(moved) } });
        }
        // axan #33: every tree mutation funnels through here, so re-derive the sections.
        _RecomputeSections();
    }

    // axan M6: build a sidebar node for `tab` whose Content is a SessionNodeViewModel (the
    // node's stable identity + label template + displayed label), and wire the live label
    // recompute. The node is not yet parented — the caller appends it under a parent or at
    // the root. The recompute handlers weak-capture node + tab so they can never extend a
    // lifetime; setting the VM's observable Label refreshes the bound item (no M5 container poke).
    MUX::Controls::TreeViewNode TerminalPage::_CreateSessionNode(const winrt::TerminalApp::Tab& tab, const winrt::hstring& labelTemplate, const winrt::hstring& iconOverride, const winrt::hstring& iconColor, const winrt::hstring& colorTarget, const winrt::hstring& entryId)
    {
        auto vm = winrt::make<winrt::TerminalApp::implementation::SessionNodeViewModel>();
        const auto vmImpl = winrt::get_self<winrt::TerminalApp::implementation::SessionNodeViewModel>(vm);
        {
            vmImpl->TabRef = winrt::make_weak(tab.as<winrt::Windows::Foundation::IInspectable>());
            vmImpl->LabelTemplate = labelTemplate;
            vmImpl->EntryId = entryId; // #422: remember the source entry so a Save can persist back to it
            vmImpl->HoistOverride = _HoistOverrideForEntry(entryId); // axan #34
            // axan M7/M13: seed the per-node icon override + recolor (persisted from a prior
            // run, or empty for a fresh tab), then resolve and store the icon path
            // (override -> profile -> auto-assigned glyph). The icon only changes when the
            // override/profile/color changes — the M13 picker re-resolves via _ApplyNodeIcon;
            // neither the Title nor OutputIdle recompute below touches it. The VM mints a fresh
            // IconElement from this path on each realization (no cached UIElement — drag-safe).
            vmImpl->IconOverride = iconOverride;
            vmImpl->ApplyAppearance(_resolveNodeIconPath(iconOverride, tab), iconColor, colorTarget);
        }
        // Seed a cheap synchronous placeholder (the walk-free default label) so the row is
        // never blank; the async recompute below delivers the templated text (#430 moved the
        // expansion — whose git variables can walk the filesystem — off the UI thread).
        vm.Label(winrt::hstring{ Axan::DefaultLabel(_GatherSessionLabelContext(tab)) });

        MUX::Controls::TreeViewNode node;
        node.Content(vm);

        // Recompute whenever the title (OSC 0/2) or — via the active control's debounced
        // OutputIdle — the cwd settles. The template rides on the VM and is read FRESH at
        // recompute time (#427 item 2: the M13 editor's Save rewrites it, so a captured copy
        // made renames revert on the next title/output change). Any captured ref being gone
        // makes the handler a no-op.
        auto weakNode = winrt::make_weak(node);
        auto weakTab = winrt::make_weak(tab);
        const auto recompute = [weakNode, weakTab]() {
            _RecomputeSessionLabelAsync(weakNode, weakTab);
        };

        // #427 item 3: every subscription stores a type-erased unsubscriber on the VM, revoked
        // when the node is pruned (_RemoveNodeSelfHealing) — so re-minting a node for a
        // still-live tab can never stack duplicate handlers on the tab/control.
        {
            const auto titleToken = tab.PropertyChanged([recompute, weakThis{ get_weak() }, weakNode](auto&&, const WUX::Data::PropertyChangedEventArgs& e) {
                if (e.PropertyName() == L"Title")
                {
                    recompute();
                    // axan: sidebar.showAgentName reads the terminal title; re-derive only
                    // when its cleaned form changed (spinner frames don't count).
                    if (const auto page{ weakThis.get() })
                    {
                        if (const auto n = weakNode.get(); n && _cleanedTitlesChanged(n))
                        {
                            page->_RecomputeNodeState(n);
                        }
                    }
                }
            });
            vmImpl->RevokeTitleSubscription = [weakTab, titleToken]() {
                if (const auto t{ weakTab.get() })
                {
                    t.PropertyChanged(titleToken);
                }
            };
        }

        // OutputIdle follows the tab's ACTIVE control: hook the current one now, and rehook on
        // every ActivePaneChanged (#427 item 3 — hooking only the startup-time control stranded
        // the subscription after a pane split, so cwd-settle label updates stopped following
        // the active pane). Rehooking first revokes the previous control's subscription.
        const auto hookActiveControlOutputIdle = [weakNode, weakTab, recompute]() {
            const auto n = weakNode.get();
            const auto t = weakTab.get();
            if (!n || !t)
            {
                return;
            }
            const auto nodeVm = _nodeVM(n);
            if (!nodeVm)
            {
                return;
            }
            if (nodeVm->RevokeOutputIdleSubscription)
            {
                nodeVm->RevokeOutputIdleSubscription();
                nodeVm->RevokeOutputIdleSubscription = nullptr;
            }
            if (const auto control = winrt::get_self<Tab>(t)->GetActiveTerminalControl())
            {
                const auto token = control.OutputIdle([recompute](auto&&, auto&&) { recompute(); });
                nodeVm->RevokeOutputIdleSubscription = [weakControl = winrt::make_weak(control), token]() {
                    if (const auto c{ weakControl.get() })
                    {
                        c.OutputIdle(token);
                    }
                };
            }
        };
        hookActiveControlOutputIdle();

        {
            const auto apToken = winrt::get_self<Tab>(tab)->ActivePaneChanged(
                [hookActiveControlOutputIdle, recompute, weakThis{ get_weak() }, weakNode, weakTab](auto&&, auto&&) {
                    hookActiveControlOutputIdle();
                    recompute(); // the newly-active pane's title/cwd are the label's inputs now
                    // axan #30: a pane of the focused tab gained focus -> its done/error is seen.
                    if (const auto page{ weakThis.get() })
                    {
                        const auto n = weakNode.get();
                        const auto t = weakTab.get();
                        if (n && t && t == page->_GetFocusedTab())
                        {
                            page->_AcknowledgeNodeState(n, "pane focused");
                        }
                    }
                });
            vmImpl->RevokeActivePaneSubscription = [weakTab, apToken]() {
                if (const auto t{ weakTab.get() })
                {
                    winrt::get_self<Tab>(t)->ActivePaneChanged(apToken);
                }
            };
        }

        // axan: the titlebar's centered title reads the focused node's Label
        // (TitlebarPrimaryText), but the async recompute above sets Label without
        // raising the page's TitleChanged — so a cwd-settle rename wouldn't reach the
        // titlebar. Bridge it: when THIS node's label changes and its tab is focused,
        // re-raise TitleChanged. Weak-captures only; the handler dies with the VM.
        vm.PropertyChanged([weakThis{ get_weak() }, weakTab](auto&&, const WUX::Data::PropertyChangedEventArgs& e) {
            if (e.PropertyName() != L"Label")
            {
                return;
            }
            if (const auto page{ weakThis.get() })
            {
                if (const auto t{ weakTab.get() })
                {
                    if (page->_GetFocusedTab() == t)
                    {
                        page->TitleChanged.raise(*page, nullptr);
                    }
                }
            }
        });

        // axan #30: session state. The tab raises AxanSessionStateChanged on the UI thread for
        // every state input (7501 records, 133 command lifecycle, bell, connection, panes
        // added/removed), so the node recomputes from all of its panes each time.
        {
            const auto stateToken = winrt::get_self<Tab>(tab)->AxanSessionStateChanged([weakThis{ get_weak() }, weakNode]() {
                if (const auto page{ weakThis.get() })
                {
                    if (const auto n = weakNode.get())
                    {
                        page->_RecomputeNodeState(n);
                    }
                }
            });
            vmImpl->RevokeSessionStateSubscription = [weakTab, stateToken]() {
                if (const auto t{ weakTab.get() })
                {
                    winrt::get_self<Tab>(t)->AxanSessionStateChanged(stateToken);
                }
            };
        }
        vmImpl->SetRowHeightPx(_SessionRowHeightPx()); // axan #31: the full-size row floor
        _RecomputeNodeState(node);
        _EnsureSessionStateClock();

        // Kick the first templated recompute (async; replaces the placeholder set above).
        recompute();

        return node;
    }

    // axan M6: the node standing for `tab`, found by identity (its VM's TabRef), or nullptr.
    MUX::Controls::TreeViewNode TerminalPage::_FindNodeForTab(const winrt::TerminalApp::Tab& tab)
    {
        MUX::Controls::TreeViewNode found{ nullptr };
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            if (!found && _nodeTab(node) == tab)
            {
                found = node;
            }
        });
        return found;
    }

    // axan M6: add a sidebar node for any live session (_tabs) that lacks one, in _tabs
    // order — the "add" half of reconciling the tree against _tabs by identity, replacing
    // the old positional InsertAt that a dragged/reparented tree would scramble. Startup
    // tabs (while the cursor is in range) consume their loaded template + parent link and
    // are reparented under the already-created parent node; interactively-created tabs
    // append at the root with the default label.
    void TerminalPage::_AddMissingSessionNodes()
    {
        for (uint32_t i = 0; i < _tabs.Size(); ++i)
        {
            const auto tab = _tabs.GetAt(i);
            if (_FindNodeForTab(tab))
            {
                continue;
            }

            const bool isStartup = _startupNodeCursor < _pendingStartupLabelTemplates.size();
            const auto spawnIndex = _startupNodeCursor;

            winrt::hstring tmpl;
            winrt::hstring iconOverride; // M13
            winrt::hstring iconColor; // M13
            winrt::hstring colorTarget; // M13
            winrt::hstring entryId; // #422
            int32_t parentIndex = -1;
            if (isStartup)
            {
                tmpl = _pendingStartupLabelTemplates[spawnIndex];
                parentIndex = spawnIndex < _pendingStartupParents.size() ? _pendingStartupParents[spawnIndex] : -1;
                iconOverride = spawnIndex < _pendingStartupIconOverrides.size() ? _pendingStartupIconOverrides[spawnIndex] : winrt::hstring{};
                iconColor = spawnIndex < _pendingStartupIconColors.size() ? _pendingStartupIconColors[spawnIndex] : winrt::hstring{};
                colorTarget = spawnIndex < _pendingStartupColorTargets.size() ? _pendingStartupColorTargets[spawnIndex] : winrt::hstring{};
                entryId = spawnIndex < _pendingStartupEntryIds.size() ? _pendingStartupEntryIds[spawnIndex] : winrt::hstring{};
                ++_startupNodeCursor;
            }

            const auto node = _CreateSessionNode(tab, tmpl, iconOverride, iconColor, colorTarget, entryId);

            MUX::Controls::TreeViewNode parentNode{ nullptr };
            if (isStartup && parentIndex >= 0 && static_cast<size_t>(parentIndex) < _startupNodesBySpawnIndex.size())
            {
                parentNode = _startupNodesBySpawnIndex[static_cast<size_t>(parentIndex)].get();
            }

            if (parentNode)
            {
                parentNode.Children().Append(node);
                parentNode.IsExpanded(true); // show the restored hierarchy on launch
            }
            else
            {
                SessionTree().RootNodes().Append(node);
            }

            if (isStartup)
            {
                _startupNodesBySpawnIndex[spawnIndex] = winrt::make_weak(node);
            }
        }
    }

    // axan M6: remove a node and promote its children into its place (same level, same
    // position) so closing a session never orphans the sessions nested under it — the
    // model's self-healing backstop (M6). Grandchildren ride along
    // inside each promoted child (their subtree is untouched).
    void TerminalPage::_RemoveNodeSelfHealing(const MUX::Controls::TreeViewNode& node)
    {
        // #427 item 3: unhook this node's recompute subscriptions from its tab/control first.
        // If the tab is still live (a node removed without its session — the reconcile path),
        // the handlers would otherwise sit on the tab's events forever, stacking a duplicate
        // pair every time a node is re-minted for it.
        if (const auto vm = _nodeVM(node))
        {
            vm->RevokeRecomputeSubscriptions();
        }

        Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode> container{ nullptr };
        uint32_t index = 0;
        if (!_locateNodeContainer(SessionTree().RootNodes(), node, container, index))
        {
            return;
        }

        // Detach the children from `node` first (a node can live in only one parent), then
        // splice them into `node`'s slot, then drop `node`.
        const auto children = node.Children();
        std::vector<MUX::Controls::TreeViewNode> promoted;
        promoted.reserve(children.Size());
        while (children.Size() > 0)
        {
            promoted.push_back(children.GetAt(0));
            children.RemoveAt(0);
        }

        container.RemoveAt(index);
        for (uint32_t k = 0; k < promoted.size(); ++k)
        {
            container.InsertAt(index + k, promoted[k]);
        }
    }

    // axan M6: remove sidebar nodes whose session is no longer live (the "prune" half of
    // reconciling against _tabs). Collect first, then remove, so we don't mutate the tree
    // mid-walk. A node with no resolvable VM is left alone — we only prune what we can prove
    // is dead.
    void TerminalPage::_PruneRemovedSessionNodes()
    {
        std::vector<MUX::Controls::TreeViewNode> dead;
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            const auto vm = _nodeVM(node);
            if (!vm)
            {
                return; // unknown node — don't touch it
            }
            if (vm->IsSeparator())
            {
                return; // axan #3: a separator has no tab to be dead; it lives until the tree is rebuilt
            }
            const auto tab = _nodeTab(node);
            uint32_t idx = 0;
            const bool live = tab && _tabs.IndexOf(tab, idx);
            if (!live)
            {
                dead.push_back(node);
            }
        });
        for (const auto& node : dead)
        {
            _RemoveNodeSelfHealing(node);
        }
    }

    // axan #436: the sidebar's resolved dark/light, shared by _RefreshNodeLabelBrushes and
    // the node editor (_ShowNodeEditPanel captures it as _nodeEditIsDark) — both previously
    // carried their own copy of this derivation. Application's RequestedTheme is unreliable —
    // WT forces its dark look via element theme, not the app theme — so the resolved
    // ActualTheme of the themed sidebar is the source of truth.
    // #422: `themeOverride` lets _updateThemeColors pass the just-set requested theme
    // directly: the sidebar's ActualTheme doesn't update until the next layout pass, so
    // reading it synchronously there returns the STALE theme (the "colors don't follow a
    // theme switch" bug). Default -> read the settled ActualTheme (Dark if no sidebar).
    bool TerminalPage::_SidebarIsDarkTheme(winrt::Windows::UI::Xaml::ElementTheme themeOverride)
    {
        auto theme = themeOverride;
        if (theme == winrt::Windows::UI::Xaml::ElementTheme::Default)
        {
            theme = winrt::Windows::UI::Xaml::ElementTheme::Dark;
            if (const auto sidebar = SessionSidebar())
            {
                theme = sidebar.ActualTheme();
            }
        }
        return theme != winrt::Windows::UI::Xaml::ElementTheme::Light;
    }

    // axan M13: push the theme-correct default label text brush to every node, derived from the
    // sidebar's *ActualTheme* (white-on-dark / near-black-on-light; see _SidebarIsDarkTheme for
    // why ActualTheme and how the override sidesteps its layout-pass lag). Called from
    // _updateThemeColors (theme switches) and after nodes are added.
    void TerminalPage::_RefreshNodeLabelBrushes(winrt::Windows::UI::Xaml::ElementTheme themeOverride)
    {
        if (!SessionTree())
        {
            return;
        }
        const bool isDark = _SidebarIsDarkTheme(themeOverride);
        const auto col = isDark ? kDefaultLabelColorDark : kDefaultLabelColorLight;
        const WUX::Media::SolidColorBrush brush{ col };
        // Pass isDark so a node's stored color NAME re-resolves to the right palette set; this
        // path runs on every theme switch (_updateThemeColors), so colors retrack the theme.
        // axan #26: nodes whose color and palette set are unchanged skip the repaint.
        size_t nodes = 0;
        size_t repainted = 0;
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            if (const auto vm = _nodeVM(node))
            {
                ++nodes;
                if (vm->SetDefaultTextBrush(brush, isDark))
                {
                    ++repainted;
                }
            }
        });
        Axan::Log::Debug("TerminalPage", "refresh node theme brushes", { { "isDark", isDark ? "1" : "0" }, { "themeOverride", std::to_string(static_cast<int32_t>(themeOverride)) }, { "nodes", std::to_string(nodes) }, { "repainted", std::to_string(repainted) } });
    }

    // axan M2/M6: keep the sidebar tree in sync with the session list (_tabs). Since drag
    // reorder/reparent (M6) detaches a node's tree position from its _tabs index, we no
    // longer act on the change's index — we reconcile by identity: drop nodes whose tab is
    // gone (self-healing their children up), then add nodes for tabs that lack one. This is
    // robust to every CollectionChange kind, and a Reset no longer flattens a user-built
    // hierarchy (surviving nodes keep their structure; only the delta is applied).
    void TerminalPage::_OnSessionsCollectionChanged(const Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::Tab>& /*sender*/,
                                                    const Windows::Foundation::Collections::IVectorChangedEventArgs& /*args*/)
    {
        // axan #428: a same-window tab move is a transient RemoveAt+InsertAt of the SAME tab,
        // and VectorChanged fires synchronously on each half. Reconciling mid-move would prune
        // the tab's node on the RemoveAt (wiping its label/icon/nesting and its EntryId
        // persistence link) and mint a blank replacement on the InsertAt. The tree is keyed by
        // identity, not _tabs index, so a pure reorder changes no node — skip entirely.
        if (_suppressSessionTreeReconcile)
        {
            return;
        }
        _PruneRemovedSessionNodes();
        _AddMissingSessionNodes();
        // axan #3: once the spawn cursor has consumed the last startup session, drop the
        // startup separators in around the nodes that now exist; then re-pin any
        // "bottom"-placed separator under the session that may just have appended below it.
        _RealizeStartupSeparators();
        _SinkBottomSeparators();
        // axan M13: give any newly-added node the theme-correct default label brush.
        _RefreshNodeLabelBrushes();
        // axan M8: keep the minimized icon list in step with the tree while it's showing
        // (startup spawn, new/closed sessions). It's hidden otherwise, so skip the work.
        if (_sidebarState == SidebarState::Minimized)
        {
            _RebuildMinimizedItems();
        }
    }

    // axan #436 item 3: resolve a session-node view-model to its live session and select it
    // — the one activation path shared by the expanded tree (_OnSessionTreeItemInvoked) and
    // the minimized icon list (_OnMinimizedItemClick), which used to carry duplicate copies
    // of this body. Resolution is by identity (the VM's weak TabRef), never tree position;
    // selecting through the TabView keeps the content swap on the existing selection path.
    void TerminalPage::_SelectTabForNodeVM(const winrt::TerminalApp::SessionNodeViewModel& vm)
    {
        if (!vm)
        {
            return;
        }
        const auto vmImpl = winrt::get_self<winrt::TerminalApp::implementation::SessionNodeViewModel>(vm);
        if (vmImpl->IsSeparator())
        {
            // axan #3: a separator stands for no session. Reachable only while Ctrl+Alt has
            // unlocked the row (it's disabled otherwise); activating it must not move the
            // selection off the current session.
            Axan::Log::Debug("TerminalPage", "separator row activated; ignoring");
            return;
        }
        if (const auto inspectable = vmImpl->TabRef.get())
        {
            if (const auto tab = inspectable.try_as<winrt::TerminalApp::Tab>())
            {
                uint32_t index = 0;
                if (_tabs.IndexOf(tab, index))
                {
                    _tabView.SelectedIndex(gsl::narrow_cast<int32_t>(index));
                }
            }
        }
    }

    // axan M2/M6: activating a sidebar node selects its session, resolved by identity (the
    // node's VM -> Tab), not by tree position — a reordered/reparented node still points at
    // the right session. Reuses the existing TabView selection path so the content swap and
    // (transitional) tab strip stay in sync.
    void TerminalPage::_OnSessionTreeItemInvoked(const MUX::Controls::TreeView& /*sender*/,
                                                 const MUX::Controls::TreeViewItemInvokedEventArgs& args)
    {
        const auto node = args.InvokedItem().try_as<MUX::Controls::TreeViewNode>();
        if (!node)
        {
            return;
        }
        if (_nodeIsSeparator(node))
        {
            // axan #33: clicking a section header toggles its section.
            if (const auto vm = _nodeVM(node); vm && vm->HasSectionHeader())
            {
                _SetSectionCollapsed(node, !vm->IsCollapsed(), "header click");
            }
            // axan #3: separators activate nothing (and _SelectTabForNodeVM would no-op anyway) —
            // but the click that got here just made the separator the tree's selection.
            _ResetSeparatorSelection("separator invoked");
            return;
        }
        _SelectTabForNodeVM(node.Content().try_as<winrt::TerminalApp::SessionNodeViewModel>());
    }

    // axan #16: after the native TreeView drag-and-drop lands, make the drop target show
    // its new child. WinUI's unbound-mode "drop onto a node" reparents the dragged node
    // correctly, but when the target was a leaf it stays collapsed and its container's
    // expander isn't refreshed for the leaf -> parent transition, so the dragged node
    // vanishes until something else expands the target (e.g. Add child session). Expand
    // the new parent explicitly (mirrors _AddChildSession) and force the chevron on —
    // TreeViewList only sets GlyphOpacity from HasChildren when the container is prepared.
    // Drops between siblings at the root leave Parent() as the tree's hidden root node
    // (Depth -1); nothing to expand there.
    // axan #3: also the drag's epilogue for separators — the drag-active freeze on the
    // Ctrl+Alt lock ends here, a row dropped ONTO a separator (separators are drop targets
    // during a drag, #28) is moved out to sit right after it at the separator's depth,
    // bottom-placed separators re-sink, and every realized container's lock state is
    // re-applied (the drop recycles containers).
    void TerminalPage::_OnSessionTreeDragItemsCompleted(const MUX::Controls::TreeView& /*sender*/,
                                                        const MUX::Controls::TreeViewDragItemsCompletedEventArgs& args)
    {
        _separatorDragActive = false;
        std::vector<MUX::Controls::TreeViewNode> droppedNodes; // axan #33
        const auto items = args.Items();
        if (items)
        {
            for (const auto& item : items)
            {
                const auto node = item.try_as<MUX::Controls::TreeViewNode>();
                if (!node)
                {
                    continue;
                }
                droppedNodes.push_back(node); // axan #33
                auto parent = node.Parent();
                if (parent && parent.Depth() >= 0 && _nodeIsSeparator(parent))
                {
                    // axan #28: a separator can't own children, so dropping a row (session or
                    // unlocked separator) ONTO a separator means "insert directly after it":
                    // relocate the dropped node to the separator's own scope, right after it.
                    // Separators are drop targets during a drag (see _ApplySeparatorContainerState)
                    // precisely so the drop takes this node-level path.
                    const auto separator = parent;
                    Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode> sepContainer{ nullptr };
                    uint32_t sepIndex = 0;
                    if (_locateNodeContainer(SessionTree().RootNodes(), separator, sepContainer, sepIndex))
                    {
                        uint32_t childIndex = 0;
                        if (separator.Children().IndexOf(node, childIndex))
                        {
                            separator.Children().RemoveAt(childIndex);
                        }
                        sepContainer.InsertAt(std::min(sepIndex + 1, sepContainer.Size()), node);
                        // The drop may have expanded the separator / shown its chevron; it's a
                        // leaf again, so put both back.
                        separator.IsExpanded(false);
                        if (const auto sepItem = SessionTree().ContainerFromNode(separator).try_as<MUX::Controls::TreeViewItem>())
                        {
                            sepItem.GlyphOpacity(0.0);
                        }
                        Axan::Log::Info("TerminalPage", "drag-drop: dropped onto a separator; inserted after it at the same depth (#28)", { { "isSeparator", _nodeIsSeparator(node) ? "1" : "0" }, { "depth", std::to_string(node.Depth()) } });
                        continue;
                    }
                    Axan::Log::Warn("TerminalPage", "drag-drop: dropped onto a separator that isn't in the tree; leaving the node where it landed (#28)");
                }
                if (!parent || parent.Depth() < 0)
                {
                    continue;
                }
                const bool wasExpanded = parent.IsExpanded();
                parent.IsExpanded(true);
                if (const auto container = SessionTree().ContainerFromNode(parent).try_as<MUX::Controls::TreeViewItem>())
                {
                    container.GlyphOpacity(1.0);
                }
                Axan::Log::Info("TerminalPage", "drag-drop reparent: expanded new parent", { { "wasExpanded", wasExpanded ? "1" : "0" }, { "childCount", std::to_string(parent.Children().Size()) }, { "isSeparator", _nodeIsSeparator(node) ? "1" : "0" } });
            }
        }
        // axan #33: a row dropped into a collapsed section was put there on purpose; show it.
        for (const auto& dropped : droppedNodes)
        {
            if (!_nodeIsSeparator(dropped))
            {
                _ExpandSectionsContaining(dropped, "dropped into section");
            }
        }
        _SinkBottomSeparators();
        // axan #28: the drag is over; separators go back to their lock state (no drop target
        // outside a drag) on the re-apply below.
        _sessionTreeDragInFlight = false;
        _ApplySeparatorContainerStates();
        _UpdateSeparatorUnlockFromKeyboard("drag completed");
        _VerifySessionTreeFlatList("drag completed");
    }

    // axan #28: the TreeView's inner TreeViewList keeps a flat vector of the visible nodes
    // that is meant to mirror RootNodes (each node, then its children when expanded). A drop
    // that WinUI handles as a list-level reorder instead of a node move can desync the two —
    // the dragged node ends up in the flat list twice and a sibling vanishes from view while
    // still present in RootNodes. Compare the two after every drop; on a mismatch rebuild the
    // view from RootNodes (re-appending a node re-adds its expanded descendants).
    void TerminalPage::_VerifySessionTreeFlatList(const char* reason)
    {
        const auto tree = SessionTree();
        if (!tree || !_sessionTreeList)
        {
            return;
        }
        const auto countVisible = [](const auto& self, const Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>& nodes) -> uint32_t {
            uint32_t n = 0;
            for (const auto& node : nodes)
            {
                ++n;
                if (node.IsExpanded())
                {
                    n += self(self, node.Children());
                }
            }
            return n;
        };
        const auto inspect = [&](uint32_t& modelCount, uint32_t& flatCount, uint32_t& duplicates) {
            modelCount = countVisible(countVisible, tree.RootNodes());
            const auto flat = _sessionTreeList.Items();
            flatCount = flat ? flat.Size() : 0;
            duplicates = 0;
            std::unordered_set<void*> seen;
            seen.reserve(flatCount);
            for (uint32_t i = 0; i < flatCount; ++i)
            {
                const auto item = flat.GetAt(i);
                void* const key = item ? winrt::get_abi(item) : nullptr;
                if (!seen.insert(key).second)
                {
                    ++duplicates;
                }
            }
        };
        uint32_t modelCount = 0, flatCount = 0, duplicates = 0;
        inspect(modelCount, flatCount, duplicates);
        if (modelCount == flatCount && duplicates == 0)
        {
            Axan::Log::Debug("TerminalPage", "session tree flat list matches RootNodes", { { "reason", reason }, { "visible", std::to_string(modelCount) }, { "flat", std::to_string(flatCount) } });
            return;
        }
        Axan::Log::Error("TerminalPage", "session tree flat list desynced from RootNodes; rebuilding the view (#28)", { { "reason", reason }, { "visible", std::to_string(modelCount) }, { "flat", std::to_string(flatCount) }, { "duplicates", std::to_string(duplicates) } });

        std::vector<MUX::Controls::TreeViewNode> roots;
        for (const auto& node : tree.RootNodes())
        {
            roots.push_back(node);
        }
        tree.RootNodes().Clear();
        for (const auto& node : roots)
        {
            tree.RootNodes().Append(node);
        }
        _ApplySeparatorContainerStates();
        if (const auto tab = _GetFocusedTab())
        {
            if (const auto node = _FindNodeForTab(tab))
            {
                tree.SelectedNode(node);
            }
        }
        inspect(modelCount, flatCount, duplicates);
        Axan::Log::Info("TerminalPage", "session tree view rebuilt from RootNodes", { { "visible", std::to_string(modelCount) }, { "flat", std::to_string(flatCount) }, { "duplicates", std::to_string(duplicates) } });
    }

    // ===================== axan #3: separator lock (Ctrl+Alt unlock) =====================
    //
    // Separators are non-interactive by default: their TreeViewItem containers are disabled
    // (not clickable/selectable/focusable, skipped by arrow-key navigation), no tab stop, no
    // drop target (a separator can't own sessions), and no drag source. Holding Ctrl+Alt
    // enables them (and CanDrag) so the native TreeView drag-and-drop can reorder/reparent
    // them; releasing either key re-locks. The state is pushed onto containers in two ways:
    // as each container is prepared (the inner TreeViewList's ContainerContentChanging —
    // WinUI 2's TreeView has no such event of its own) and, on every lock flip, over every
    // already-realized container.

    // The TreeView's inner TreeViewList (template part "ListControl"), found by walking the
    // visual tree — it isn't exposed as a property.
    static MUX::Controls::TreeViewList _findTreeViewList(const WUX::DependencyObject& root)
    {
        if (!root)
        {
            return nullptr;
        }
        if (const auto list = root.try_as<MUX::Controls::TreeViewList>())
        {
            return list;
        }
        const auto count = WUX::Media::VisualTreeHelper::GetChildrenCount(root);
        for (int32_t i = 0; i < count; ++i)
        {
            if (const auto found = _findTreeViewList(WUX::Media::VisualTreeHelper::GetChild(root, i)))
            {
                return found;
            }
        }
        return nullptr;
    }

    // Hook the inner list's ContainerContentChanging once it exists (the tree's Loaded);
    // idempotent, and retried from the realize/apply paths in case Loaded ran before the
    // template was applied.
    void TerminalPage::_HookSessionTreeList()
    {
        if (_sessionTreeList)
        {
            return;
        }
        const auto list = _findTreeViewList(SessionTree());
        if (!list)
        {
            Axan::Log::Debug("TerminalPage", "session TreeViewList not realized yet; separator container hook deferred");
            return;
        }
        _sessionTreeList = list;
        list.ContainerContentChanging({ get_weak(), &TerminalPage::_OnSessionTreeContainerContentChanging });
        Axan::Log::Debug("TerminalPage", "hooked session TreeViewList ContainerContentChanging for separator rows");
        _ApplySeparatorContainerStates();
    }

    // Per-container preparation. In unbound mode the list's item IS the TreeViewNode; fall
    // back to NodeFromContainer in case a WinUI update changes that.
    void TerminalPage::_OnSessionTreeContainerContentChanging(const ListViewBase& /*sender*/,
                                                              const ContainerContentChangingEventArgs& args)
    {
        if (args.InRecycleQueue())
        {
            return;
        }
        const auto container = args.ItemContainer().try_as<MUX::Controls::TreeViewItem>();
        if (!container)
        {
            return;
        }
        auto node = args.Item().try_as<MUX::Controls::TreeViewNode>();
        if (!node)
        {
            node = SessionTree().NodeFromContainer(container);
        }
        if (!node)
        {
            return;
        }
        const auto vm = _nodeVM(node); // axan #33: header + section-hidden state too
        _ApplySeparatorContainerState(container, _nodeIsSeparator(node), vm && vm->HasSectionHeader(), vm && vm->IsSectionHidden());
    }

    // Push the lock state onto one container. A session container only has its flags
    // RESTORED (a recycled container may have last hosted a locked separator) — nothing a
    // session already has is disturbed.
    void TerminalPage::_ApplySeparatorContainerState(const MUX::Controls::TreeViewItem& container, bool isSeparator, bool isSectionHeader, bool sectionHidden)
    {
        if (!container)
        {
            return;
        }
        // axan #33: a row hidden by a collapsed section keeps its place in the tree (and in
        // TreeViewList's flat list, so the #28 check is unaffected); only its container
        // collapses to zero size.
        const auto wantVisibility = sectionHidden ? Visibility::Collapsed : Visibility::Visible;
        if (container.Visibility() != wantVisibility)
        {
            container.Visibility(wantVisibility);
        }
        if (isSeparator)
        {
            // axan #28: during a drag every separator is an enabled drop target, locked or not.
            // A disabled / AllowDrop(false) container refuses the node-level drop and the drop
            // falls through to TreeViewList's flat-list reorder, which desyncs from RootNodes
            // (duplicated row, vanished separator). DragItemsCompleted turns a drop onto a
            // separator into "insert after it". Outside a drag the lock is unchanged.
            const bool unlocked = _separatorsUnlocked;
            const bool dragInFlight = _sessionTreeDragInFlight;
            // axan #33: a section header stays enabled so a click can toggle it (ItemInvoked)
            // and its badge button works; it still can't be dragged unless unlocked, and it
            // stays out of the tab order like every separator.
            container.IsEnabled(unlocked || dragInFlight || isSectionHeader);
            container.IsTabStop(false);
            container.AllowDrop(dragInFlight);
            container.CanDrag(unlocked);
            return;
        }
        if (!container.IsEnabled())
        {
            container.IsEnabled(true);
        }
        if (!container.IsTabStop())
        {
            container.IsTabStop(true);
        }
        if (!container.AllowDrop() && SessionTree().AllowDrop())
        {
            container.AllowDrop(true);
        }
        if (!container.CanDrag() && SessionTree().CanDragItems())
        {
            container.CanDrag(true);
        }
    }

    // Re-apply the lock state to every realized container (ContainerFromNode is null for
    // unrealized rows; those get theirs from ContainerContentChanging when they appear).
    void TerminalPage::_ApplySeparatorContainerStates()
    {
        _HookSessionTreeList();
        if (!SessionTree())
        {
            return;
        }
        size_t separators = 0;
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            const bool isSeparator = _nodeIsSeparator(node);
            if (isSeparator)
            {
                ++separators;
            }
            if (const auto container = SessionTree().ContainerFromNode(node).try_as<MUX::Controls::TreeViewItem>())
            {
                const auto vm = _nodeVM(node);
                _ApplySeparatorContainerState(container, isSeparator, vm && vm->HasSectionHeader(), vm && vm->IsSectionHidden());
            }
        });
        if (separators > 0)
        {
            Axan::Log::Debug("TerminalPage", "applied separator container states", { { "separators", std::to_string(separators) }, { "unlocked", _separatorsUnlocked ? "1" : "0" } });
        }
    }

    // Flip the lock; no-op when unchanged.
    void TerminalPage::_SetSeparatorsUnlocked(bool unlocked, const char* reason)
    {
        if (_separatorsUnlocked == unlocked)
        {
            return;
        }
        _separatorsUnlocked = unlocked;
        Axan::Log::Debug("TerminalPage", unlocked ? "separators unlocked (Ctrl+Alt held)" : "separators locked", { { "reason", reason } });
        _ApplySeparatorContainerStates();
        if (!unlocked)
        {
            // A click on an unlocked separator made it the tree's (single) selection; once the
            // row is disabled again nothing else would ever move the highlight off it, and both
            // the keyboard context menu and focusSidebar resolve SelectedNode first.
            _ResetSeparatorSelection("separators locked");
        }
    }

    // axan #3: if the tree's selection sits on a separator, move it to the focused session's
    // node (or clear it). Only an unlocked separator can be selected, so this runs when the
    // lock re-engages and when a separator is invoked.
    void TerminalPage::_ResetSeparatorSelection(const char* reason)
    {
        const auto tree = SessionTree();
        if (!tree)
        {
            return;
        }
        const auto selected = tree.SelectedNode();
        if (!selected || !_nodeIsSeparator(selected))
        {
            return;
        }
        MUX::Controls::TreeViewNode target{ nullptr };
        if (const auto tab = _GetFocusedTab())
        {
            target = _FindNodeForTab(tab);
        }
        tree.SelectedNode(target);
        Axan::Log::Debug("TerminalPage", "moved the tree selection off a separator row", { { "reason", reason }, { "target", target ? "focused session" : "none" } });
    }

    // Derive the lock from the live modifier state (Win32 GetKeyState — evaluated inside a
    // key message it reflects that message, so a Ctrl/Alt key-up reads as up). Frozen while
    // a drag is in flight so a release mid-drag can't disable the container being dragged.
    // AltGr on international layouts arrives as LeftCtrl+RightAlt (ControlKeyStates::
    // IsAltGrPressed models it the same way, and the keybinding path excludes it per GH#2235):
    // typing '@' or '{' must not unlock the dividers, so that exact shape stays locked.
    void TerminalPage::_UpdateSeparatorUnlockFromKeyboard(const char* reason)
    {
        if (_separatorDragActive)
        {
            return;
        }
        const auto down = [](int vk) { return (::GetKeyState(vk) & 0x8000) != 0; };
        const bool lctrl = down(VK_LCONTROL);
        const bool rctrl = down(VK_RCONTROL);
        const bool lalt = down(VK_LMENU);
        const bool ralt = down(VK_RMENU);
        const bool altGr = lctrl && ralt && !rctrl && !lalt;
        const bool unlocked = (lctrl || rctrl) && (lalt || ralt) && !altGr;
        _SetSeparatorsUnlocked(unlocked, reason);
    }

    // PreviewKeyDown/PreviewKeyUp on the page root (tunneling — runs before the focused
    // terminal control handles the key). Re-derives the lock on EVERY key event, not only on
    // modifier keys: a Ctrl/Alt release delivered while a flyout or dialog owns focus never
    // tunnels through this root (popups live under the PopupRoot, outside it), so the next
    // keystroke anywhere in the page is the first chance to notice it. Two GetKeyState reads
    // per key; _SetSeparatorsUnlocked no-ops when nothing changed. Never marks the event handled.
    void TerminalPage::_OnRootPreviewKey(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                         const WUX::Input::KeyRoutedEventArgs& /*args*/)
    {
        _UpdateSeparatorUnlockFromKeyboard("key event");
    }

    // Belt and braces for the lock: a drag that includes a separator while locked is
    // cancelled outright (the container should be disabled and CanDrag false already).
    // Otherwise mark the drag active so the lock state holds until DragItemsCompleted.
    void TerminalPage::_OnSessionTreeDragItemsStarting(const MUX::Controls::TreeView& /*sender*/,
                                                       const MUX::Controls::TreeViewDragItemsStartingEventArgs& args)
    {
        bool draggingSeparator = false;
        if (const auto items = args.Items())
        {
            for (const auto& item : items)
            {
                if (const auto node = item.try_as<MUX::Controls::TreeViewNode>(); node && _nodeIsSeparator(node))
                {
                    draggingSeparator = true;
                    break;
                }
            }
        }
        if (draggingSeparator && !_separatorsUnlocked)
        {
            args.Cancel(true);
            Axan::Log::Warn("TerminalPage", "drag of a locked separator reached DragItemsStarting; cancelled (#3)");
            return;
        }
        _separatorDragActive = true;
        _sessionTreeDragInFlight = true;
        Axan::Log::Debug("TerminalPage", "session tree drag starting", { { "draggingSeparator", draggingSeparator ? "1" : "0" }, { "unlocked", _separatorsUnlocked ? "1" : "0" } });
        // axan #28: make the realized separator rows drop targets for this drag (rows realized
        // later pick it up from ContainerContentChanging).
        _ApplySeparatorContainerStates();
    }

    // ===================== axan M13: session-node context menu =====================
    //
    // Right-click a session-tree node for its actions. The menu is built in code (not XAML)
    // and shown at the right-clicked TreeViewItem: this sidesteps the XAML-compiler WinUI-2
    // type-set gaps M7/M8 hit (IconSourceElement, RadioMenuFlyoutItem), and lets each
    // MenuFlyoutItem capture the target node directly. Every action reuses the M6/M7 node
    // plumbing (_nodeVM/_nodeTab, _DuplicateTab, _RemoveTab self-heal, _resolveNodeIconPath,
    // _LaunchSettings) rather than introducing a new model.

    // The TreeViewNode under a right-tap: walk up the visual tree from the event's
    // OriginalSource to its TreeViewItem, then map the container back to its node. Returns
    // nullptr if the tap missed a node (e.g. empty sidebar space below the last row).
    MUX::Controls::TreeViewNode TerminalPage::_NodeUnderPointer(const winrt::Windows::Foundation::IInspectable& originalSource)
    {
        auto current = originalSource.try_as<WUX::DependencyObject>();
        while (current)
        {
            if (const auto item = current.try_as<MUX::Controls::TreeViewItem>())
            {
                return SessionTree().NodeFromContainer(item);
            }
            current = WUX::Media::VisualTreeHelper::GetParent(current);
        }
        return nullptr;
    }

    void TerminalPage::_OnSessionTreeRightTapped(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                 const WUX::Input::RightTappedRoutedEventArgs& args)
    {
        // axan #446: both bail branches log. During the 2026-07-02 startup-freeze report,
        // right-clicks left no trace at all — indistinguishable between "input never
        // dispatched" and "handler bailed here". Now a right-click that reaches us always
        // writes a line, so absence of any line means the input never arrived (blocked UI
        // thread), not a silent bail.
        const auto node = _NodeUnderPointer(args.OriginalSource());
        if (!node)
        {
            // axan #12: the empty sidebar space below the last row is still a valid
            // "create a session here" surface — offer the New session split row.
            Axan::Log::Debug("TerminalPage", "sidebar right-tap: no node under pointer; showing background menu");
            _ShowSidebarBackgroundContextMenu(args.GetPosition(SessionTree()));
            args.Handled(true);
            return;
        }
        if (const auto vm = _nodeVM(node); vm && vm->HasSectionHeader())
        {
            // axan #33: a section header offers Collapse/Expand ahead of the background items.
            _ShowSectionHeaderMenu(node, args.GetPosition(SessionTree()));
            args.Handled(true);
            return;
        }
        if (_nodeIsSeparator(node))
        {
            // axan #3: a separator has no session actions (it's edited on the Startup
            // sessions page); treat the tap like empty sidebar space.
            Axan::Log::Debug("TerminalPage", "sidebar right-tap: separator row; showing background menu");
            _ShowSidebarBackgroundContextMenu(args.GetPosition(SessionTree()));
            args.Handled(true);
            return;
        }
        const auto item = SessionTree().ContainerFromNode(node).try_as<MUX::Controls::TreeViewItem>();
        if (!item)
        {
            Axan::Log::Warn("TerminalPage", "sidebar right-tap: node found but its container is not realized; menu suppressed (#446)");
            return;
        }
        _ShowSessionNodeContextMenu(node, item, std::nullopt);
        args.Handled(true);
    }

    // axan #429: the keyboard route to the same per-node menu. Shift+F10 / the menu key (and
    // some assistive tech) raise ContextRequested — NOT RightTapped — so the menu was
    // mouse-only. Resolve the target node from the focused element (falling back to the
    // tree's selected node when focus sits on the tree itself), and place the flyout at the
    // reported position when one exists (pointer-driven invocations), else anchored to the
    // node's row (keyboard invocations carry no position — TryGetPosition returns false).
    // A mouse right-click can raise BOTH RightTapped and ContextRequested; the IsOpen guard
    // in _ShowSessionNodeContextMenu makes the second arrival a no-op.
    void TerminalPage::_OnSessionTreeContextRequested(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                      const WUX::Input::ContextRequestedEventArgs& args)
    {
        auto node = _NodeUnderPointer(args.OriginalSource());
        if (!node)
        {
            node = SessionTree().SelectedNode();
        }
        if (!node)
        {
            Axan::Log::Debug("TerminalPage", "sidebar context-requested: no node under pointer and no selection");
            return;
        }
        if (_nodeIsSeparator(node))
        {
            Axan::Log::Debug("TerminalPage", "sidebar context-requested: separator row; no menu (#3)");
            args.Handled(true);
            return;
        }
        const auto item = SessionTree().ContainerFromNode(node).try_as<MUX::Controls::TreeViewItem>();
        if (!item)
        {
            Axan::Log::Warn("TerminalPage", "sidebar context-requested: node found but its container is not realized; menu suppressed (#446)");
            return;
        }
        winrt::Windows::Foundation::Point point{};
        if (args.TryGetPosition(item, point))
        {
            _ShowSessionNodeContextMenu(node, item, point);
        }
        else
        {
            _ShowSessionNodeContextMenu(node, item, std::nullopt);
        }
        args.Handled(true);
    }

    // axan M13/#429: build and show the per-node menu. Shared by the mouse (RightTapped) and
    // keyboard (ContextRequested) paths above; `position` is item-relative when the invoking
    // gesture reported one, else the flyout anchors to the row. The menu is rebuilt per show
    // (each item captures the target node); _sessionNodeMenu only exists so a gesture that
    // raises both events can't open it twice.
    void TerminalPage::_ShowSessionNodeContextMenu(const MUX::Controls::TreeViewNode& node,
                                                   const MUX::Controls::TreeViewItem& item,
                                                   const std::optional<winrt::Windows::Foundation::Point>& position)
    {
        if (_sessionNodeMenu && _sessionNodeMenu.IsOpen())
        {
            return;
        }

        // A small helper to build a MenuFlyoutItem with a Segoe MDL2 glyph icon.
        const auto makeItem = [](const winrt::hstring& text, const wchar_t* glyph) {
            MenuFlyoutItem mfi{};
            mfi.Text(text);
            FontIcon fi{};
            fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
            fi.Glyph(winrt::hstring{ glyph });
            mfi.Icon(fi);
            return mfi;
        };

        MenuFlyout flyout{};

        {
            auto mi = makeItem(RS_(L"AxanMenuEditNode"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_ShowNodeEditPanel(node); });
            flyout.Items().Append(mi);
        }
        {
            auto mi = makeItem(RS_(L"AxanMenuClearIconOverride"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_ApplyNodeIcon(node, {}, {}, {}); });
            flyout.Items().Append(mi);
        }
        flyout.Items().Append(MenuFlyoutSeparator{});
        // axan #12: root-level session creation from the sidebar — the New session split
        // row (activate = default profile, submenu = pick a type), above the node-scoped
        // Add child / Duplicate.
        _AppendNewSessionSplitItem(flyout, L"Segoe MDL2 Assets");
        {
            auto mi = makeItem(RS_(L"AxanMenuAddChildSession"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_AddChildSession(node); });
            flyout.Items().Append(mi);
        }
        {
            auto mi = makeItem(RS_(L"AxanMenuDuplicateSession"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_DuplicateNodeSession(node); });
            flyout.Items().Append(mi);
        }
        flyout.Items().Append(MenuFlyoutSeparator{});
        // axan: plain Settings (the profile is WT-owned and SettingsTarget cannot deep-link
        // to it), the same item as the empty-area menu.
        flyout.Items().Append(_MakeSettingsMenuItem());
        {
            auto mi = makeItem(RS_(L"AxanMenuCloseSession"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_CloseNodeSession(node); });
            flyout.Items().Append(mi);
        }
        // axan: debugging / preview aid, last so it stays out of the way.
        flyout.Items().Append(MenuFlyoutSeparator{});
        _AppendStatePreviewSubMenu(flyout, node);

        _sessionNodeMenu = flyout;
        // axan #3: a Ctrl/Alt release while the menu has focus never reaches Root()'s key
        // handlers (the flyout lives under the PopupRoot); re-derive the separator lock as it closes.
        flyout.Closed([weak = get_weak()](auto&&, auto&&) {
            if (auto p{ weak.get() })
            {
                p->_UpdateSeparatorUnlockFromKeyboard("context menu closed");
            }
        });
        if (position)
        {
            flyout.ShowAt(item, *position);
        }
        else
        {
            flyout.ShowAt(item);
        }
        Axan::Log::Debug("TerminalPage", "showed session-node context menu");
    }

    // axan: the sidebar menus' Settings item, the same entry point as the titlebar app menu's
    // Settings item (_SettingsButtonOnClick honours Alt/Shift the same way there), with its
    // key binding shown. Used by the empty-area menu and the session node menu.
    MenuFlyoutItem TerminalPage::_MakeSettingsMenuItem()
    {
        MenuFlyoutItem settingsItem{};
        settingsItem.Text(RS_(L"SettingsMenuItem"));
        FontIcon fi{};
        fi.FontFamily(WUX::Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        fi.Glyph(winrt::hstring{ L"\xE713" });
        settingsItem.Icon(fi);
        settingsItem.Click({ this, &TerminalPage::_SettingsButtonOnClick });
        if (_settings)
        {
            if (const auto keyChord{ _settings.ActionMap().GetKeyBindingForAction(L"Terminal.OpenSettingsUI") })
            {
                _SetAcceleratorForMenuItem(settingsItem, keyChord);
            }
        }
        return settingsItem;
    }

    // axan #12: the menu for a right-tap on empty sidebar space (below the last row) —
    // no target node, so just the New session split row, anchored at the pointer.
    // Reuses _sessionNodeMenu's double-open guard: a gesture that raises both RightTapped
    // and ContextRequested can't stack two menus.
    void TerminalPage::_ShowSidebarBackgroundContextMenu(const winrt::Windows::Foundation::Point& position)
    {
        if (_sessionNodeMenu && _sessionNodeMenu.IsOpen())
        {
            return;
        }
        MenuFlyout flyout{};
        _AppendNewSessionSplitItem(flyout, L"Segoe MDL2 Assets");
        _AppendNeedsYouGroupToggle(flyout); // axan #34

        // axan: Settings at the bottom (see _MakeSettingsMenuItem).
        flyout.Items().Append(MenuFlyoutSeparator{});
        flyout.Items().Append(_MakeSettingsMenuItem());

        _sessionNodeMenu = flyout;
        flyout.Closed([weak = get_weak()](auto&&, auto&&) {
            if (auto p{ weak.get() })
            {
                p->_UpdateSeparatorUnlockFromKeyboard("background menu closed");
            }
        });
        flyout.ShowAt(SessionTree(), position);
        Axan::Log::Debug("TerminalPage", "showed sidebar background context menu");
    }

    // axan #12: append the "New session" split row to a menu — one row that both creates
    // and picks. Activating the row itself opens a session of the DEFAULT profile — via
    // _OpenNewTab(nullptr), NOT a dispatched ActionAndArgs{NewTab, nullptr}, which
    // _HandleNewTab silently no-ops on (the keybinding works because defaults.json
    // materializes real NewTabArgs). Hovering (or keyboard-expanding) the row opens a
    // submenu of the active profiles, one entry per type. A MenuFlyoutSubItem exposes no
    // Click, so the default-profile activation rides a Tapped handler registered with
    // handledEventsToo; the submenu children live in their own popup, so their clicks
    // don't bubble here. The titlebar app menu and both sidebar menus (node + background)
    // append through this helper so they can't drift; iconFontFamily matches the caller's
    // other items.
    void TerminalPage::_AppendNewSessionSplitItem(const MenuFlyout& flyout, const wchar_t* iconFontFamily)
    {
        MenuFlyoutSubItem sub{};
        sub.Text(RS_(L"AxanMenuNewSession"));
        FontIcon fi{};
        fi.FontFamily(WUX::Media::FontFamily{ iconFontFamily });
        fi.Glyph(L""); // Add — same glyph the plain "New session" item carried
        sub.Icon(fi);

        sub.AddHandler(WUX::UIElement::TappedEvent(),
                       winrt::box_value(WUX::Input::TappedEventHandler{ [weakThis{ get_weak() }, weakFlyout{ winrt::make_weak(flyout) }](auto&&, auto&&) {
                           if (const auto f = weakFlyout.get())
                           {
                               f.Hide();
                           }
                           if (auto page{ weakThis.get() })
                           {
                               LOG_IF_FAILED(page->_OpenNewTab(nullptr));
                           }
                       } }),
                       true /* handledEventsToo */);

        const auto activeProfiles = _settings.ActiveProfiles();
        const auto defaultProfileGuid = _settings.GlobalSettings().DefaultProfile();
        const auto profileCount = gsl::narrow_cast<int32_t>(activeProfiles.Size());
        for (int32_t index = 0; index < profileCount; ++index)
        {
            const auto profile = activeProfiles.GetAt(static_cast<uint32_t>(index));
            MenuFlyoutItem mi{};
            mi.Text(profile.Name());
            // Mirror the upstream new-tab flyout (_CreateNewTabFlyoutProfile): the
            // profile's own icon, and the default profile contrasted in bold.
            if (const auto iconPath = profile.Icon().Resolved(); !iconPath.empty())
            {
                mi.Icon(_CreateNewTabFlyoutIcon(iconPath));
            }
            if (profile.Guid() == defaultProfileGuid)
            {
                mi.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
            }
            mi.Click([weakThis{ get_weak() }, index](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    LOG_IF_FAILED(page->_OpenNewTab(NewTerminalArgs{ index }));
                }
            });
            sub.Items().Append(mi);
        }
        flyout.Items().Append(sub);
    }

    // axan: build the titlebar app-menu flyout and attach it to the "axan" button in
    // the TabRowControl (the otherwise-empty upper-left of the titlebar). The menu
    // mirrors the per-node context menu above, but targets the ACTIVE session — every
    // session item resolves _FindNodeForTab(_GetFocusedTab()) at click time instead of
    // capturing a node — and appends the app-level entries: new session, toggle
    // sidebar, and the settings / command palette / about tail shared with the
    // new-tab flyout (_AppendCommonMenuItems). Rebuilt whole on settings reload
    // because the accelerator hints read the (possibly rebound) action map.
    void TerminalPage::_CreateAppMenuFlyout()
    {
        const auto makeItem = [](const winrt::hstring& text, const wchar_t* glyph) {
            MenuFlyoutItem mfi{};
            mfi.Text(text);
            FontIcon fi{};
            fi.FontFamily(WUX::Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            fi.Glyph(winrt::hstring{ glyph });
            mfi.Icon(fi);
            return mfi;
        };

        MenuFlyout flyout{};
        flyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        _appMenuSessionItems.clear();

        const auto actionMap = _settings.ActionMap();

        // axan #12: the New session split row — activating it opens a default-profile
        // session, its submenu picks a type. (The old plain item's Ctrl+Shift+T hint is
        // gone with it: a MenuFlyoutSubItem has no accelerator-text slot.)
        _AppendNewSessionSplitItem(flyout, L"Segoe Fluent Icons, Segoe MDL2 Assets");

        flyout.Items().Append(MenuFlyoutSeparator{});

        // Header naming the session the items below will act on — this menu has no
        // right-clicked row to make the target obvious, so spell it out. Disabled =
        // non-interactive label (same pattern as the sidebar-size flyout's header);
        // the text is filled in by _RefreshAppMenuSessionItems on every open.
        {
            MenuFlyoutItem header{};
            header.IsEnabled(false);
            _appMenuSessionHeader = header;
            flyout.Items().Append(header);
        }

        // The active-session items. addSessionItem covers the helpers that take just
        // the node; each built item is also remembered in _appMenuSessionItems so
        // _RefreshAppMenuSessionItems can disable the lot when nothing is focused.
        const auto addSessionItem = [&](const winrt::hstring& text, const wchar_t* glyph, void (TerminalPage::*handler)(const MUX::Controls::TreeViewNode&)) {
            auto mi = makeItem(text, glyph);
            mi.Click([weakThis{ get_weak() }, handler](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    if (const auto tab = page->_GetFocusedTab())
                    {
                        if (const auto node = page->_FindNodeForTab(tab))
                        {
                            (page.get()->*handler)(node);
                        }
                    }
                }
            });
            flyout.Items().Append(mi);
            _appMenuSessionItems.push_back(mi);
        };

        addSessionItem(RS_(L"AxanMenuEditNode"), L"", &TerminalPage::_ShowNodeEditPanel);
        {
            // Clear icon override takes extra (empty) args — inline instead of addSessionItem.
            auto mi = makeItem(RS_(L"AxanMenuClearIconOverride"), L"");
            mi.Click([weakThis{ get_weak() }](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    if (const auto tab = page->_GetFocusedTab())
                    {
                        if (const auto node = page->_FindNodeForTab(tab))
                        {
                            page->_ApplyNodeIcon(node, {}, {}, {});
                        }
                    }
                }
            });
            flyout.Items().Append(mi);
            _appMenuSessionItems.push_back(mi);
        }
        flyout.Items().Append(MenuFlyoutSeparator{});
        addSessionItem(RS_(L"AxanMenuAddChildSession"), L"", &TerminalPage::_AddChildSession);
        addSessionItem(RS_(L"AxanMenuDuplicateSession"), L"", &TerminalPage::_DuplicateNodeSession);
        flyout.Items().Append(MenuFlyoutSeparator{});
        addSessionItem(RS_(L"AxanMenuCloseSession"), L"", &TerminalPage::_CloseNodeSession);

        flyout.Items().Append(MenuFlyoutSeparator{});

        // Toggle sidebar — same _CycleSidebarState the header chevron and the
        // Ctrl+Shift+B action use. E8A0 is OpenPane.
        {
            auto mi = makeItem(RS_(L"AxanMenuToggleSidebar"), L"");
            mi.Click([weakThis{ get_weak() }](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    page->_CycleSidebarState();
                }
            });
            if (const auto keyChord{ actionMap.GetKeyBindingForAction(L"Terminal.ToggleSidebar") })
            {
                _SetAcceleratorForMenuItem(mi, keyChord);
            }
            flyout.Items().Append(mi);
        }

        flyout.Items().Append(MenuFlyoutSeparator{});

        // Settings / command palette / about — the exact items the new-tab flyout shows.
        _AppendCommonMenuItems(flyout);

        // Focus handling mirrors _CreateNewTabFlyout: focus the current tab before the
        // menu opens so dismissal always lands focus somewhere sane, and again on close
        // unless the command palette took over.
        flyout.Opening([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                page->_RefreshAppMenuSessionItems();
                page->_FocusCurrentTab(true);
            }
        });
        flyout.Closing([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                if (!page->_commandPaletteIs(Visibility::Visible))
                {
                    page->_FocusCurrentTab(true);
                }
            }
        });

        _appMenuFlyout = flyout;
        const auto tabRowImpl = winrt::get_self<implementation::TabRowControl>(_tabRow);
        tabRowImpl->AxanMenuButton().Flyout(flyout);
        Axan::Log::Debug("TerminalPage", "built titlebar app-menu flyout");
    }

    // axan: enable/disable the session-scoped app-menu items to match whether a live
    // session is focused right now. Runs on every menu open. Disable — not hide — so
    // the menu keeps a stable shape; the click handlers also re-resolve and null-check
    // as defense in depth.
    void TerminalPage::_RefreshAppMenuSessionItems()
    {
        bool haveSession = false;
        winrt::hstring label;
        if (const auto tab = _GetFocusedTab())
        {
            if (const auto node = _FindNodeForTab(tab))
            {
                haveSession = true;
                if (const auto vm = _nodeVM(node))
                {
                    label = vm->Label();
                }
                if (label.empty())
                {
                    label = tab.Title();
                }
            }
        }
        if (_appMenuSessionHeader)
        {
            _appMenuSessionHeader.Text(haveSession ? RS_(L"AxanMenuCurrentSession") + L" " + label :
                                                     RS_(L"AxanMenuNoActiveSession"));
        }
        for (const auto& item : _appMenuSessionItems)
        {
            item.IsEnabled(haveSession);
        }
    }

    // axan: the two halves of the titlebar's centered title. Primary is the focused
    // session's sidebar label — the same text its tree row shows — falling back to
    // Title() when there's no node (settings tab, startup). Secondary is the terminal
    // title, only when the user shows titles in the titlebar and it adds anything
    // beyond the label. AppHost pulls both on every TitleChanged.
    hstring TerminalPage::TitlebarPrimaryText()
    {
        if (const auto tab{ _GetFocusedTab() })
        {
            if (const auto node = _FindNodeForTab(tab))
            {
                if (const auto vm = _nodeVM(node))
                {
                    if (const auto label = vm->Label(); !label.empty())
                    {
                        return label;
                    }
                }
            }
        }
        return Title();
    }

    hstring TerminalPage::TitlebarSecondaryText()
    {
        if (_settings && _settings.GlobalSettings().ShowTitleInTitlebar())
        {
            if (const auto tab{ _GetFocusedTab() })
            {
                const auto title = tab.Title();
                if (!title.empty() && title != TitlebarPrimaryText())
                {
                    return title;
                }
            }
        }
        return {};
    }

    // axan M13: apply (or clear) a node's icon override + recolor and refresh the row. Empty
    // override -> the node falls back to its profile icon, else an auto-assigned glyph (the M7
    // chain); empty color -> the glyph paints with the theme foreground. The change is held on
    // the view-model (IconOverride / IconColor) so the next "Save sessions as startup" persists
    // it (the settings.json startup tree, mirrored to startup-sessions.toml) and it returns
    // on relaunch — the M13 Done gate.
    void TerminalPage::_ApplyNodeIcon(const MUX::Controls::TreeViewNode& node, const winrt::hstring& iconOverride, const winrt::hstring& iconColor, const winrt::hstring& colorTarget)
    {
        const auto vm = _nodeVM(node);
        if (!vm || vm->IsSeparator()) // axan #3: a separator has no icon/label to restyle
        {
            return;
        }
        vm->IconOverride = iconOverride;
        const auto tab = _nodeTab(node);
        vm->ApplyAppearance(_resolveNodeIconPath(iconOverride, tab), iconColor, colorTarget);

        Axan::Log::Info("TerminalPage", "applied node appearance", { { "override", winrt::to_string(iconOverride) }, { "color", winrt::to_string(iconColor) }, { "target", winrt::to_string(colorTarget) } });

        // Keep the minimized icon list in step if it's the surface currently showing.
        if (_sidebarState == SidebarState::Minimized)
        {
            _RebuildMinimizedItems();
        }
    }

    // axan #422/#427: write a node's current label/icon/color/target back to the curated startup
    // entry it spawned from (matched by EntryId) and flush settings.json, so a sidebar edit
    // survives a restart. Post-D19 the curated tree lives on GlobalSettings().StartupSessions()
    // — the same global vector the Settings editor's StartupSessionsViewModel commits to, so the
    // two write paths can't diverge again (#427 item 1: this previously targeted the retired
    // per-profile store, which the migration empties, so every Save silently missed). A node
    // with no EntryId — a runtime-created session, not part of the curated set — is a no-op
    // (the edit stays live-only).
    void TerminalPage::_PersistNodeToEntry(const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        if (!vm || vm->EntryId.empty() || !_settings)
        {
            return;
        }
        if (vm->IsSeparator())
        {
            // axan #3: separators are edited only on the Startup sessions page; the live tree
            // never writes one back (a capture re-emits it whole instead).
            return;
        }
        const auto entries = _settings.GlobalSettings().StartupSessions();
        if (!entries)
        {
            Axan::Log::Warn("TerminalPage", "node edit: no global StartupSessions vector; applied live but not persisted", { { "entryId", winrt::to_string(vm->EntryId) } });
            return;
        }

        const auto entryId = vm->EntryId;
        bool found = false;
        std::vector<LaunchEntry> updated;
        updated.reserve(entries.Size());
        for (const auto& e : entries)
        {
            if (e.Id() == entryId)
            {
                e.Name(vm->LabelTemplate);
                e.Icon(vm->IconOverride);
                e.Color(vm->IconColor());
                e.ColorTarget(vm->ColorTarget);
                e.Hoist(vm->HoistOverride); // axan #34
                found = true;
            }
            updated.push_back(e);
        }
        if (!found)
        {
            Axan::Log::Warn("TerminalPage", "node edit: source entry not in global StartupSessions; applied live but not persisted", { { "entryId", winrt::to_string(entryId) } });
            return;
        }

        // Reassign a fresh vector so the GlobalAppSettings setter marks its layer dirty (mirrors
        // the Startup editor's StartupSessionsViewModel::_commit), then flush to settings.json.
        _settings.GlobalSettings().StartupSessions(winrt::single_threaded_vector(std::move(updated)));
        if (!_settings.WriteSettingsToDisk())
        {
            Axan::Log::Error("TerminalPage", "node edit: WriteSettingsToDisk failed; applied live but not persisted", { { "entryId", winrt::to_string(entryId) } });
            return;
        }
        Axan::Log::Info("TerminalPage", "persisted node edit to global startup entry", { { "entryId", winrt::to_string(entryId) } });
    }

    // axan #14: "Save current as startup" — snapshot the live session tree as launch
    // entries (the Linux "Capture current window" parity; the M4b-era sidebar save button
    // retired in D19, reintroduced as a settings-page action). This is the app-side half:
    // the settings editor edits a settings CLONE and can't see live sessions, so its
    // Startup sessions page pulls the snapshot through this provider (registered on the
    // settings UI in _makeSettingsContent) and commits it through its own list, where the
    // user confirmed the replace. Captured per node: hierarchy (pre-order, so ParentId
    // keeps the forward-reference-only invariant LoadStartupTree expects), the focused
    // pane's profile, the live cwd, and the label/icon/color overrides off the node VM.
    // The command a session was originally launched with is NOT recoverable from a live
    // session, so captured entries carry no Command — the accepted #14 limitation (matches
    // Linux; the page's confirm prompt warns about it). Every captured node is adopted
    // into the curated set (EntryId assigned), so a later node edit persists to its
    // captured entry via _PersistNodeToEntry once the settings save lands.
    IVector<LaunchEntry> TerminalPage::_CaptureLiveSessionEntries()
    {
        std::vector<LaunchEntry> captured;
        // Reuse a node's existing EntryId when it has one (stable identity across repeated
        // captures); collisions or runtime-only nodes get a fresh GUID.
        std::unordered_set<winrt::hstring> usedIds;
        const std::function<void(const MUX::Controls::TreeViewNode&, const winrt::hstring&)> visit =
            [&](const MUX::Controls::TreeViewNode& node, const winrt::hstring& parentId) {
                const auto vm = _nodeVM(node);
                if (vm && vm->IsSeparator())
                {
                    // axan #3: a separator captures as a Kind="separator" entry in tree order,
                    // under the nearest captured ancestor, carrying its style/height/placement
                    // off the VM. It never owns sessions, so any children it somehow holds hang
                    // from ITS parent instead.
                    auto id = vm->EntryId;
                    if (id.empty() || usedIds.count(id) > 0)
                    {
                        id = winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) };
                    }
                    usedIds.insert(id);
                    LaunchEntry entry{};
                    entry.Id(id);
                    entry.ParentId(parentId);
                    entry.Kind(winrt::hstring{ Axan::LaunchEntryWire::KindSeparatorW });
                    entry.SeparatorStyle(vm->SeparatorStyle());
                    entry.Height(vm->SeparatorHeight());
                    entry.Placement(vm->Placement());
                    entry.Name(vm->SeparatorLabel()); // axan #33: header label + live collapsed state
                    entry.Collapsed(vm->IsCollapsed());
                    captured.push_back(entry);
                    vm->EntryId = id;
                    for (const auto& child : node.Children())
                    {
                        visit(child, parentId);
                    }
                    return;
                }
                const auto tab = _nodeTab(node);
                const auto tabImpl = tab ? winrt::get_self<Tab>(tab) : nullptr;
                const auto profile = tabImpl ? tabImpl->GetFocusedProfile() : Profile{ nullptr };
                if (!vm || !profile)
                {
                    // Not a capturable session — a mid-prune row, or a profile-less tab
                    // like the Settings tab (a live tab with a sidebar node, but nothing
                    // meaningful to relaunch — and the Settings tab is ALWAYS open when
                    // this runs, since the capture button lives on a settings page). Any
                    // children hang from the nearest captured ancestor instead.
                    for (const auto& child : node.Children())
                    {
                        visit(child, parentId);
                    }
                    return;
                }
                auto id = vm->EntryId;
                if (id.empty() || usedIds.count(id) > 0)
                {
                    id = winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) };
                }
                usedIds.insert(id);

                LaunchEntry entry{};
                entry.Id(id);
                entry.ParentId(parentId);
                entry.Profile(winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(profile.Guid()) });
                if (const auto control = tabImpl->GetActiveTerminalControl())
                {
                    // Empty when the shell never reported OSC 9;9 — the entry then opens
                    // in the launch cwd, same as a hand-authored entry with no directory.
                    entry.Directory(control.CurrentWorkingDirectory());
                }
                entry.Name(vm->LabelTemplate);
                entry.Icon(vm->IconOverride);
                entry.Color(vm->IconColor());
                entry.ColorTarget(vm->ColorTarget);
                entry.Hoist(vm->HoistOverride); // axan #34
                captured.push_back(entry);
                vm->EntryId = id;
                for (const auto& child : node.Children())
                {
                    visit(child, id);
                }
            };
        for (const auto& root : SessionTree().RootNodes())
        {
            visit(root, winrt::hstring{});
        }

        Axan::Log::Info("TerminalPage", "captured live session tree as launch entries", { { "entryCount", std::to_string(captured.size()) } });
        return winrt::single_threaded_vector(std::move(captured));
    }

    // ===================== axan M13: the "Edit session node" editor =====================
    //
    // The editor card is built programmatically (the XAML hosts only the NodeEditOverlay
    // scrim + empty NodeEditCard border). #436 split the original ~420-line builder into
    // the per-section helpers below; the file-statics need no page state, the members read
    // and wire the _nodeEdit* fields. #38 moved the identity sections (name + "?" help,
    // icon grid + Browse, swatches, Apply color to) into the shared
    // AxanEntryIdentityEditor.h, which the Startup sessions settings page hosts too.

    // A bold section caption ("Hoisted group").
    static TextBlock _buildNodeEditSectionLabel(const wchar_t* text)
    {
        TextBlock t{};
        t.Text(text);
        t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        t.Margin(WUX::Thickness{ 0, 10, 0, 4 });
        return t;
    }

    // The editor's title row.
    static TextBlock _buildNodeEditTitle()
    {
        TextBlock title{};
        title.Text(L"Edit session node");
        title.FontSize(18);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.Margin(WUX::Thickness{ 0, 0, 0, 10 });
        return title;
    }

    // axan #34: "Hoisted group": Default (follow sidebar.hoistStates) / Never / Custom, the
    // Custom list as the same state checkboxes as Settings > Sidebar, shown inline only
    // while Custom is chosen. Applied on Save, like the rest of the card.
    StackPanel TerminalPage::_BuildNodeEditHoistSection()
    {
        using namespace Axan::HoistRule;
        StackPanel section{};
        section.Spacing(2);
        section.Children().Append(_buildNodeEditSectionLabel(L"Hoisted group"));

        ComboBox mode{};
        mode.MinWidth(160);
        mode.Items().Append(winrt::box_value(winrt::hstring{ L"Default" }));
        mode.Items().Append(winrt::box_value(winrt::hstring{ L"Never" }));
        mode.Items().Append(winrt::box_value(winrt::hstring{ L"Custom" }));
        WUX::Automation::AutomationProperties::SetName(mode, L"Hoist in the sidebar");
        ToolTipService::SetToolTip(mode, winrt::box_value(winrt::hstring{ L"Default follows Settings > Sidebar; Never keeps this session out of the group; Custom picks its own states" }));
        mode.SelectedIndex(static_cast<int32_t>(Parse(_nodeEditHoist).mode));

        WUX::Controls::VariableSizedWrapGrid boxes{};
        boxes.Orientation(Orientation::Horizontal);
        boxes.Margin(WUX::Thickness{ 0, 6, 0, 0 });
        // One box per name in display order (checks[i] is kDisplayOrder[i], for sync below),
        // in columns as wide as the longest label, so "needs permission" etc. wrap instead of
        // clipping.
        std::vector<CheckBox> checks;
        Axan::Ui::CheckBoxWrap::BuildNameBoxes(
            boxes,
            kDisplayOrder,
            [](std::wstring_view name) { return DisplayName(name); },
            [&checks](const CheckBox& box, std::wstring_view) { checks.push_back(box); },
            [weakThis{ get_weak() }](const winrt::hstring& key, bool on) {
                if (auto p{ weakThis.get() })
                {
                    const auto o = Parse(p->_nodeEditHoist);
                    const Mask cur = o.mode == Mode::Custom ? o.mask : 0;
                    const auto next = With(cur, key, on);
                    // An emptied custom list stays Custom in the card (Save writes "never").
                    p->_nodeEditHoist = next == 0 ? winrt::hstring{ kNever } : winrt::hstring{ Format(FromMask(next)) };
                }
            });
        const auto sync = [checks, boxes](const winrt::hstring& hoist, bool custom) {
            const auto o = Parse(hoist);
            for (size_t i = 0; i < checks.size(); ++i)
            {
                checks[i].IsChecked(o.mode == Mode::Custom && Has(o.mask, kDisplayOrder[i]));
            }
            boxes.Visibility(custom ? Visibility::Visible : Visibility::Collapsed);
        };
        sync(_nodeEditHoist, Parse(_nodeEditHoist).mode == Mode::Custom);
        mode.SelectionChanged([weakThis{ get_weak() }, sync](const winrt::Windows::Foundation::IInspectable& s, auto&&) {
            auto p{ weakThis.get() };
            const auto combo = s.try_as<ComboBox>();
            if (!p || !combo || combo.SelectedIndex() < 0)
            {
                return;
            }
            const auto idx = combo.SelectedIndex();
            if (idx == static_cast<int32_t>(Mode::Never))
            {
                p->_nodeEditHoist = winrt::hstring{ kNever };
            }
            else if (idx == static_cast<int32_t>(Mode::Custom))
            {
                if (Parse(p->_nodeEditHoist).mode != Mode::Custom)
                {
                    p->_nodeEditHoist = winrt::hstring{ Format(FromMask(kDefault)) }; // start from the default list
                }
            }
            else
            {
                p->_nodeEditHoist = {};
            }
            sync(p->_nodeEditHoist, idx == static_cast<int32_t>(Mode::Custom));
        });

        section.Children().Append(mode);
        section.Children().Append(boxes);
        return section;
    }

    // The Save / Cancel footer. Save commits the editor state to the node's VM, kicks the
    // async label recompute, applies the icon/recolor, and persists to the source entry.
    StackPanel TerminalPage::_BuildNodeEditFooter()
    {
        Button saveBtn{};
        saveBtn.Content(winrt::box_value(winrt::hstring{ L"Save" }));
        if (const auto accentStyle = Application::Current().Resources().TryLookup(winrt::box_value(winrt::hstring{ L"AccentButtonStyle" })))
        {
            if (const auto st = accentStyle.try_as<WUX::Style>())
            {
                saveBtn.Style(st);
            }
        }
        saveBtn.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto p{ weakThis.get() })
            {
                if (const auto n = p->_nodeEditTarget.get())
                {
                    if (const auto v = _nodeVM(n))
                    {
                        if (p->_nodeEditLabelBox)
                        {
                            v->LabelTemplate = p->_nodeEditLabelBox.Text();
                            // #427/#430: same async path as the live recompute — it reads the
                            // template just stored on the VM, and the expansion (which can walk
                            // the filesystem for git variables) runs off the UI thread.
                            if (const auto tab = _nodeTab(n))
                            {
                                _RecomputeSessionLabelAsync(winrt::make_weak(n), winrt::make_weak(tab));
                            }
                        }
                        p->_ApplyNodeIcon(n, p->_nodeEditIconOverride, p->_nodeEditColor, p->_nodeEditColorTarget);
                        v->HoistOverride = p->_nodeEditHoist; // axan #34
                        p->_RefreshNeedsYouGroup();
                        // #422: persist the edit back to its curated startup entry so it survives
                        // a restart (no-op for a runtime session with no source entry).
                        p->_PersistNodeToEntry(n);
                        Axan::Log::Info("TerminalPage", "saved node edit (name + icon + color)");
                    }
                }
                p->_HideNodeEditPanel();
            }
        });
        Button cancelBtn{};
        cancelBtn.Content(winrt::box_value(winrt::hstring{ L"Cancel" }));
        cancelBtn.Click([weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_HideNodeEditPanel(); });
        StackPanel footer{};
        footer.Orientation(Orientation::Horizontal);
        footer.Spacing(10);
        footer.HorizontalAlignment(WUX::HorizontalAlignment::Right);
        footer.Margin(WUX::Thickness{ 0, 20, 0, 0 });
        footer.Children().Append(saveBtn);
        footer.Children().Append(cancelBtn);
        return footer;
    }

    // axan M13 (design handoff "Edit Session Node"): open the in-tree "Edit session node"
    // editor. In-tree (NodeEditOverlay scrim + NodeEditCard), not a popup, so the Session name
    // TextBox takes typed characters and the editor can't be orphaned on minimize. Layout mirrors
    // the design: [icon preview | Session name] with a "?" variables popover, an icon button row
    // + Browse, color swatches, and an Apply-color-to (Icon/Text/Both) radio. State on _nodeEdit*.
    // #436: each section is built by a helper above; this function seeds the _nodeEdit* state,
    // assembles the card, and owns the show/dismiss wiring.
    void TerminalPage::_ShowNodeEditPanel(const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        if (!vm || vm->IsSeparator()) // axan #3: separators are edited on the Startup sessions page
        {
            return;
        }
        const auto card = NodeEditCard();
        const auto overlay = NodeEditOverlay();
        if (!card || !overlay)
        {
            return;
        }

        _nodeEditTarget = winrt::make_weak(node);
        _nodeEditIconOverride = vm->IconOverride;
        _nodeEditColor = vm->IconColor();
        _nodeEditColorTarget = vm->ColorTarget.empty() ? winrt::hstring{ L"both" } : vm->ColorTarget;
        _nodeEditFallbackPath = _resolveNodeIconPath({}, _nodeTab(node));
        _nodeEditHoist = vm->HoistOverride; // axan #34
        // Capture the sidebar's resolved theme so the swatches + preview resolve color NAMES
        // against the matching palette set (the same derivation _RefreshNodeLabelBrushes uses).
        _nodeEditIsDark = _SidebarIsDarkTheme();

        // axan #38: the identity sections (name + "?" help, icon grid + Browse, swatches,
        // Apply color to) are the shared AxanEntryIdentityEditor, also hosted by the Startup
        // sessions settings page. Edits stage into _nodeEditIconOverride / _nodeEditColor /
        // _nodeEditColorTarget through onChanged; the name is read from the field on Save.
        // Nothing is applied until Save (the footer), exactly as before.
        namespace Identity = Axan::Ui::IdentityEditor;
        Identity::Options identity;
        identity.initial.name = vm->LabelTemplate;
        identity.initial.icon = _nodeEditIconOverride;
        identity.initial.color = _nodeEditColor;
        identity.initial.colorTarget = _nodeEditColorTarget;
        identity.isDark = _nodeEditIsDark;
        // The preview uses the same BuildIcon as the sidebar row, falls back to the node's own
        // icon before a glyph is picked, and tints only when the target paints the icon, so
        // it matches the realized row.
        identity.renderPreview = [weakThis{ get_weak() }](const Identity::Fields& f) -> WUX::UIElement {
            const auto p{ weakThis.get() };
            if (!p)
            {
                return nullptr;
            }
            const auto path = f.icon.empty() ? p->_nodeEditFallbackPath : f.icon;
            const auto effColor = (f.colorTarget != L"text") ? f.color : winrt::hstring{};
            return winrt::TerminalApp::implementation::SessionNodeViewModel::BuildIcon(path, effColor, p->_nodeEditIsDark);
        };
        identity.onChanged = [weakThis{ get_weak() }](const Identity::Fields& f) {
            if (const auto p{ weakThis.get() })
            {
                p->_nodeEditIconOverride = f.icon;
                p->_nodeEditColor = f.color;
                p->_nodeEditColorTarget = f.colorTarget;
            }
        };
        identity.onBrowseIcon = [weakThis{ get_weak() }]() {
            if (const auto p{ weakThis.get() })
            {
                p->_BrowseNodeIconFile();
            }
        };
        _nodeEditor = Identity::Build(std::move(identity));
        _nodeEditLabelBox = _nodeEditor->NameBox();

        // --- assemble ---
        StackPanel content{};
        content.Spacing(2);
        content.MinWidth(420);
        content.Children().Append(_buildNodeEditTitle());
        content.Children().Append(_nodeEditor->Root());
        content.Children().Append(_BuildNodeEditHoistSection()); // axan #34
        content.Children().Append(_BuildNodeEditFooter());

        // axan #436 item 4: Escape dismisses the editor from ANY focused control — the
        // handler sits on the card content (rebuilt each open, so handlers never stack) and
        // sees the bubbling KeyDown from the name field, buttons, swatches and radios alike.
        // Previously it was attached only to the name TextBox, so Esc went dead the moment
        // focus moved to anything else in the card.
        content.KeyDown([weakThis{ get_weak() }](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                if (auto p{ weakThis.get() })
                {
                    p->_HideNodeEditPanel();
                }
                e.Handled(true);
            }
        });

        card.Child(content); // the identity editor rendered its preview during Build

        overlay.Visibility(Visibility::Visible);

        // Focus the Session name field on its 2nd LayoutUpdated (in-tree focus timing).
        _nodeEditLayoutCount = 0;
        _nodeEditLayoutUpdatedRevoker.revoke();
        _nodeEditLayoutUpdatedRevoker = _nodeEditLabelBox.LayoutUpdated(winrt::auto_revoke, [weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto p{ weakThis.get() })
            {
                if (p->_nodeEditLayoutCount < 2) { p->_nodeEditLayoutCount++; }
                if (p->_nodeEditLayoutCount >= 2)
                {
                    p->_nodeEditLayoutUpdatedRevoker.revoke();
                    if (p->_nodeEditLabelBox) { p->_nodeEditLabelBox.Focus(FocusState::Programmatic); }
                }
            }
        });
        Axan::Log::Debug("TerminalPage", "opened node edit panel");
    }

    // axan M13 (design handoff): "Browse to icon file…" — pick an image/exe and set it as the
    // editor's current icon override; the preview updates. Mirrors WT's profile icon picker.
    // #38: the pick goes through the identity editor (SetIcon), which refreshes its preview and
    // stages the override via onChanged; a panel closed while the picker was up is left alone.
    safe_void_coroutine TerminalPage::_BrowseNodeIconFile()
    {
        auto strongThis{ get_strong() };
        if (!_hostingHwnd)
        {
            co_return;
        }
        static constexpr COMDLG_FILTERSPEC filterTypes[] = {
            { L"Images (*.png;*.ico;*.bmp;*.gif;*.jpg;*.svg)", L"*.png;*.ico;*.bmp;*.gif;*.jpg;*.jpeg;*.svg" },
            { L"Programs (*.exe;*.dll;*.lnk)", L"*.exe;*.dll;*.lnk" },
            { L"All Files (*.*)", L"*.*" }
        };
        const auto file = co_await OpenFilePicker(*_hostingHwnd, [](auto&& dialog) {
            try
            {
                THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(filterTypes), filterTypes));
                THROW_IF_FAILED(dialog->SetFileTypeIndex(1));
            }
            CATCH_LOG();
        });
        if (!file.empty() && _nodeEditor)
        {
            _nodeEditor->SetIcon(winrt::hstring{ file });
        }
    }

    // axan M13: hide the editor overlay and clear its transient state, so it never holds a node
    // past the edit. Safe to call from Save, Cancel, or Esc.
    void TerminalPage::_HideNodeEditPanel()
    {
        _nodeEditLayoutUpdatedRevoker.revoke();
        if (const auto overlay = NodeEditOverlay())
        {
            overlay.Visibility(Visibility::Collapsed);
        }
        if (const auto card = NodeEditCard())
        {
            card.Child(nullptr);
        }
        _nodeEditTarget = {};
        _nodeEditLabelBox = nullptr;
        _nodeEditor.reset(); // its controls go inert (they hold only a weak_ptr)
        _nodeEditIconOverride = {};
        _nodeEditColor = {};
        _nodeEditColorTarget = L"both";
        _nodeEditFallbackPath = {};
        _nodeEditHoist = {};
    }

    // axan M13: spawn a new session from this node's profile and nest it under the node.
    // _DuplicateTab synchronously appends the new tab to _tabs (firing the M6 reconcile, which
    // creates the node at the root and focuses the new tab); we then move that node under the
    // target so it lands as a child. If the new node can't be located the session still opens,
    // just at the root — non-fatal.
    void TerminalPage::_AddChildSession(const MUX::Controls::TreeViewNode& node)
    {
        const auto parentTab = _nodeTab(node);
        if (!parentTab)
        {
            return;
        }
        _DuplicateTab(*winrt::get_self<Tab>(parentTab));

        const auto focused = _GetFocusedTabIndex();
        if (!focused || *focused >= _tabs.Size())
        {
            return;
        }
        const auto newNode = _FindNodeForTab(_tabs.GetAt(*focused));
        if (!newNode || newNode == node)
        {
            return;
        }

        Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode> container{ nullptr };
        uint32_t index = 0;
        if (_locateNodeContainer(SessionTree().RootNodes(), newNode, container, index))
        {
            container.RemoveAt(index);
        }
        node.Children().Append(newNode);
        node.IsExpanded(true);
        // axan #3: the append may have landed the new child below a bottom-placed separator.
        _SinkBottomSeparators();
        Axan::Log::Info("TerminalPage", "added child session under node");
    }

    // axan M13: duplicate this node's session (same profile), landing at the root.
    void TerminalPage::_DuplicateNodeSession(const MUX::Controls::TreeViewNode& node)
    {
        if (const auto tab = _nodeTab(node))
        {
            _DuplicateTab(*winrt::get_self<Tab>(tab));
            Axan::Log::Info("TerminalPage", "duplicated node session");
        }
    }

    // axan M13: close this node's session. Removing the tab fires the M6 reconcile, which
    // self-heals the node out of the tree (children are promoted into its slot, never orphaned).
    void TerminalPage::_CloseNodeSession(const MUX::Controls::TreeViewNode& node)
    {
        if (const auto tab = _nodeTab(node))
        {
            Axan::Log::Info("TerminalPage", "closing node session from context menu");
            _RemoveTab(tab);
        }
    }

    // Method Description:
    // - axan M8: cycle the sidebar collapse state on each chevron click:
    //   expanded -> minimized -> collapsed -> expanded.
    void TerminalPage::_OnSidebarCollapseClick(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                               const winrt::Windows::UI::Xaml::RoutedEventArgs& /*args*/)
    {
        _CycleSidebarState();
    }

    // Method Description:
    // - axan M8: advance the sidebar one step through expanded -> minimized -> collapsed ->
    //   expanded. Shared by the header chevron and the Ctrl+Shift+B action (M8d).
    void TerminalPage::_CycleSidebarState()
    {
        const auto next = _sidebarState == SidebarState::Expanded  ? SidebarState::Minimized :
                          _sidebarState == SidebarState::Minimized ? SidebarState::Collapsed :
                                                                     SidebarState::Expanded;
        _SetSidebarState(next);
    }

    // Method Description:
    // - axan #429: move keyboard focus into the session sidebar (the focusSidebar action,
    //   default Ctrl+Shift+Y). Until now no action or access key ever moved focus from the
    //   terminal into the tree, so its built-in arrow/Enter/Shift+F10 handling was
    //   unreachable by keyboard. A collapsed sidebar expands first (its surfaces are hidden
    //   and unfocusable); a minimized sidebar keeps its state and focuses the flat icon
    //   list instead, since that's the navigator actually showing. Focus lands on the
    //   selected row (or the first one) so arrow keys take over from there; Enter then
    //   activates a session, which routes focus back to that session's terminal via the
    //   existing tab-selection path.
    void TerminalPage::_FocusSessionTree()
    {
        if (_sidebarState == SidebarState::Collapsed)
        {
            _SetSidebarState(SidebarState::Expanded);
        }

        if (_sidebarState == SidebarState::Minimized)
        {
            const auto list = MinimizedList();
            auto item = list.ContainerFromItem(list.SelectedItem()).try_as<WUX::Controls::ListViewItem>();
            if (!item)
            {
                item = list.ContainerFromIndex(0).try_as<WUX::Controls::ListViewItem>();
            }
            if (item)
            {
                item.Focus(FocusState::Keyboard);
            }
            else
            {
                list.Focus(FocusState::Keyboard);
            }
            return;
        }

        const auto tree = SessionTree();
        auto node = tree.SelectedNode();
        // axan #3: a separator row can't take focus (disabled, no tab stop) — skip one that is
        // selected or that leads the root list and land on the first session row instead.
        if (node && _nodeIsSeparator(node))
        {
            node = nullptr;
        }
        if (!node)
        {
            const auto roots = tree.RootNodes();
            for (uint32_t i = 0; i < roots.Size() && !node; ++i)
            {
                if (const auto candidate = roots.GetAt(i); !_nodeIsSeparator(candidate))
                {
                    node = candidate;
                }
            }
        }
        if (node)
        {
            // A sidebar expanded just above may not have realized row containers yet.
            tree.UpdateLayout();
            if (const auto item = tree.ContainerFromNode(node).try_as<MUX::Controls::TreeViewItem>(); item && item.Focus(FocusState::Keyboard))
            {
                return;
            }
        }
        // No rows, no realized container, or a row that refused focus — at least land focus
        // on the tree control.
        tree.Focus(FocusState::Keyboard);
    }

    // Method Description:
    // - axan M8: the collapsed hint strip is a single click target back to expanded.
    void TerminalPage::_OnSidebarExpandStripClick(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                  const winrt::Windows::UI::Xaml::RoutedEventArgs& /*args*/)
    {
        _SetSidebarState(SidebarState::Expanded);
    }

    // Method Description:
    // - axan M8: record the new collapse state (persisted to state.json so it's remembered
    //   on the next launch) and drive the UI to match.
    void TerminalPage::_SetSidebarState(SidebarState state)
    {
        _sidebarState = state;
        ApplicationState::SharedInstance().SidebarState(state);
        _ApplySidebarState(state);
    }

    // Method Description:
    // - axan M8: the width in px of the minimized icon column, from the persisted size step.
    double TerminalPage::_minimizedSidebarWidth() const
    {
        switch (ApplicationState::SharedInstance().SidebarMinimizedSize())
        {
        case SidebarMinimizedSize::Small:
            return 32.0;
        case SidebarMinimizedSize::Large:
            return 72.0;
        case SidebarMinimizedSize::Medium:
        default:
            return 44.0;
        }
    }

    // axan M12: the expanded sidebar's width (px). The draggable divider persists a chosen
    // width to state.json; clamp on read so a hand-edited or stale value can't produce an
    // unusable column. The bounds (kSidebarExpandedMin/MaxWidth) are shared with the
    // divider's live clamp in _OnSidebarResizeDelta (#436 item 2).
    double TerminalPage::_expandedSidebarWidth() const
    {
        const auto stored = static_cast<double>(ApplicationState::SharedInstance().SidebarExpandedWidth());
        if (stored < kSidebarExpandedMinWidth)
        {
            return kSidebarExpandedMinWidth;
        }
        if (stored > kSidebarExpandedMaxWidth)
        {
            return kSidebarExpandedMaxWidth;
        }
        return stored;
    }

    // Method Description:
    // - axan M12: live-resize the expanded sidebar as the divider Thumb is dragged. Resizing
    //   is meaningful only in the Expanded state (the minimized icon column and collapsed
    //   strip have their own fixed widths), so other states no-op. The width is clamped to a
    //   usable range; persistence happens once on drag-complete, not per delta.
    void TerminalPage::_OnSidebarResizeDelta(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                             const winrt::Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args)
    {
        if (_sidebarState != SidebarState::Expanded)
        {
            return;
        }
        auto next = SessionSidebarColumn().ActualWidth() + args.HorizontalChange();
        next = std::clamp(next, kSidebarExpandedMinWidth, kSidebarExpandedMaxWidth);
        SessionSidebarColumn().Width(GridLengthHelper::FromPixels(next));
    }

    // Method Description:
    // - axan M12: persist the expanded sidebar width once the divider drag finishes.
    void TerminalPage::_OnSidebarResizeCompleted(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                 const winrt::Windows::UI::Xaml::Controls::Primitives::DragCompletedEventArgs& /*args*/)
    {
        if (_sidebarState != SidebarState::Expanded)
        {
            return;
        }
        const auto width = static_cast<int32_t>(SessionSidebarColumn().ActualWidth());
        ApplicationState::SharedInstance().SidebarExpandedWidth(width);
        Axan::Log::Info("TerminalPage", "sidebar divider: persisted expanded width", { { "width", std::to_string(width) } });
    }

    // Method Description:
    // - axan M12 fix: flip the pointer to the west-east resize cursor while it's over the divider
    //   (and throughout a drag, since the Thumb keeps pointer capture). Wired to both PointerEntered
    //   and PointerMoved: XAML's default WM_SETCURSOR handling resets the cursor to the arrow on every
    //   mouse move, so we have to re-assert on each move to win. System Windows.UI.Xaml (the XAML
    //   Islands flavor WT hosts) exposes no per-element Cursor property, so this is the Win32 route —
    //   the same one the window's own NonClientIslandWindow uses for its resize borders.
    void TerminalPage::_OnSidebarResizeThumbHover(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                  const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& /*args*/)
    {
        if (_sidebarState != SidebarState::Expanded)
        {
            return;
        }
        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
    }

    // Method Description:
    // - axan M12 fix: restore the arrow once the pointer leaves the divider.
    void TerminalPage::_OnSidebarResizeThumbLeave(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                  const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& /*args*/)
    {
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    }

    // Method Description:
    // - axan M8: flatten the session tree (roots + all descendants, depth-first, in order)
    //   into _minimizedItems — the backing list the minimized-state MinimizedList renders as
    //   a flat column of node icons. Re-selects the row whose tab is currently active so the
    //   minimized list highlights the same session the tree/content does.
    void TerminalPage::_RebuildMinimizedItems()
    {
        if (!_minimizedItems)
        {
            return;
        }
        _minimizedItems.Clear();
        const auto active = _GetFocusedTab();
        winrt::TerminalApp::SessionNodeViewModel selected{ nullptr };
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            if (_nodeIsSeparator(node))
            {
                return; // axan #3: the minimized icon column shows sessions only
            }
            if (const auto vm = node.Content().try_as<winrt::TerminalApp::SessionNodeViewModel>())
            {
                _minimizedItems.Append(vm);
                if (active && _nodeTab(node) == active)
                {
                    selected = vm;
                }
            }
        });
        MinimizedList().SelectedItem(selected);
    }

    // Method Description:
    // - axan M8: clicking a minimized icon activates that session (same path as invoking the
    //   tree node) — resolve the view-model's tab and select it (#436 item 3: via the shared
    //   _SelectTabForNodeVM rather than a duplicate of _OnSessionTreeItemInvoked's body).
    void TerminalPage::_OnMinimizedItemClick(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                             const winrt::Windows::UI::Xaml::Controls::ItemClickEventArgs& args)
    {
        _SelectTabForNodeVM(args.ClickedItem().try_as<winrt::TerminalApp::SessionNodeViewModel>());
    }

    // Method Description:
    // - axan M8c: tick the width step that's currently in effect when the chevron's
    //   right-click flyout opens.
    void TerminalPage::_OnSidebarSizeFlyoutOpening(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                   const winrt::Windows::Foundation::IInspectable& /*args*/)
    {
        const auto size = ApplicationState::SharedInstance().SidebarMinimizedSize();
        SidebarSizeSmall().IsChecked(size == SidebarMinimizedSize::Small);
        SidebarSizeMedium().IsChecked(size == SidebarMinimizedSize::Medium);
        SidebarSizeLarge().IsChecked(size == SidebarMinimizedSize::Large);
    }

    // Method Description:
    // - axan M8c: choose the minimized-column width step. Persist it (ApplicationState) and,
    //   if the sidebar is showing minimized right now, re-apply so the width changes live.
    void TerminalPage::_OnSidebarSizeClick(const winrt::Windows::Foundation::IInspectable& sender,
                                           const winrt::Windows::UI::Xaml::RoutedEventArgs& /*args*/)
    {
        const auto item = sender.try_as<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem>();
        if (!item)
        {
            return;
        }
        const auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"");
        const auto size = tag == L"small"  ? SidebarMinimizedSize::Small :
                          tag == L"large"  ? SidebarMinimizedSize::Large :
                                             SidebarMinimizedSize::Medium;
        ApplicationState::SharedInstance().SidebarMinimizedSize(size);
        SidebarSizeSmall().IsChecked(size == SidebarMinimizedSize::Small);
        SidebarSizeMedium().IsChecked(size == SidebarMinimizedSize::Medium);
        SidebarSizeLarge().IsChecked(size == SidebarMinimizedSize::Large);
        if (_sidebarState == SidebarState::Minimized)
        {
            _ApplySidebarState(SidebarState::Minimized);
        }
    }

    // Method Description:
    // - axan M8: the single place that maps a collapse state onto the sidebar column width
    //   and which of the three surfaces is shown.
    //     Expanded  - full-width column (persisted divider width); the icon + label tree;
    //                 header (chevron).
    //     Minimized - narrow icon-only column (width = size step); the flat MinimizedList;
    //                 header keeps just the chevron (the save button's text won't fit).
    //     Collapsed - ~12px strip; header + tree + list hidden; the click-to-expand strip.
    void TerminalPage::_ApplySidebarState(SidebarState state)
    {
        // axan M12: the resize divider is only meaningful (and only shown) in the Expanded
        // state — the minimized icon column and collapsed strip own their fixed widths.
        switch (state)
        {
        case SidebarState::Minimized:
            _RebuildMinimizedItems();
            SessionSidebarColumn().Width(GridLengthHelper::FromPixels(_minimizedSidebarWidth()));
            SidebarHeader().Visibility(Visibility::Visible);
            // axan M14: the header now holds only the collapse chevron (the Save/Import actions are
            // retired — startup sessions are edited in Settings). Center it in the narrow icon column.
            SidebarCollapseButton().HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Center);
            SidebarCollapseButton().Margin(winrt::Windows::UI::Xaml::ThicknessHelper::FromLengths(0, 4, 0, 2));
            SessionTree().Visibility(Visibility::Collapsed);
            MinimizedList().Visibility(Visibility::Visible);
            SidebarExpandStrip().Visibility(Visibility::Collapsed);
            SidebarResizeThumb().Visibility(Visibility::Collapsed);
            break;
        case SidebarState::Collapsed:
            SessionSidebarColumn().Width(GridLengthHelper::FromPixels(12.0));
            SidebarHeader().Visibility(Visibility::Collapsed);
            SessionTree().Visibility(Visibility::Collapsed);
            MinimizedList().Visibility(Visibility::Collapsed);
            SidebarExpandStrip().Visibility(Visibility::Visible);
            SidebarResizeThumb().Visibility(Visibility::Collapsed);
            break;
        case SidebarState::Expanded:
        default:
            // axan M12: width comes from the persisted divider value, not a fixed 260.
            SessionSidebarColumn().Width(GridLengthHelper::FromPixels(_expandedSidebarWidth()));
            SidebarHeader().Visibility(Visibility::Visible);
            // axan M14: only the collapse chevron lives in the header now; keep it right-aligned.
            SidebarCollapseButton().HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Right);
            SidebarCollapseButton().Margin(winrt::Windows::UI::Xaml::ThicknessHelper::FromLengths(0, 4, 4, 2));
            SessionTree().Visibility(Visibility::Visible);
            MinimizedList().Visibility(Visibility::Collapsed);
            SidebarExpandStrip().Visibility(Visibility::Collapsed);
            SidebarResizeThumb().Visibility(Visibility::Visible);
            break;
        }
        Axan::Log::Info("TerminalPage", "applied sidebar state", { { "state", state == SidebarState::Minimized ? "minimized" : state == SidebarState::Collapsed ? "collapsed" : "expanded" } });
        _RefreshNeedsYouGroup(); // axan #34: only shown in the expanded state
    }

    // ----- axan #30: session state ---------------------------------------------------------

    // Gather the node's tab's panes into AxanSessionState inputs, derive, push onto the VM.
    // UI thread. If the node's tab is the focused tab in an activated window, the user is
    // looking at it, so its done/error records are acknowledged first (the bell rule: WT
    // clears the bell indicator for a focused tab the same way).
    // axan: the localized state word and its one-sentence explanation for a WordToken
    // (AxanStateGlyphs.h). Literal RS_ keys (USES_RESOURCE checks them at build time).
    static std::pair<winrt::hstring, winrt::hstring> _slotWordAndExplanation(std::wstring_view token)
    {
        if (token == L"idle")
        {
            return { RS_(L"AxanSlotWordIdle"), RS_(L"AxanSlotExplainIdle") };
        }
        if (token == L"working")
        {
            return { RS_(L"AxanSlotWordWorking"), RS_(L"AxanSlotExplainWorking") };
        }
        if (token == L"running")
        {
            return { RS_(L"AxanSlotWordRunning"), RS_(L"AxanSlotExplainRunning") };
        }
        if (token == L"needs-permission")
        {
            return { RS_(L"AxanSlotWordNeedsPermission"), RS_(L"AxanSlotExplainNeedsPermission") };
        }
        if (token == L"needs-answer")
        {
            return { RS_(L"AxanSlotWordNeedsAnswer"), RS_(L"AxanSlotExplainNeedsAnswer") };
        }
        if (token == L"needs-login")
        {
            return { RS_(L"AxanSlotWordNeedsLogin"), RS_(L"AxanSlotExplainNeedsLogin") };
        }
        if (token == L"needs-you")
        {
            return { RS_(L"AxanSlotWordNeedsYou"), RS_(L"AxanSlotExplainNeedsYou") };
        }
        if (token == L"done")
        {
            return { RS_(L"AxanSlotWordDone"), RS_(L"AxanSlotExplainDone") };
        }
        if (token == L"error")
        {
            return { RS_(L"AxanSlotWordError"), RS_(L"AxanSlotExplainError") };
        }
        return {};
    }

    // axan: the right-edge slot of a SessionRestingState for a slot state + blocked kind: the
    // sidebar.stateGlyphs glyph (a missing key keeps the default; "" shows nothing) and the
    // state word + explanation (sidebar.stateLabels hides only the visible word; the
    // accessible name and tooltips keep it). Shared by the real derive and the state preview.
    static void _fillSlot(SessionRestingState& resting, const CascadiaSettings& settings, std::wstring_view slotKey, std::wstring_view kind)
    {
        resting.slotKey = winrt::hstring{ slotKey };
        winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring> map{ nullptr };
        if (settings)
        {
            map = settings.GlobalSettings().SidebarStateGlyphs();
        }
        resting.slotGlyph = winrt::hstring{ Axan::StateGlyphs::GlyphFor(slotKey, kind, [&](std::wstring_view key) -> std::optional<std::wstring> {
            const winrt::hstring k{ key };
            if (map && map.HasKey(k))
            {
                return std::wstring{ map.Lookup(k) };
            }
            return std::nullopt;
        }) };
        const auto [word, explanation] = _slotWordAndExplanation(Axan::StateGlyphs::WordToken(slotKey, kind));
        resting.slotWord = word;
        resting.slotExplanation = explanation;
        resting.showSlotWord = !settings || settings.GlobalSettings().SidebarStateLabels();
    }

    // axan: the "Preview state" submenu's entries: what each one makes the row look like. The
    // message is what an OSC 7501 msg would carry; "Done, already seen" is the kept report
    // that sidebar.statusAfterSeen = keep leaves on an idle row.
    struct StatePreviewSpec
    {
        std::wstring_view id;
        const wchar_t* label; // resource key (USES_RESOURCE)
        std::wstring_view state;
        std::wstring_view kind;
        std::wstring_view slot;
        std::wstring_view message;
        std::wstring_view retainedWhat;
    };
    static const std::array<StatePreviewSpec, 10> kStatePreviews{ {
        { L"idle", USES_RESOURCE(L"AxanPreviewStateIdle"), L"idle", L"", L"idle", L"", L"" },
        { L"working", USES_RESOURCE(L"AxanPreviewStateWorking"), L"working", L"", L"working", L"Preview: editing files", L"" },
        { L"running", USES_RESOURCE(L"AxanPreviewStateRunning"), L"running", L"", L"running", L"", L"" },
        { L"needs-permission", USES_RESOURCE(L"AxanPreviewStateNeedsPermission"), L"needs-you", L"permission", L"blocked", L"Preview: allow a tool to run?", L"" },
        { L"needs-answer", USES_RESOURCE(L"AxanPreviewStateNeedsAnswer"), L"needs-you", L"question", L"blocked", L"Preview: waiting for your input", L"" },
        { L"needs-login", USES_RESOURCE(L"AxanPreviewStateNeedsLogin"), L"needs-you", L"auth", L"blocked", L"Preview: sign-in required", L"" },
        { L"needs-you", USES_RESOURCE(L"AxanPreviewStateNeedsYou"), L"needs-you", L"", L"blocked", L"Preview: waiting for you", L"" },
        { L"done", USES_RESOURCE(L"AxanPreviewStateDone"), L"done-unseen", L"", L"done", L"Preview: task finished", L"" },
        { L"error", USES_RESOURCE(L"AxanPreviewStateError"), L"error", L"", L"error", L"Preview: build failed", L"" },
        { L"done-seen", USES_RESOURCE(L"AxanPreviewStateDoneSeen"), L"idle", L"", L"idle", L"", L"done" },
    } };

    // axan: a pane's highest OSC 7501 record revision (0 for none): what "seen" and the state
    // preview's baseline are measured in.
    static uint64_t _maxRecordRevision(const winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::Terminal::Control::ProgramStatusRecord>& records)
    {
        uint64_t maxRevision = 0;
        for (const auto& r : records)
        {
            maxRevision = std::max(maxRevision, r.Revision);
        }
        return maxRevision;
    }

    void TerminalPage::_RecomputeNodeState(const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        const auto tab = _nodeTab(node);
        if (!vm || !tab || vm->IsSeparator())
        {
            return;
        }
        // axan: the row sizes (sidebar.labelFontSize etc.), NaN read as the default and
        // clamped to the settings page's ranges. A no-op unless one changed; this runs on node
        // creation and settings reload.
        if (_settings)
        {
            namespace Sizes = Axan::SidebarSizes;
            const auto globals = _settings.GlobalSettings();
            vm->SetSizes(Sizes::SanitizeSize(globals.SidebarLabelFontSize(), Sizes::kLabelFontSize),
                         Sizes::SanitizeSize(globals.SidebarStateLineFontSize(), Sizes::kStateLineFontSize),
                         Sizes::SanitizeSize(globals.SidebarIconSize(), Sizes::kIconSize),
                         Sizes::SanitizeSize(globals.SidebarStateGlyphSize(), Sizes::kStateGlyphSize));
        }
        // axan: sidebar.idleEffects, resolved once per settings change (_IdleEffectsMask); a
        // no-op unless it changed. Covers new nodes and the post-reload re-derive.
        vm->SetIdleEffects(static_cast<Axan::IdleEffects::Mask>(_IdleEffectsMask()));
        const auto tabImpl = winrt::get_self<Tab>(tab);
        const auto controls = tabImpl->AxanTerminalControls();
        const bool focused = _activated && tab == _GetFocusedTab();

        // Drop "seen" bookkeeping for panes that have closed (keyed by ContentId, which a
        // new pane never reuses), so a long-lived tab's map doesn't grow with every split.
        std::erase_if(vm->SeenRevisions, [&](const auto& entry) {
            return std::none_of(controls.begin(), controls.end(), [&](const auto& control) { return control.ContentId() == entry.first; });
        });

        // RecordInput holds string views; a deque keeps the owning hstrings at stable
        // addresses while the inputs are built.
        std::deque<winrt::hstring> texts;
        std::vector<Axan::SessionState::PaneInput> panes;
        Axan::SessionState::RevisionsByPane reportRevisions; // axan: a new report ends a state preview
        for (const auto& control : controls)
        {
            // One snapshot per control (each call takes the terminal read lock and
            // allocates); it feeds both the focused-tab acknowledge and the inputs.
            const auto records = control.ProgramStatusRecords();
            const auto contentId = control.ContentId();
            const auto maxRevision = _maxRecordRevision(records);
            reportRevisions[contentId] = maxRevision;
            if (focused)
            {
                auto& seen = vm->SeenRevisions[contentId];
                seen = std::max(seen, maxRevision);
            }

            auto& pane = panes.emplace_back();
            pane.commandRunning = control.ShellCommandRunning();
            pane.paneId = contentId;
            pane.terminalTitle = texts.emplace_back(control.Title()); // axan: agent name source
            const auto seenIt = vm->SeenRevisions.find(contentId);
            const uint64_t seenRevision = seenIt == vm->SeenRevisions.end() ? 0 : seenIt->second;
            pane.records.reserve(records.Size());
            for (const auto& r : records)
            {
                auto& in = pane.records.emplace_back();
                in.isRoot = r.Id.empty();
                in.id = texts.emplace_back(r.Id);
                in.state = static_cast<Axan::SessionState::RecordState>(r.State);
                in.kind = static_cast<Axan::SessionState::RecordKind>(r.Kind);
                in.revision = r.Revision;
                in.seen = r.Revision <= seenRevision;
                in.msg = texts.emplace_back(r.Msg);
                in.title = texts.emplace_back(r.Title);
            }
        }

        Axan::SessionState::Options options;
        options.keepAfterSeen = _settings && _settings.GlobalSettings().SidebarStatusAfterSeen() == winrt::Microsoft::Terminal::Settings::Model::SidebarStatusAfterSeen::Keep;
        const auto result = Axan::SessionState::Derive(panes, tabImpl->AxanBellIndicator(), options);
        const auto now = winrt::clock::now();
        SessionRestingState resting;
        if (result.retained.present)
        {
            resting.retainedWhat = result.retained.state == Axan::SessionState::RecordState::Error ? L"error" : L"done";
            resting.retainedMessage = winrt::hstring{ result.retained.message };
        }
        resting.reportKey = winrt::hstring{ result.reportKey };
        _fillSlot(resting, _settings, Axan::SessionState::SlotKey(result.slot), result.kind);
        // axan: the resting agent name.
        if (!_settings || _settings.GlobalSettings().SidebarShowAgentName())
        {
            resting.agentName = winrt::hstring{ result.agentName };
        }
        // axan: a new OSC 7501 report ends a state preview, so a preview is never mistaken for
        // what the program said. (SetSessionState also ends it on a real state change.)
        if (vm->IsPreviewing() && Axan::SessionState::HasNewReport(vm->PreviewBaselineRevisions, reportRevisions) && vm->ClearPreview(now))
        {
            Axan::Log::Debug("TerminalPage", "state preview cleared: new report", { { "label", winrt::to_string(vm->Label()) } });
        }
        const bool wasPreviewing = vm->IsPreviewing();
        // axan #34: hoist membership is matched on the slot state + kind, and the slot can
        // change while the state doesn't (an agent at rest -> state=clear, still idle), so
        // compare those around the update rather than relying on the transition alone.
        const auto hoistSlotBefore = vm->SlotKey();
        const auto hoistKindBefore = vm->StateKind();
        const bool transition = vm->SetSessionState(winrt::hstring{ Axan::SessionState::Name(result.state) }, winrt::hstring{ result.message }, winrt::hstring{ result.kind }, now, resting);
        if (wasPreviewing && !vm->IsPreviewing())
        {
            Axan::Log::Debug("TerminalPage", "state preview cleared: state changed", { { "label", winrt::to_string(vm->Label()) } });
        }
        if (transition)
        {
            _RecomputeSections(); // axan #33: a collapsed header's badge summarises hidden states
            Axan::Log::Info("SessionState",
                            "node state changed",
                            { { "label", winrt::to_string(vm->Label()) },
                              { "state", winrt::to_string(Axan::SessionState::Name(result.state)) },
                              { "kind", winrt::to_string(result.kind) },
                              { "source", std::string{ result.source } },
                              { "panes", std::to_string(panes.size()) } });
        }
        else if (vm->SlotKey() != hoistSlotBefore || vm->StateKind() != hoistKindBefore)
        {
            _RecomputeSections(); // axan #34: refreshes the hoisted group too
            Axan::Log::Debug("SessionState",
                             "node slot changed without a state transition",
                             { { "label", winrt::to_string(vm->Label()) },
                               { "from", winrt::to_string(hoistSlotBefore) },
                               { "to", winrt::to_string(vm->SlotKey()) } });
        }
    }

    // axan: the node menu's "Preview state" submenu: one check item per kStatePreviews entry
    // (the one being previewed is checked) and "Clear preview".
    void TerminalPage::_AppendStatePreviewSubMenu(const MenuFlyout& flyout, const MUX::Controls::TreeViewNode& node)
    {
        const auto vm = _nodeVM(node);
        if (!vm || vm->IsSeparator())
        {
            return;
        }
        const auto current = vm->PreviewId();
        MenuFlyoutSubItem sub{};
        sub.Text(RS_(L"AxanMenuPreviewState"));
        FontIcon fi{};
        fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
        fi.Glyph(L"\xE7B3"); // RedEye: a look at how it would appear
        sub.Icon(fi);
        for (const auto& spec : kStatePreviews)
        {
            ToggleMenuFlyoutItem item{};
            item.Text(GetLibraryResourceString(spec.label));
            item.IsChecked(std::wstring_view{ current } == spec.id);
            item.Click([node, id = std::wstring{ spec.id }, weakThis{ get_weak() }](auto&&, auto&&) {
                if (const auto p{ weakThis.get() })
                {
                    p->_PreviewNodeState(node, id);
                }
            });
            sub.Items().Append(item);
        }
        sub.Items().Append(MenuFlyoutSeparator{});
        MenuFlyoutItem clear{};
        clear.Text(RS_(L"AxanMenuPreviewClear"));
        clear.IsEnabled(vm->IsPreviewing());
        clear.Click([node, weakThis{ get_weak() }](auto&&, auto&&) {
            if (const auto p{ weakThis.get() })
            {
                p->_PreviewNodeState(node, {});
            }
        });
        sub.Items().Append(clear);
        flyout.Items().Append(sub);
    }

    // axan: show a node as if it were in a previewed state (id = a kStatePreviews id), or go
    // back to its real state (id = ""). Purely visual; see SessionNodeViewModel::SetPreview.
    // The age counts from two minutes ago so the second line shows one.
    void TerminalPage::_PreviewNodeState(const MUX::Controls::TreeViewNode& node, std::wstring_view id)
    {
        const auto vm = _nodeVM(node);
        const auto tab = _nodeTab(node);
        if (!vm || !tab || vm->IsSeparator())
        {
            return;
        }
        const auto now = winrt::clock::now();
        const auto spec = std::find_if(kStatePreviews.begin(), kStatePreviews.end(), [&](const auto& s) { return s.id == id; });
        if (id.empty() || spec == kStatePreviews.end())
        {
            if (vm->ClearPreview(now))
            {
                Axan::Log::Debug("TerminalPage", "state preview cleared: menu", { { "label", winrt::to_string(vm->Label()) } });
            }
            return;
        }
        Axan::SessionState::RevisionsByPane baseline;
        for (const auto& control : winrt::get_self<Tab>(tab)->AxanTerminalControls())
        {
            baseline[control.ContentId()] = _maxRecordRevision(control.ProgramStatusRecords());
        }
        SessionRestingState resting;
        _fillSlot(resting, _settings, spec->slot, spec->kind);
        if (!spec->retainedWhat.empty())
        {
            resting.retainedWhat = winrt::hstring{ spec->retainedWhat };
            resting.retainedMessage = winrt::hstring{ L"Preview: task finished" };
            resting.reportKey = winrt::hstring{ L"preview" };
        }
        vm->PreviewBaselineRevisions = std::move(baseline);
        vm->SetPreview(winrt::hstring{ spec->id }, winrt::hstring{ spec->state }, winrt::hstring{ spec->message }, winrt::hstring{ spec->kind }, now - std::chrono::minutes{ 2 }, now, resting);
        Axan::Log::Debug("TerminalPage", "state preview set", { { "label", winrt::to_string(vm->Label()) }, { "preview", winrt::to_string(spec->id) }, { "state", winrt::to_string(spec->state) }, { "kind", winrt::to_string(spec->kind) } });
    }

    // Mark everything the node currently shows as seen, then recompute. One-shot: called on
    // selection / pane focus / window activation, never per keystroke.
    void TerminalPage::_AcknowledgeNodeState(const MUX::Controls::TreeViewNode& node, const char* reason)
    {
        const auto vm = _nodeVM(node);
        const auto tab = _nodeTab(node);
        if (!vm || !tab)
        {
            return;
        }
        // axan #30: a background window isn't being looked at, so nothing in it is seen yet
        // (same rule as the implicit acknowledge in _RecomputeNodeState). WindowActivated
        // acknowledges the focused node when the user comes back.
        if (!_activated)
        {
            Axan::Log::Debug("SessionState", "node acknowledge skipped", { { "label", winrt::to_string(vm->Label()) }, { "reason", reason }, { "skipped", "window inactive" } });
            _RecomputeNodeState(node);
            return;
        }
        bool advanced = false;
        for (const auto& control : winrt::get_self<Tab>(tab)->AxanTerminalControls())
        {
            const auto maxRevision = _maxRecordRevision(control.ProgramStatusRecords());
            auto& seen = vm->SeenRevisions[control.ContentId()];
            if (maxRevision > seen)
            {
                seen = maxRevision;
                advanced = true;
            }
        }
        if (advanced)
        {
            Axan::Log::Debug("SessionState", "node acknowledged", { { "label", winrt::to_string(vm->Label()) }, { "reason", reason } });
        }
        _RecomputeNodeState(node);
    }

    // axan: re-derive every node, e.g. after sidebar.statusAfterSeen / stateGlyphs changed
    // (settings reload or the sidebar menu).
    void TerminalPage::_RecomputeAllNodeStates()
    {
        _idleEffectsMask.reset(); // axan: re-resolve sidebar.idleEffects (each node picks it up below)
        if (!SessionTree())
        {
            return;
        }
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            _RecomputeNodeState(node);
        });
    }

    // axan: sidebar.iconFontScale. IconPathConverter keeps one process-wide copy (every
    // window's icons, the Settings UI's previews included, render through it) and a generation
    // that changes only when that copy does; each page tracks the generation it last applied
    // so every window rebuilds its own node icons after a change, whichever window's reload
    // pushed the new map first. Icons are only rebuilt on a change.
    void TerminalPage::_ApplyIconFontScales()
    {
        winrt::Windows::Foundation::Collections::IMap<winrt::hstring, double> map{ nullptr };
        if (_settings)
        {
            map = _settings.GlobalSettings().SidebarIconFontScale();
        }
        const auto generation = winrt::Microsoft::Terminal::UI::IconPathConverter::SetIconFontScales(map);
        if (_appliedIconFontScales == generation)
        {
            return;
        }
        const bool firstApply = !_appliedIconFontScales;
        _appliedIconFontScales = generation;
        Axan::Log::Debug("TerminalPage", "icon font scales applied", { { "families", std::to_string(map ? map.Size() : 0u) }, { "generation", std::to_string(generation) } });
        if (!firstApply && SessionTree())
        {
            _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
                if (const auto vm = _nodeVM(node))
                {
                    vm->RefreshIcons();
                }
            });
        }
    }

    void TerminalPage::_AcknowledgeFocusedNode(const char* reason)
    {
        if (const auto tab = _GetFocusedTab())
        {
            if (const auto node = _FindNodeForTab(tab))
            {
                _AcknowledgeNodeState(node, reason);
            }
            _RecomputeSections(); // axan #33: the focused row is exempt from section hiding
        }
    }

    // The ONE sidebar-wide 1s clock (#30 ambient activity; #32 ages and #31 idle
    // de-emphasis hang off the same tick). Started lazily when the first node is created.
    void TerminalPage::_EnsureSessionStateClock()
    {
        if (_sessionStateClock)
        {
            return;
        }
        _sessionStateClock = Windows::UI::Xaml::DispatcherTimer{};
        _sessionStateClock.Interval(std::chrono::seconds{ 1 });
        _sessionStateClock.Tick([weakThis{ get_weak() }](auto&&, auto&&) {
            if (const auto page{ weakThis.get() })
            {
                page->_SessionStateClockTick();
            }
        });
        _sessionStateClock.Start();
        Axan::Log::Debug("SessionState", "session state clock started");
    }

    void TerminalPage::_SessionStateClockTick()
    {
        const auto now = winrt::clock::now();
        const auto idleAfterMinutes = _settings ? _settings.GlobalSettings().SidebarIdleAfterMinutes() : 0; // axan #31
        const auto focusedTab = _GetFocusedTab(); // axan #31: the row being looked at is never de-emphasised
        const auto idleEffects = static_cast<Axan::IdleEffects::Mask>(_IdleEffectsMask()); // axan: sidebar.idleEffects (cached)
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            const auto vm = _nodeVM(node);
            const auto tab = _nodeTab(node);
            if (!vm || !tab || vm->IsSeparator())
            {
                return;
            }
            winrt::Windows::Foundation::DateTime latest{};
            for (const auto& control : winrt::get_self<Tab>(tab)->AxanTerminalControls())
            {
                latest = std::max({ latest, control.LastInputTime(), control.LastOutputTime() });
            }
            vm->SetLastActivity(latest);
            vm->RefreshAge(now); // axan #32: only rows showing an age ever change
            if (vm->UpdateDeEmphasis(now, tab == focusedTab ? 0 : idleAfterMinutes)) // axan #31
            {
                Axan::Log::Debug("SessionState", vm->IsDeEmphasised() ? "row de-emphasised: idle" : "row promoted", { { "label", winrt::to_string(vm->Label()) }, { "idleAfterMinutes", std::to_string(idleAfterMinutes) } });
            }
        });
        // axan: the collapse pass walks the whole tree, so skip it while the effect is off,
        // except for the one pass after it was turned off that re-expands what it collapsed
        // (after that pass no node has AutoCollapsed set).
        const bool collapse = (idleEffects & Axan::IdleEffects::kCollapse) != 0;
        if (collapse || _idleCollapseApplied)
        {
            _ApplyIdleCollapse(collapse);
            _idleCollapseApplied = collapse;
        }
    }

    // axan: sidebar.idleEffects as a mask, resolved on first use and again after
    // _RecomputeAllNodeStates drops the cache (settings reload, sidebar menu). Not cached
    // before settings exist, so an early call can't pin the default.
    uint8_t TerminalPage::_IdleEffectsMask()
    {
        if (!_idleEffectsMask)
        {
            const auto mask = _ResolveIdleEffects();
            if (!_settings)
            {
                return mask;
            }
            _idleEffectsMask = mask;
            Axan::Log::Debug("SessionState", "idle effects resolved", { { "mask", std::to_string(mask) } });
        }
        return *_idleEffectsMask;
    }

    // axan: sidebar.idleEffects -> mask. Unset = dim + smaller (the original #31 look); an
    // empty list = no effects; unknown names are ignored and logged once per process.
    uint8_t TerminalPage::_ResolveIdleEffects() const
    {
        if (!_settings || !_settings.GlobalSettings().HasSidebarIdleEffects())
        {
            return Axan::IdleEffects::kDefault;
        }
        const auto names = _settings.GlobalSettings().SidebarIdleEffects();
        if (!names)
        {
            return Axan::IdleEffects::kDefault;
        }
        std::vector<std::wstring> unknown;
        const auto mask = Axan::IdleEffects::MaskFromNames(names, &unknown);
        static std::set<std::wstring> logged;
        for (const auto& u : unknown)
        {
            if (logged.insert(u).second)
            {
                Axan::Log::Warn("SessionState", "sidebar.idleEffects: unknown effect name ignored", { { "name", winrt::to_string(u) } });
            }
        }
        return mask;
    }

    // axan: the "collapse" idle effect. Rule: a session node with session children closes
    // its expander when it AND every session descendant are de-emphasised (idle past
    // sidebar.idleAfterMinutes). The focused session is never de-emphasised, so a node that
    // is or contains the focused pane never qualifies. It fires on the transition into
    // all-idle only, so a user who re-expands an idle subtree isn't fought every second.
    // When any node in the subtree becomes active again (or the effect is turned off), a
    // node the page collapsed is re-expanded, the same way dim/smaller are undone; a node
    // the user collapsed by hand is never touched. Separators don't count either way.
    void TerminalPage::_ApplyIdleCollapse(bool enabled)
    {
        const auto visit = [&](const auto& self, const MUX::Controls::TreeViewNode& node) -> bool {
            bool childrenIdle = true;
            bool hasSessionChild = false;
            for (const auto& child : node.Children())
            {
                const bool childIdle = self(self, child);
                if (!_nodeIsSeparator(child))
                {
                    hasSessionChild = true;
                    childrenIdle = childrenIdle && childIdle;
                }
            }
            const auto vm = _nodeVM(node);
            if (!vm || vm->IsSeparator())
            {
                return true;
            }
            const bool subtreeIdle = enabled && vm->IsDeEmphasised() && childrenIdle;
            if (subtreeIdle && !vm->SubtreeIdle && hasSessionChild && node.IsExpanded())
            {
                node.IsExpanded(false);
                vm->AutoCollapsed = true;
                Axan::Log::Debug("SessionState", "idle subtree collapsed", { { "label", winrt::to_string(vm->Label()) } });
            }
            else if (!subtreeIdle && vm->AutoCollapsed)
            {
                vm->AutoCollapsed = false;
                if (!node.IsExpanded() && node.Children().Size() > 0)
                {
                    node.IsExpanded(true);
                    Axan::Log::Debug("SessionState", enabled ? "idle subtree re-expanded: activity" : "idle subtree re-expanded: effect off", { { "label", winrt::to_string(vm->Label()) } });
                }
            }
            vm->SubtreeIdle = subtreeIdle;
            return subtreeIdle;
        };
        for (const auto& root : SessionTree().RootNodes())
        {
            visit(visit, root);
        }
    }

    // ----- axan #33: section headers ---------------------------------------------------------

    static int _stateRank(const winrt::hstring& state) noexcept
    {
        if (state == L"needs-you")
        {
            return 5;
        }
        if (state == L"error")
        {
            return 4;
        }
        if (state == L"done-unseen")
        {
            return 3;
        }
        if (state == L"working")
        {
            return 2;
        }
        if (state == L"running")
        {
            return 1;
        }
        return 0;
    }

    static std::wstring _stateBadge(int rank, uint32_t count)
    {
        const auto n = std::to_wstring(count);
        switch (rank)
        {
        case 5:
            return n + L" needs you";
        case 4:
            return n + (count == 1 ? L" error" : L" errors");
        case 3:
            return n + L" done";
        case 2:
            return n + L" working";
        case 1:
            return n + L" running";
        default:
            return {};
        }
    }

    // Recompute which rows a collapsed section hides and what each collapsed header
    // summarises. The model never changes shape: a hidden row keeps its place among its
    // siblings and only its VM's IsSectionHidden (mapped onto the container's Visibility by
    // _ApplySeparatorContainerState) flips. A section is the run of siblings after a
    // labelled separator up to the next separator at that depth; a hidden row's children
    // hide with it. The focused session (and its ancestors) is exempt: collapsing the section
    // you're working in keeps your row visible under the header instead of hiding it, and
    // it isn't counted in the header's summary. It hides once you move elsewhere.
    void TerminalPage::_RecomputeSections()
    {
        if (!SessionTree())
        {
            return;
        }
        std::vector<MUX::Controls::TreeViewNode> activeChain;
        if (const auto tab = _GetFocusedTab())
        {
            for (auto n = _FindNodeForTab(tab); n && n.Depth() >= 0; n = n.Parent())
            {
                activeChain.push_back(n);
            }
        }
        const auto isActive = [&](const MUX::Controls::TreeViewNode& node) {
            return std::find(activeChain.begin(), activeChain.end(), node) != activeChain.end();
        };

        size_t hiddenRows = 0;
        size_t collapsedHeaders = 0;
        const std::function<void(const Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>&, bool)> walk =
            [&](const Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode>& container, bool parentHidden) {
                if (!container)
                {
                    return;
                }
                winrt::com_ptr<winrt::TerminalApp::implementation::SessionNodeViewModel> headerVm;
                uint32_t count = 0;
                int strongest = 0;
                uint32_t strongestCount = 0;
                const auto finish = [&]() {
                    if (headerVm)
                    {
                        if (headerVm->IsCollapsed())
                        {
                            ++collapsedHeaders;
                            headerVm->SetSectionSummary(winrt::hstring{ L" \u00B7 " + std::to_wstring(count) }, winrt::hstring{ _stateBadge(strongest, strongestCount) });
                        }
                        else
                        {
                            headerVm->SetSectionSummary({}, {});
                        }
                    }
                    headerVm = nullptr;
                    count = 0;
                    strongest = 0;
                    strongestCount = 0;
                };
                const std::function<void(const MUX::Controls::TreeViewNode&)> tally = [&](const MUX::Controls::TreeViewNode& n) {
                    if (const auto v = _nodeVM(n); v && !v->IsSeparator())
                    {
                        ++count;
                        const auto rank = _stateRank(v->State());
                        if (rank > strongest)
                        {
                            strongest = rank;
                            strongestCount = 1;
                        }
                        else if (rank == strongest && rank > 0)
                        {
                            ++strongestCount;
                        }
                    }
                    for (const auto& c : n.Children())
                    {
                        tally(c);
                    }
                };

                for (const auto& node : container)
                {
                    const auto vm = _nodeVM(node);
                    if (!vm)
                    {
                        continue;
                    }
                    const bool exempt = isActive(node);
                    if (vm->IsSeparator())
                    {
                        finish();
                        headerVm = vm->HasSectionHeader() ? vm : nullptr;
                        vm->SetSectionHidden(parentHidden);
                        walk(node.Children(), parentHidden);
                        continue;
                    }
                    const bool bySection = headerVm && headerVm->IsCollapsed() && !exempt;
                    const bool hidden = (parentHidden && !exempt) || bySection;
                    vm->SetSectionHidden(hidden);
                    if (hidden)
                    {
                        ++hiddenRows;
                    }
                    if (bySection)
                    {
                        tally(node);
                    }
                    walk(node.Children(), hidden);
                }
                finish();
            };
        walk(SessionTree().RootNodes(), false);
        _ApplySeparatorContainerStates();
        _RefreshNeedsYouGroup(); // axan #34: same trigger set (mutations, state transitions, focus)
        if (collapsedHeaders > 0)
        {
            Axan::Log::Debug("TerminalPage", "recomputed sections", { { "collapsedHeaders", std::to_string(collapsedHeaders) }, { "hiddenRows", std::to_string(hiddenRows) } });
        }
    }

    void TerminalPage::_SetSectionCollapsed(const MUX::Controls::TreeViewNode& header, bool collapsed, const char* reason)
    {
        const auto vm = _nodeVM(header);
        if (!vm || !vm->HasSectionHeader() || vm->IsCollapsed() == collapsed)
        {
            return;
        }
        vm->SetCollapsed(collapsed);
        Axan::Log::Info("TerminalPage", collapsed ? "section collapsed" : "section expanded", { { "label", winrt::to_string(vm->SeparatorLabel()) }, { "reason", reason } });
        _RecomputeSections();
    }

    // The section header a row sits under (nearest preceding separator among its siblings),
    // or nullptr when it isn't in a labelled section.
    MUX::Controls::TreeViewNode TerminalPage::_SectionHeaderFor(const MUX::Controls::TreeViewNode& node)
    {
        Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode> container{ nullptr };
        uint32_t index = 0;
        if (!_locateNodeContainer(SessionTree().RootNodes(), node, container, index))
        {
            return nullptr;
        }
        for (uint32_t i = index; i-- > 0;)
        {
            const auto sibling = container.GetAt(i);
            if (const auto vm = _nodeVM(sibling); vm && vm->IsSeparator())
            {
                return vm->HasSectionHeader() ? sibling : nullptr;
            }
        }
        return nullptr;
    }

    // Expand every collapsed section that hides `node` (its own, and its ancestors').
    void TerminalPage::_ExpandSectionsContaining(const MUX::Controls::TreeViewNode& node, const char* reason)
    {
        bool changed = false;
        for (auto n = node; n && n.Depth() >= 0; n = n.Parent())
        {
            if (const auto header = _SectionHeaderFor(n))
            {
                if (const auto vm = _nodeVM(header); vm && vm->IsCollapsed())
                {
                    vm->SetCollapsed(false);
                    changed = true;
                    Axan::Log::Info("TerminalPage", "section expanded", { { "label", winrt::to_string(vm->SeparatorLabel()) }, { "reason", reason } });
                }
            }
        }
        if (changed)
        {
            _RecomputeSections();
        }
    }

    // The badge: expand the section and select the hidden row with the strongest state
    // (on a tie, the one that has been in that state longest).
    void TerminalPage::_ActivateSectionBadge(const MUX::Controls::TreeViewNode& header)
    {
        Windows::Foundation::Collections::IVector<MUX::Controls::TreeViewNode> container{ nullptr };
        uint32_t index = 0;
        if (!_locateNodeContainer(SessionTree().RootNodes(), header, container, index))
        {
            return;
        }
        MUX::Controls::TreeViewNode target{ nullptr };
        int bestRank = 0;
        winrt::Windows::Foundation::DateTime bestSince{};
        const std::function<void(const MUX::Controls::TreeViewNode&)> scan = [&](const MUX::Controls::TreeViewNode& n) {
            if (const auto v = _nodeVM(n); v && !v->IsSeparator())
            {
                const auto rank = _stateRank(v->State());
                if (rank > bestRank || (rank == bestRank && rank > 0 && v->StateSince() < bestSince))
                {
                    bestRank = rank;
                    bestSince = v->StateSince();
                    target = n;
                }
            }
            for (const auto& c : n.Children())
            {
                scan(c);
            }
        };
        for (uint32_t i = index + 1; i < container.Size(); ++i)
        {
            const auto sibling = container.GetAt(i);
            if (_nodeIsSeparator(sibling))
            {
                break;
            }
            scan(sibling);
        }
        _SetSectionCollapsed(header, false, "badge");
        if (target)
        {
            _SelectTabForNodeVM(target.Content().try_as<winrt::TerminalApp::SessionNodeViewModel>());
        }
    }

    // Right-click on a section header: Collapse / Expand. Headers are not keyboard focusable
    // (separators keep IsTabStop(false) under the #3 lock), so this and the row click are the
    // two ways to toggle; there is no Left/Right-on-focused-header binding.
    void TerminalPage::_ShowSectionHeaderMenu(const MUX::Controls::TreeViewNode& header, const winrt::Windows::Foundation::Point& position)
    {
        if (_sessionNodeMenu && _sessionNodeMenu.IsOpen())
        {
            return;
        }
        const auto vm = _nodeVM(header);
        if (!vm)
        {
            return;
        }
        MenuFlyout flyout{};
        MenuFlyoutItem toggle{};
        toggle.Text(vm->IsCollapsed() ? RS_(L"AxanMenuExpandSection") : RS_(L"AxanMenuCollapseSection"));
        FontIcon fi{};
        fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
        fi.Glyph(vm->IsCollapsed() ? winrt::hstring{ L"\uE70D" } : winrt::hstring{ L"\uE76C" });
        toggle.Icon(fi);
        toggle.Click([header, weakThis{ get_weak() }](auto&&, auto&&) {
            if (const auto p{ weakThis.get() })
            {
                if (const auto v = _nodeVM(header))
                {
                    p->_SetSectionCollapsed(header, !v->IsCollapsed(), "context menu");
                }
            }
        });
        flyout.Items().Append(toggle);
        flyout.Items().Append(MenuFlyoutSeparator{});
        _AppendNewSessionSplitItem(flyout, L"Segoe MDL2 Assets");
        _AppendNeedsYouGroupToggle(flyout); // axan #34
        _sessionNodeMenu = flyout;
        flyout.Closed([weak = get_weak()](auto&&, auto&&) {
            if (auto p{ weak.get() })
            {
                p->_UpdateSeparatorUnlockFromKeyboard("section menu closed");
            }
        });
        flyout.ShowAt(SessionTree(), position);
    }

    // ----- axan #34: hoisted "Needs you" group ---------------------------------------------

    // Rebuild the small projection above the tree: every session node whose #30 state is
    // needs-you / error / done-unseen, longest-waiting first (StateSince ascending). The
    // list holds the SAME SessionNodeViewModel objects the tree shows (nothing duplicated in
    // the model); it is rebuilt only when its membership or order changes. Hidden entirely
    // when the setting is off, when it would be empty, or when the sidebar isn't expanded.
    void TerminalPage::_RefreshNeedsYouGroup()
    {
        if (!SessionTree())
        {
            return;
        }
        if (!_needsYouItems)
        {
            _needsYouItems = winrt::single_threaded_observable_vector<winrt::TerminalApp::SessionNodeViewModel>();
            NeedsYouList().ItemsSource(_needsYouItems);
        }
        const bool enabled = _settings && _settings.GlobalSettings().SidebarNeedsYouGroup();
        // axan #34: the global list (sidebar.hoistStates; unset = blocked / error / done).
        auto globalMask = Axan::HoistRule::kDefault;
        if (_settings && _settings.GlobalSettings().HasSidebarHoistStates())
        {
            if (const auto names = _settings.GlobalSettings().SidebarHoistStates())
            {
                std::vector<std::wstring> unknown;
                globalMask = Axan::HoistRule::MaskFromNames(names, &unknown);
                static std::set<std::wstring> logged; // log each unknown name once per process
                for (const auto& u : unknown)
                {
                    if (logged.insert(u).second)
                    {
                        Axan::Log::Warn("TerminalPage", "sidebar.hoistStates: unknown state name ignored", { { "name", winrt::to_string(u) } });
                    }
                }
            }
        }
        {
            const auto header = winrt::hstring{ Axan::HoistRule::IsAttentionOnly(globalMask) ? RS_(L"AxanHoistGroupNeedsYou") : RS_(L"AxanHoistGroupHoisted") };
            if (NeedsYouHeader().Text() != header)
            {
                NeedsYouHeader().Text(header);
                WUX::Automation::AutomationProperties::SetName(NeedsYouList(), header);
            }
        }
        std::vector<std::pair<winrt::Windows::Foundation::DateTime, winrt::TerminalApp::SessionNodeViewModel>> wanted;
        if (enabled)
        {
            _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
                const auto vm = _nodeVM(node);
                if (!vm || vm->IsSeparator())
                {
                    return;
                }
                // Matched on the slot state + blocked kind (AxanHoistRule.h): blocked = any
                // kind, done = an unseen report, idle = an agent at rest (never a plain shell).
                const auto slot = vm->SlotKey();
                const auto kind = vm->StateKind();
                if (Axan::HoistRule::IsHoisted(slot, kind, Axan::HoistRule::Parse(vm->HoistOverride), globalMask))
                {
                    wanted.emplace_back(vm->StateSince(), node.Content().as<winrt::TerminalApp::SessionNodeViewModel>());
                }
            });
            std::stable_sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }

        bool same = wanted.size() == _needsYouItems.Size();
        for (uint32_t i = 0; same && i < wanted.size(); ++i)
        {
            same = _needsYouItems.GetAt(i) == wanted[i].second;
        }
        if (!same)
        {
            std::vector<winrt::TerminalApp::SessionNodeViewModel> items;
            items.reserve(wanted.size());
            for (const auto& w : wanted)
            {
                items.push_back(w.second);
            }
            _needsYouItems.ReplaceAll(items);
            Axan::Log::Debug("TerminalPage", "needs-you group refreshed", { { "rows", std::to_string(items.size()) } });
        }
        const bool show = enabled && !wanted.empty() && SessionTree().Visibility() == Visibility::Visible;
        const auto vis = show ? Visibility::Visible : Visibility::Collapsed;
        if (NeedsYouGroup().Visibility() != vis)
        {
            NeedsYouGroup().Visibility(vis);
        }
    }

    // axan #34: the hoist override stored on a startup entry ("" when the entry is unknown,
    // or for a runtime session with no entry).
    winrt::hstring TerminalPage::_HoistOverrideForEntry(const winrt::hstring& entryId) const
    {
        if (entryId.empty() || !_settings)
        {
            return {};
        }
        if (const auto entries = _settings.GlobalSettings().StartupSessions())
        {
            for (const auto& e : entries)
            {
                if (e.Id() == entryId)
                {
                    return e.Hoist();
                }
            }
        }
        return {};
    }

    // axan #34: on settings reload, re-read every curated node's hoist override, so a change
    // made on the Startup sessions page applies to the live tree. Runtime nodes (no entry id)
    // keep their live-only value.
    void TerminalPage::_SyncHoistOverridesFromSettings()
    {
        if (!SessionTree() || !_settings)
        {
            return;
        }
        std::map<winrt::hstring, winrt::hstring> hoists;
        if (const auto entries = _settings.GlobalSettings().StartupSessions())
        {
            for (const auto& e : entries)
            {
                if (!e.IsSeparator() && !e.Id().empty())
                {
                    hoists[e.Id()] = e.Hoist();
                }
            }
        }
        _forEachSessionNode(SessionTree().RootNodes(), [&](const MUX::Controls::TreeViewNode& node) {
            if (const auto vm = _nodeVM(node); vm && !vm->IsSeparator() && !vm->EntryId.empty())
            {
                if (const auto it = hoists.find(vm->EntryId); it != hoists.end())
                {
                    vm->HoistOverride = it->second;
                }
            }
        });
        _RefreshNeedsYouGroup();
    }

    void TerminalPage::_OnNeedsYouItemClick(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                            const winrt::Windows::UI::Xaml::Controls::ItemClickEventArgs& args)
    {
        _SelectTabForNodeVM(args.ClickedItem().try_as<winrt::TerminalApp::SessionNodeViewModel>());
    }

    // The toggle (sidebar background / separator right-click menu). Persisted as the global
    // `sidebar.needsYouGroup` setting.
    void TerminalPage::_SetNeedsYouGroupEnabled(bool enabled)
    {
        if (!_settings || _settings.GlobalSettings().SidebarNeedsYouGroup() == enabled)
        {
            return;
        }
        _settings.GlobalSettings().SidebarNeedsYouGroup(enabled);
        if (!_settings.WriteSettingsToDisk())
        {
            Axan::Log::Error("TerminalPage", "needs-you group toggle: WriteSettingsToDisk failed; applied live only");
        }
        Axan::Log::Info("TerminalPage", enabled ? "needs-you group enabled" : "needs-you group disabled");
        _RefreshNeedsYouGroup();
    }

    // Shared by the background and section-header menus.
    void TerminalPage::_AppendNeedsYouGroupToggle(const MenuFlyout& flyout)
    {
        ToggleMenuFlyoutItem item{};
        item.Text(RS_(L"AxanMenuShowNeedsYouGroup"));
        item.IsChecked(_settings && _settings.GlobalSettings().SidebarNeedsYouGroup());
        item.Click([weakThis{ get_weak() }](auto&& sender, auto&&) {
            if (const auto p{ weakThis.get() })
            {
                if (const auto t = sender.template try_as<ToggleMenuFlyoutItem>())
                {
                    p->_SetNeedsYouGroupEnabled(t.IsChecked());
                }
            }
        });
        flyout.Items().Append(MenuFlyoutSeparator{});
        flyout.Items().Append(item);

        // axan: the two session-status display settings ride along in the same menus.
        // Persisted to settings.json like the group toggle; every row re-derives at once.
        ToggleMenuFlyoutItem keepItem{};
        keepItem.Text(RS_(L"AxanMenuKeepStatusAfterOpening"));
        keepItem.IsChecked(_settings && _settings.GlobalSettings().SidebarStatusAfterSeen() == winrt::Microsoft::Terminal::Settings::Model::SidebarStatusAfterSeen::Keep);
        keepItem.Click([weakThis{ get_weak() }](auto&& sender, auto&&) {
            if (const auto p{ weakThis.get() })
            {
                if (const auto t = sender.template try_as<ToggleMenuFlyoutItem>())
                {
                    p->_SetSidebarStatusSetting("statusAfterSeen", [keep = t.IsChecked()](const auto& globals) {
                        const auto want = keep ? winrt::Microsoft::Terminal::Settings::Model::SidebarStatusAfterSeen::Keep : winrt::Microsoft::Terminal::Settings::Model::SidebarStatusAfterSeen::Clear;
                        if (globals.SidebarStatusAfterSeen() == want)
                        {
                            return false;
                        }
                        globals.SidebarStatusAfterSeen(want);
                        return true;
                    });
                }
            }
        });
        flyout.Items().Append(keepItem);
    }

    // axan: apply one sidebar status-display setting change from the menu, persist it, and
    // re-derive every row.
    void TerminalPage::_SetSidebarStatusSetting(const char* name, const std::function<bool(const winrt::Microsoft::Terminal::Settings::Model::GlobalAppSettings&)>& apply)
    {
        if (!_settings || !apply(_settings.GlobalSettings()))
        {
            return;
        }
        if (!_settings.WriteSettingsToDisk())
        {
            Axan::Log::Error("TerminalPage", "sidebar status setting: WriteSettingsToDisk failed; applied live only", { { "setting", name } });
        }
        Axan::Log::Info("TerminalPage", "sidebar status setting changed", { { "setting", name } });
        _RecomputeAllNodeStates();
    }
}

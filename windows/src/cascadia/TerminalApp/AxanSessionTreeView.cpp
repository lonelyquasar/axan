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
#include "SessionNodeViewModel.h"
#include <AxanIconRegistry.h>

#include <shlobj.h>
#include <array>
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
            const auto titleToken = tab.PropertyChanged([recompute](auto&&, const WUX::Data::PropertyChangedEventArgs& e) {
                if (e.PropertyName() == L"Title")
                {
                    recompute();
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
                [hookActiveControlOutputIdle, recompute](auto&&, auto&&) {
                    hookActiveControlOutputIdle();
                    recompute(); // the newly-active pane's title/cwd are the label's inputs now
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
            if (!_nodeVM(node))
            {
                return; // unknown node — don't touch it
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
        Axan::Log::Debug("TerminalPage", "refresh node theme brushes", { { "isDark", isDark ? "1" : "0" }, { "themeOverride", std::to_string(static_cast<int32_t>(themeOverride)) } });
        const auto col = isDark ? kDefaultLabelColorDark : kDefaultLabelColorLight;
        const WUX::Media::SolidColorBrush brush{ col };
        // Pass isDark so a node's stored color NAME re-resolves to the right palette set; this
        // path runs on every theme switch (_updateThemeColors), so colors retrack the theme.
        _forEachSessionNode(SessionTree().RootNodes(), [&brush, isDark](const MUX::Controls::TreeViewNode& node) {
            if (const auto vm = _nodeVM(node))
            {
                vm->SetDefaultTextBrush(brush, isDark);
            }
        });
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
        _SelectTabForNodeVM(node.Content().try_as<winrt::TerminalApp::SessionNodeViewModel>());
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
        {
            auto mi = makeItem(RS_(L"AxanMenuOpenProfileSettings"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_OpenNodeProfileSettings(node); });
            flyout.Items().Append(mi);
        }
        {
            auto mi = makeItem(RS_(L"AxanMenuCloseSession"), L"");
            mi.Click([node, weakThis{ get_weak() }](auto&&, auto&&) { if (auto p{ weakThis.get() }) p->_CloseNodeSession(node); });
            flyout.Items().Append(mi);
        }

        _sessionNodeMenu = flyout;
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
        _sessionNodeMenu = flyout;
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
        addSessionItem(RS_(L"AxanMenuOpenProfileSettings"), L"", &TerminalPage::_OpenNodeProfileSettings);
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
        if (!vm)
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
    // and wire the _nodeEdit* fields.

    // A bold section caption ("Icon", "Color").
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

    // The "?" button whose flyout documents the label-template variables.
    static Button _buildNodeEditHelpButton()
    {
        Button helpBtn{};
        helpBtn.Content(winrt::box_value(winrt::hstring{ L"?" }));
        helpBtn.Width(22);
        helpBtn.Height(22);
        helpBtn.Padding(WUX::Thickness{ 0, 0, 0, 0 });
        // axan #429: "?" alone is a useless Narrator name.
        Automation::AutomationProperties::SetName(helpBtn, L"Session name variables help");
        {
            const auto varRow = [](const wchar_t* code, const wchar_t* desc) {
                StackPanel row{};
                row.Orientation(Orientation::Horizontal);
                row.Spacing(8);
                TextBlock c{};
                c.Text(code);
                c.FontFamily(WUX::Media::FontFamily{ L"Cascadia Mono, Consolas, monospace" });
                c.Width(92);
                TextBlock d{};
                d.Text(desc);
                d.TextWrapping(WUX::TextWrapping::Wrap);
                row.Children().Append(c);
                row.Children().Append(d);
                return row;
            };
            StackPanel vars{};
            vars.Spacing(3);
            vars.MaxWidth(330);
            TextBlock head{};
            head.Text(L"Session name variables — type any of these and axan substitutes the live value; mix with plain text.");
            head.TextWrapping(WUX::TextWrapping::Wrap);
            head.Margin(WUX::Thickness{ 0, 0, 0, 6 });
            vars.Children().Append(head);
            vars.Children().Append(varRow(L"$shell", L"Shell name — pwsh, cmd, bash…"));
            vars.Children().Append(varRow(L"$dir", L"Current folder name only"));
            vars.Children().Append(varRow(L"$cwd  $pwd", L"Full working-directory path"));
            vars.Children().Append(varRow(L"$~", L"Home-relative working directory"));
            vars.Children().Append(varRow(L"$branch", L"Active git branch, if any"));
            vars.Children().Append(varRow(L"$repo", L"Git repository name"));
            vars.Children().Append(varRow(L"$user", L"Logged-in user"));
            vars.Children().Append(varRow(L"$host", L"Machine hostname"));
            vars.Children().Append(varRow(L"$title", L"Terminal title (OSC 0/2)"));
            vars.Children().Append(varRow(L"$cmd", L"Foreground command"));
            vars.Children().Append(varRow(L"${file:path}", L"First line of the file at path"));
            TextBlock eg{};
            eg.Text(L"Example:  $dir — $shell  →  axan — pwsh");
            eg.Margin(WUX::Thickness{ 0, 8, 0, 0 });
            eg.FontFamily(WUX::Media::FontFamily{ L"Cascadia Mono, Consolas, monospace" });
            vars.Children().Append(eg);
            Flyout helpFlyout{};
            helpFlyout.Content(vars);
            helpBtn.Flyout(helpFlyout);
        }
        return helpBtn;
    }

    // The name section: the "Session name" caption with its "?" help popover, then the
    // [icon preview | Session name field] row. Stores _nodeEditPreview / _nodeEditLabelBox.
    // (Escape handling lives on the card content — #436 item 4 — not on the TextBox.)
    StackPanel TerminalPage::_BuildNodeEditNameSection(const winrt::hstring& currentTemplate,
                                                       const WUX::Media::SolidColorBrush& neutralBrush)
    {
        StackPanel nameLabelRow{};
        nameLabelRow.Orientation(Orientation::Horizontal);
        nameLabelRow.Spacing(6);
        nameLabelRow.Margin(WUX::Thickness{ 0, 0, 0, 5 });
        {
            TextBlock t{};
            t.Text(L"Session name");
            t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            t.VerticalAlignment(WUX::VerticalAlignment::Center);
            nameLabelRow.Children().Append(t);
        }
        nameLabelRow.Children().Append(_buildNodeEditHelpButton());

        // [icon preview | Session name field]
        ContentPresenter preview{};
        preview.HorizontalAlignment(WUX::HorizontalAlignment::Center);
        preview.VerticalAlignment(WUX::VerticalAlignment::Center);
        _nodeEditPreview = preview;
        Border previewBox{};
        previewBox.Width(40);
        previewBox.Height(40);
        previewBox.CornerRadius(WUX::CornerRadius{ 5, 5, 5, 5 });
        previewBox.BorderThickness(WUX::Thickness{ 1, 1, 1, 1 });
        previewBox.BorderBrush(neutralBrush);
        previewBox.Child(preview);

        TextBox labelBox{};
        labelBox.Text(currentTemplate);
        labelBox.PlaceholderText(L"$dir — $shell");
        labelBox.AcceptsReturn(false);
        labelBox.Height(40);
        labelBox.VerticalContentAlignment(WUX::VerticalAlignment::Center);
        _nodeEditLabelBox = labelBox;

        Grid nameRow{};
        {
            ColumnDefinition c0{};
            c0.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c1{};
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            nameRow.ColumnDefinitions().Append(c0);
            nameRow.ColumnDefinitions().Append(c1);
            nameRow.ColumnSpacing(8);
            Grid::SetColumn(previewBox, 0);
            Grid::SetColumn(labelBox, 1);
            nameRow.Children().Append(previewBox);
            nameRow.Children().Append(labelBox);
        }

        // Spacing(2) matches the assembled card's own child spacing, so wrapping the two
        // rows in this section panel is layout-neutral.
        StackPanel section{};
        section.Spacing(2);
        section.Children().Append(nameLabelRow);
        section.Children().Append(nameRow);
        return section;
    }

    // The icon row: one pickable button per registry builtin, plus "Browse to icon file…".
    Grid TerminalPage::_BuildNodeEditIconRow(const WUX::Media::SolidColorBrush& accentBrush,
                                             const WUX::Media::SolidColorBrush& neutralBrush)
    {
        auto iconBtns = std::make_shared<std::vector<Button>>();
        const auto selectIconButton = [iconBtns, accentBrush, neutralBrush](const Button& chosen) {
            for (const auto& b : *iconBtns)
            {
                const bool sel = (b == chosen);
                b.BorderBrush(sel ? accentBrush : neutralBrush);
                b.BorderThickness(sel ? WUX::Thickness{ 2, 2, 2, 2 } : WUX::Thickness{ 1, 1, 1, 1 });
            }
        };
        // axan #10: a horizontal StackPanel clipped everything past ~6 buttons at the card's
        // width. Wrap into a fixed 6-per-row grid instead (the same shape as the startup
        // page's picker), led by a "No icon" cell that clears the override back to the
        // session's own resolved icon.
        VariableSizedWrapGrid iconBtnRow{};
        iconBtnRow.Orientation(Orientation::Horizontal);
        iconBtnRow.MaximumRowsOrColumns(6);
        iconBtnRow.ItemWidth(42);
        iconBtnRow.ItemHeight(42);
        // axan #436 item 1: the picker offers exactly AxanIconRegistry's builtins (all 11),
        // so every pickable glyph round-trips to a portable `builtin:NAME` token on TOML
        // export. (The old hardcoded 8-glyph row had five glyphs outside the registry, which
        // exported as raw PUA chars that Linux renders as nothing.)
        const auto addIconCell = [&](const winrt::hstring& glyph, const winrt::hstring& name, const winrt::hstring& storedValue) {
            Button b{};
            b.Width(36);
            b.Height(36);
            b.Padding(WUX::Thickness{ 0, 0, 0, 0 });
            b.BorderThickness(WUX::Thickness{ 1, 1, 1, 1 });
            b.BorderBrush(neutralBrush);
            // axan #429/#436: the picker buttons are icon-only (a bare Segoe glyph reads as
            // nothing or as a codepoint); the registry's portable name names them for
            // Narrator (and a tooltip) — better than the old positional "Icon option N".
            Automation::AutomationProperties::SetName(b, winrt::hstring{ L"Icon: " + std::wstring{ name } });
            WUX::Controls::ToolTipService::SetToolTip(b, winrt::box_value(name));
            FontIcon fi{};
            fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
            fi.Glyph(glyph);
            b.Content(fi);
            iconBtns->push_back(b);
            b.Click([weakThis{ get_weak() }, storedValue, b, selectIconButton](auto&&, auto&&) {
                if (auto p{ weakThis.get() })
                {
                    p->_nodeEditIconOverride = storedValue;
                    selectIconButton(b);
                    p->_UpdateNodeEditPreview();
                }
            });
            if (_nodeEditIconOverride == storedValue)
            {
                b.BorderBrush(accentBrush);
                b.BorderThickness(WUX::Thickness{ 2, 2, 2, 2 });
            }
            iconBtnRow.Children().Append(b);
        };
        addIconCell(L"", L"No icon", L"" /* clear -> the session's own icon */);
        for (const auto& builtin : Axan::IconRegistry::Builtins())
        {
            const winrt::hstring glyph{ std::wstring(1, builtin.glyph) };
            addIconCell(glyph, winrt::hstring{ builtin.name }, glyph);
        }
        Button browseBtn{};
        {
            StackPanel bc{};
            bc.Orientation(Orientation::Horizontal);
            bc.Spacing(7);
            FontIcon fi{};
            fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
            fi.Glyph(L"");
            fi.FontSize(14);
            TextBlock t{};
            t.Text(L"Browse to icon file…");
            bc.Children().Append(fi);
            bc.Children().Append(t);
            browseBtn.Content(bc);
        }
        // axan #429: the StackPanel content yields no automatic UIA name.
        Automation::AutomationProperties::SetName(browseBtn, L"Browse to icon file");
        browseBtn.VerticalAlignment(WUX::VerticalAlignment::Center);
        browseBtn.Click([weakThis{ get_weak() }, selectIconButton](auto&&, auto&&) {
            if (auto p{ weakThis.get() })
            {
                selectIconButton(Button{ nullptr });
                p->_BrowseNodeIconFile();
            }
        });
        Grid iconRow{};
        {
            ColumnDefinition c0{};
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition c1{};
            c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            iconRow.ColumnDefinitions().Append(c0);
            iconRow.ColumnDefinitions().Append(c1);
            iconRow.ColumnSpacing(8);
            Grid::SetColumn(iconBtnRow, 0);
            Grid::SetColumn(browseBtn, 1);
            iconRow.Children().Append(iconBtnRow);
            iconRow.Children().Append(browseBtn);
        }
        return iconRow;
    }

    // The color swatch row.
    StackPanel TerminalPage::_BuildNodeEditSwatchRow(const WUX::Media::SolidColorBrush& accentBrush,
                                                     const WUX::Media::SolidColorBrush& neutralBrush)
    {
        auto swatchEls = std::make_shared<std::vector<std::pair<winrt::hstring, Border>>>();
        const auto selectSwatch = [swatchEls, accentBrush, neutralBrush](const winrt::hstring& token) {
            for (const auto& entry : *swatchEls)
            {
                const bool sel = (entry.first == token);
                entry.second.BorderBrush(sel ? accentBrush : neutralBrush);
                entry.second.BorderThickness(sel ? WUX::Thickness{ 2, 2, 2, 2 } : WUX::Thickness{ 1, 1, 1, 1 });
            }
        };
        StackPanel swatchRow{};
        swatchRow.Orientation(Orientation::Horizontal);
        swatchRow.Spacing(8);
        // axan #422: a swatch STORES a semantic palette name ("blue") — the cross-OS-portable
        // token — and FILLS with that name resolved to the active theme's hex (WYSIWYG). The
        // names match SessionNodeViewModel's palette + the Linux set. First entry is "no color".
        struct Swatch
        {
            const wchar_t* name;
            bool none;
        };
        static constexpr std::array<Swatch, 7> swatches{ {
            { L"", true },
            { L"red", false },
            { L"orange", false },
            { L"yellow", false },
            { L"green", false },
            { L"blue", false },
            { L"purple", false } } };
        for (const auto& sw : swatches)
        {
            Border el{};
            el.Width(34);
            el.Height(34);
            el.CornerRadius(WUX::CornerRadius{ 5, 5, 5, 5 });
            el.BorderThickness(WUX::Thickness{ 1, 1, 1, 1 });
            el.BorderBrush(neutralBrush);
            const winrt::hstring token{ sw.name };
            if (sw.none)
            {
                FontIcon fi{};
                fi.FontFamily(WUX::Media::FontFamily{ L"Segoe MDL2 Assets" });
                fi.Glyph(L"");
                fi.FontSize(14);
                el.Child(fi);
            }
            else if (const auto fill = winrt::TerminalApp::implementation::SessionNodeViewModel::ResolveColorBrush(token, _nodeEditIsDark))
            {
                el.Background(fill);
                WUX::Controls::ToolTipService::SetToolTip(el, winrt::box_value(token));
            }
            swatchEls->push_back({ token, el });
            el.Tapped([weakThis{ get_weak() }, token, selectSwatch](auto&&, auto&&) {
                if (auto p{ weakThis.get() })
                {
                    p->_nodeEditColor = token;
                    selectSwatch(token);
                    p->_UpdateNodeEditPreview();
                }
            });
            if (_nodeEditColor == token)
            {
                el.BorderBrush(accentBrush);
                el.BorderThickness(WUX::Thickness{ 2, 2, 2, 2 });
            }
            swatchRow.Children().Append(el);
        }
        return swatchRow;
    }

    // The "Apply color to" section: caption + hint, then the Icon/Text/Both radio row.
    StackPanel TerminalPage::_BuildNodeEditApplyColorSection()
    {
        const auto makeRadio = [this](const wchar_t* text, const wchar_t* val) {
            RadioButton rb{};
            rb.Content(winrt::box_value(winrt::hstring{ text }));
            rb.GroupName(L"axanApplyColorTo");
            const winrt::hstring v{ val };
            rb.IsChecked(_nodeEditColorTarget == v);
            rb.Checked([weakThis{ get_weak() }, v](auto&&, auto&&) {
                if (auto p{ weakThis.get() })
                {
                    p->_nodeEditColorTarget = v;
                    p->_UpdateNodeEditPreview();
                }
            });
            return rb;
        };
        StackPanel applyRow{};
        applyRow.Orientation(Orientation::Horizontal);
        applyRow.Spacing(18);
        applyRow.Children().Append(makeRadio(L"Icon", L"icon"));
        applyRow.Children().Append(makeRadio(L"Text", L"text"));
        applyRow.Children().Append(makeRadio(L"Both", L"both"));

        // Apply-to label + hint.
        StackPanel applyLabelRow{};
        applyLabelRow.Orientation(Orientation::Horizontal);
        applyLabelRow.Spacing(6);
        applyLabelRow.Margin(WUX::Thickness{ 0, 10, 0, 4 });
        {
            TextBlock t{};
            t.Text(L"Apply color to");
            t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            t.VerticalAlignment(WUX::VerticalAlignment::Bottom);
            TextBlock h{};
            h.Text(L"recolors glyph icons; image files keep their own colors");
            h.Opacity(0.6);
            h.VerticalAlignment(WUX::VerticalAlignment::Bottom);
            h.TextWrapping(WUX::TextWrapping::Wrap);
            applyLabelRow.Children().Append(t);
            applyLabelRow.Children().Append(h);
        }

        // Spacing(2) matches the assembled card's own child spacing — layout-neutral wrap.
        StackPanel section{};
        section.Spacing(2);
        section.Children().Append(applyLabelRow);
        section.Children().Append(applyRow);
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
        if (!vm)
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
        // Capture the sidebar's resolved theme so the swatches + preview resolve color NAMES
        // against the matching palette set (the same derivation _RefreshNodeLabelBrushes uses).
        _nodeEditIsDark = _SidebarIsDarkTheme();

        // Accent + neutral brushes for selection highlights.
        winrt::Windows::UI::Color accentColor{ 255, 0x4c, 0xc2, 0xff };
        if (const auto a = Application::Current().Resources().TryLookup(winrt::box_value(winrt::hstring{ L"SystemAccentColor" })))
        {
            accentColor = winrt::unbox_value_or<winrt::Windows::UI::Color>(a, accentColor);
        }
        const WUX::Media::SolidColorBrush accentBrush{ accentColor };
        const WUX::Media::SolidColorBrush neutralBrush{ winrt::Windows::UI::Color{ 0x40, 0x80, 0x80, 0x80 } };

        // --- assemble ---
        StackPanel content{};
        content.Spacing(2);
        content.MinWidth(420);
        content.Children().Append(_buildNodeEditTitle());
        content.Children().Append(_BuildNodeEditNameSection(vm->LabelTemplate, neutralBrush));
        content.Children().Append(_buildNodeEditSectionLabel(L"Icon"));
        content.Children().Append(_BuildNodeEditIconRow(accentBrush, neutralBrush));
        content.Children().Append(_buildNodeEditSectionLabel(L"Color"));
        content.Children().Append(_BuildNodeEditSwatchRow(accentBrush, neutralBrush));
        content.Children().Append(_BuildNodeEditApplyColorSection());
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

        card.Child(content);

        _UpdateNodeEditPreview();
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

    // axan M13: rebuild the editor's live icon preview from the current icon choice + recolor,
    // via the same BuildIcon the sidebar row uses. The preview tints only when the target paints
    // the icon (so it matches the realized row).
    void TerminalPage::_UpdateNodeEditPreview()
    {
        if (!_nodeEditPreview)
        {
            return;
        }
        const auto path = _nodeEditIconOverride.empty() ? _nodeEditFallbackPath : _nodeEditIconOverride;
        const auto effColor = (_nodeEditColorTarget != L"text") ? _nodeEditColor : winrt::hstring{};
        auto el = winrt::TerminalApp::implementation::SessionNodeViewModel::BuildIcon(path, effColor, _nodeEditIsDark);
        if (el)
        {
            el.Width(24);
            el.Height(24);
        }
        _nodeEditPreview.Content(el);
    }

    // axan M13 (design handoff): "Browse to icon file…" — pick an image/exe and set it as the
    // editor's current icon override; the preview updates. Mirrors WT's profile icon picker.
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
        if (!file.empty())
        {
            _nodeEditIconOverride = file;
            _UpdateNodeEditPreview();
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
        _nodeEditPreview = nullptr;
        _nodeEditIconOverride = {};
        _nodeEditColor = {};
        _nodeEditColorTarget = L"both";
        _nodeEditFallbackPath = {};
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

    // axan M13: open WT's Settings UI for the node's profile. Deep-linking to the specific
    // profile page isn't exposed by SettingsTarget, so we open the editor and let the user
    // navigate — the profile is WT-owned (Divergence 1). A deep link is a future enhancement.
    void TerminalPage::_OpenNodeProfileSettings(const MUX::Controls::TreeViewNode& /*node*/)
    {
        Axan::Log::Info("TerminalPage", "opening WT Settings for node profile");
        _LaunchSettings(Settings::Model::SettingsTarget::SettingsUI);
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
        if (!node && tree.RootNodes().Size() > 0)
        {
            node = tree.RootNodes().GetAt(0);
        }
        if (node)
        {
            // A sidebar expanded just above may not have realized row containers yet.
            tree.UpdateLayout();
            if (const auto item = tree.ContainerFromNode(node).try_as<MUX::Controls::TreeViewItem>())
            {
                item.Focus(FocusState::Keyboard);
                return;
            }
        }
        // No rows (or no realized container) — at least land focus on the tree control.
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
    }
}

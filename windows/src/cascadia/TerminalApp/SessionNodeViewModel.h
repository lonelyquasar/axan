// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

#include "SessionNodeViewModel.g.h"

#include <atomic> // axan #430: LabelGeneration
#include <functional> // axan #427: type-erased recompute unsubscribers

namespace winrt::TerminalApp::implementation
{
    // axan M6: see SessionNodeViewModel.idl. Label is the projected, observable display
    // text. TabRef + LabelTemplate are impl-only identity payload reached via
    // winrt::get_self from the tree plumbing (TabManagement.cpp) — deliberately not in the
    // IDL, since they're C++-internal bookkeeping, not anything XAML or other projections
    // consume. TabRef is type-erased (weak_ref<IInspectable>) so this header needn't pull
    // in the Tab projection; the consumer try_as<TerminalApp::Tab>()s it back.
    //
    // axan #429: also IStringable. The sidebar TreeView is unbound, so each TreeViewItem's
    // UIA name resolves through WinUI 2.8's TreeViewItemAutomationPeer::GetNameCore, whose
    // fallback is SharedHelpers::TryGetStringRepresentationFromObject(node.Content()) — an
    // IStringable::ToString probe. Returning the computed Label there gives every row a
    // Narrator name that's evaluated fresh on each query, so it can never go stale through
    // container recycling or the async label recompute. (The system XAML data-item peers use
    // the same string-representation probe, which also names the minimized icon list's rows.)
    struct SessionNodeViewModel : SessionNodeViewModelT<SessionNodeViewModel, winrt::Windows::Foundation::IStringable>
    {
        SessionNodeViewModel() = default;

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Label, PropertyChanged.raise);

    public:
        // axan #429: IStringable — the UIA/Narrator name of any row displaying this node
        // (see the struct comment). Kept in lockstep with the displayed text by definition.
        // axan #3: a separator has no label of its own; Narrator reads it as "Separator".
        hstring ToString() const { return _isSeparator ? hstring{ L"Separator" } : _Label; }

        // axan #3: separator rows (see the idl). The projected getters feed the shared
        // ItemTemplate's x:Binds; ConfigureSeparator (impl-only, called once by
        // TerminalPage::_CreateSeparatorNode) flips the node into separator mode with its
        // normalized style/height/placement and the pixel height of one session row.
        bool IsSeparator() const noexcept { return _isSeparator; }
        hstring SeparatorStyle() const noexcept { return _separatorStyle; }
        double SeparatorHeight() const noexcept { return _separatorHeight; }
        hstring Placement() const noexcept { return _placement; }
        winrt::Windows::UI::Xaml::Visibility SessionVisibility() const noexcept;
        winrt::Windows::UI::Xaml::Visibility SeparatorVisibility() const noexcept;
        winrt::Windows::UI::Xaml::Visibility SeparatorLineVisibility() const noexcept;
        double SeparatorPixelHeight() const noexcept { return _separatorHeight * _rowHeightPx; }
        void ConfigureSeparator(const hstring& style, double height, const hstring& placement, double rowHeightPx);

        // WINRT_OBSERVABLE_PROPERTY leaves the access region non-public; restore it so the
        // tree plumbing (via winrt::get_self) can reach the impl-only identity payload below.
    public:
        // axan M7: the node's icon. Built FRESH on every get (mirrors WT's
        // BasePaletteItem::ResolvedIcon) — never cached. A cached IconElement is a UIElement;
        // binding the same instance into the node's TreeViewItem as it's recycled during a
        // drag reparent throws "element already has a parent" (Windows.UI.Xaml.dll 0xc000027b)
        // and crashes. So we store only the resolved path and mint a new IconElement here per
        // realization; IconPath(...) stores the path and raises PropertyChanged("Icon").
        winrt::Windows::UI::Xaml::Controls::IconElement Icon();
        // axan M13 (design handoff): the label-text brush — the recolor when it targets text
        // (Apply color to = Text/Both), else the default theme text brush. Bound by the tree
        // ItemTemplate's TextBlock Foreground.
        winrt::Windows::UI::Xaml::Media::Brush LabelBrush();
        // axan M13/#422: the page sets the theme-correct default text brush (derived from the
        // sidebar's ActualTheme — white on dark, near-black on light) AND whether that theme is
        // dark. Re-raises LabelBrush and Icon: a per-node color is stored as a palette NAME, so a
        // theme switch re-resolves it to the other set's hex and both the glyph tint and the label
        // must repaint. Used for every node; refreshed on a theme switch.
        void SetDefaultTextBrush(const winrt::Windows::UI::Xaml::Media::Brush& brush, bool isDarkTheme);
        // axan #422: resolve a stored color token to a concrete "#RRGGBB" for the given theme:
        // "" -> "" (no recolor); "#hex" -> verbatim (theme-static escape hatch); a palette name
        // ("red".."purple", case-insensitive) -> that hue's hex in the active-theme set; an
        // unknown name -> "" (no recolor). Shared with the sidebar node editor so its swatches
        // fill with the same theme-resolved hex (WYSIWYG). Mirrors Linux sidebar_resolve_color_token.
        static winrt::hstring ResolveColorToken(const winrt::hstring& token, bool isDarkTheme);
        // axan #422: a SolidColorBrush for a stored token in the given theme, or nullptr when the
        // token resolves to no recolor. Lets the sidebar editor fill each swatch with the exact
        // hex the row will render (WYSIWYG) without duplicating the hex parser.
        static winrt::Windows::UI::Xaml::Media::Brush ResolveColorBrush(const winrt::hstring& token, bool isDarkTheme);
        // axan M8: derive a <=4-char abbreviation from a display name for the minimized cells.
        static winrt::hstring Initials(const winrt::hstring& label);
        void IconPath(const winrt::hstring& value);
        winrt::hstring IconPath() const noexcept { return _iconPath; }
        // axan M13: set the resolved icon path, the recolor, AND the recolor target in one shot,
        // always raising PropertyChanged("Icon") + ("LabelBrush") so a color/target-only change
        // (same path) still refreshes the realized row. The recolor (icon_color,
        // "#RRGGBB"/"#AARRGGBB", "" -> default) tints the icon glyph and/or the label text per
        // the target ("icon" / "text" / "both"); an image icon keeps its own pixels.
        void ApplyAppearance(const winrt::hstring& path, const winrt::hstring& color, const winrt::hstring& target);
        winrt::hstring IconColor() const noexcept { return _iconColor; }
        // axan M13: which surfaces the recolor paints: "icon", "text", or "both" (default).
        winrt::hstring ColorTarget{ L"both" };
        // axan M13: build a (recolored) IconElement from an already-resolved icon path. Shared
        // by Icon() and the picker's live preview so both render identically. IconWUX returns an
        // IconSourceElement wrapping a FontIconSource (glyph) or an ImageIcon (binary) — never a
        // FontIcon — so the recolor tints the FontIconSource's Foreground (image icons keep their
        // pixels). Caller sets the size. Empty path -> nullptr.
        static winrt::Windows::UI::Xaml::Controls::IconElement BuildIcon(const winrt::hstring& path, const winrt::hstring& color, bool isDarkTheme);
        // The session this node stands for. Weak so a node can never keep a closed tab
        // alive; the tree prunes nodes whose tab has gone (self-healing, M6).
        winrt::weak_ref<winrt::Windows::Foundation::IInspectable> TabRef{ nullptr };
        // The node's M5 label template ("" -> default title/cwd label). Lives here now
        // instead of a parallel vector, so it rides along through reorder/reparent and the
        // save round-trip reads it straight off the node.
        winrt::hstring LabelTemplate;
        // axan #422/#427: the Id of the global StartupSessions entry this node spawned from, or
        // "" for a runtime-created session that has no curated entry. The node editor's Save
        // writes its label/icon/color/target back to the matching entry by this Id; a node with
        // no EntryId is live-only (not persisted) — preserving the curated-startup vs.
        // currently-open split.
        winrt::hstring EntryId;
        // axan M7: the per-node icon override path ("" -> inherit the profile icon, else an
        // auto-assigned builtin glyph). The first link in the override -> profile -> auto
        // resolution chain. Empty for every node until an override picker/config round-trip
        // lands (deferred); kept here now so the VM is the icon's single home, mirroring
        // LabelTemplate, and the resolution helper can read it uniformly.
        winrt::hstring IconOverride;
        // axan #427 item 3: revoke-on-prune plumbing for the live label recompute. The tree
        // plumbing (TabManagement.cpp _CreateSessionNode) subscribes the node's recompute to
        // the tab's Title PropertyChanged, the active control's OutputIdle, and the tab's
        // ActivePaneChanged, and stores type-erased unsubscribers here — type-erased
        // (std::function over weak refs + tokens) so this header stays free of the
        // Tab/TermControl projections, matching TabRef. RevokeRecomputeSubscriptions runs when
        // the node is pruned (_RemoveNodeSelfHealing) so a node re-minted for a still-live tab
        // can't stack duplicate handlers; the destructor is the backstop for any other teardown.
        std::function<void()> RevokeTitleSubscription;
        std::function<void()> RevokeOutputIdleSubscription;
        std::function<void()> RevokeActivePaneSubscription;
        void RevokeRecomputeSubscriptions();
        ~SessionNodeViewModel();
        // axan #430 item 1: generation stamp for the async label recompute. Bumped on the UI
        // thread when a recompute is dispatched; the background expansion's result is applied
        // only if its generation is still current, so a stale (slower) expansion can never
        // overwrite a newer label.
        std::atomic<uint64_t> LabelGeneration{ 0 };

    private:
        // axan M7: the resolved icon path/glyph (override -> profile -> auto), set by the
        // tree plumbing via IconPath(...). Icon() renders a fresh element from it each get.
        winrt::hstring _iconPath;
        // axan M13: the per-node recolor ("#RRGGBB"/"#AARRGGBB", "" -> theme foreground),
        // applied to a glyph FontIcon's Foreground in Icon(). Persisted as icon_color.
        winrt::hstring _iconColor;
        // axan M13: the theme-correct default label brush the page pushes (from the sidebar's
        // ActualTheme). Used by LabelBrush when the node's recolor doesn't target the text.
        winrt::Windows::UI::Xaml::Media::Brush _defaultTextBrush{ nullptr };
        // axan #422: whether the sidebar's resolved theme is dark, pushed alongside the default
        // brush. Drives which palette set a stored color NAME resolves against. Defaults dark to
        // match _defaultTextBrush's white-on-dark fallback before the page pushes the real theme.
        bool _isDarkTheme{ true };
        // axan #3: separator state (see ConfigureSeparator). _rowHeightPx is the resolved
        // TreeViewItemMinHeight the page passes in, so SeparatorPixelHeight needs no XAML lookup.
        bool _isSeparator{ false };
        hstring _separatorStyle{ L"line" };
        double _separatorHeight{ 1.0 };
        hstring _placement{ L"inline" };
        double _rowHeightPx{ 32.0 };
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(SessionNodeViewModel);
}

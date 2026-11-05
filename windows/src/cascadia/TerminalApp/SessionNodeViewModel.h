// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

#include "SessionNodeViewModel.g.h"

#include <atomic> // axan #430: LabelGeneration
#include <functional> // axan #427: type-erased recompute unsubscribers
#include <unordered_map> // axan #30: SeenRevisions

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
    // axan: the resting details of a node's state (AxanSessionState.h Result), pushed with
    // the state itself. retainedWhat is "done" / "error" when a seen report is kept on the
    // row (sidebar.statusAfterSeen = keep), else "". reportKey identifies the done/error
    // report the age counts from ("" = none). showPresence = an OSC 7501 program lives in
    // the session AND sidebar.showAgentPresence is on.
    struct SessionRestingState
    {
        hstring retainedWhat;
        hstring retainedMessage;
        hstring reportKey;
        bool showPresence{ false };
    };

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
        // axan #33: a labelled header is never shorter than its label needs.
        double SeparatorPixelHeight() const noexcept { return std::max(_separatorHeight * _rowHeightPx, _separatorLabel.empty() ? 0.0 : 24.0); }
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
        // must repaint. Used for every node; refreshed on a theme switch. axan #26: no-op when
        // neither the color nor isDarkTheme changed; returns whether anything was raised.
        bool SetDefaultTextBrush(const winrt::Windows::UI::Xaml::Media::Brush& brush, bool isDarkTheme);
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
        // axan #30: the tab's AxanSessionStateChanged subscription (same revoke-on-prune rule).
        std::function<void()> RevokeSessionStateSubscription;
        void RevokeRecomputeSubscriptions();
        ~SessionNodeViewModel();

        // axan #30: session state (see the idl and AxanSessionState.h). SetSessionState is
        // called by TerminalPage on the UI thread; it raises PropertyChanged only for what
        // changed and restarts StateSince only when the state or kind changes (a message
        // update keeps the age). Returns true when the state or kind changed.
        hstring State() const noexcept { return _state; }
        winrt::Windows::Foundation::DateTime StateSince() const noexcept { return _stateSince; }
        hstring StateMessage() const noexcept { return _stateMessage; }
        hstring StateKind() const noexcept { return _stateKind; }
        winrt::Windows::Foundation::DateTime LastActivity() const noexcept { return _lastActivity; }
        bool SetSessionState(const hstring& state, const hstring& message, const hstring& kind, winrt::Windows::Foundation::DateTime now, const SessionRestingState& resting = {});
        void SetLastActivity(winrt::Windows::Foundation::DateTime value);

        // axan #32: row presentation of the state (see the idl).
        winrt::Windows::UI::Xaml::Visibility StateDotVisibility() const noexcept;
        winrt::Windows::UI::Xaml::Media::Brush StateDotFill() const;
        winrt::Windows::UI::Xaml::Media::Brush StateDotStroke() const;
        hstring StateGlyph() const noexcept;
        winrt::Windows::UI::Xaml::Visibility StateGlyphVisibility() const noexcept;
        hstring SubLabel() const noexcept { return _subLabel; }
        winrt::Windows::UI::Xaml::Visibility SubLabelVisibility() const noexcept;
        // axan: a kept (seen) report renders more muted than a live one.
        double SubLabelOpacity() const noexcept { return ShowsRetained() ? 0.45 : 0.7; }
        bool ShowsRetained() const noexcept { return !_isSeparator && !_retainedWhat.empty(); }
        hstring StateToolTip() const;
        winrt::Windows::UI::Xaml::Visibility StateToolTipVisibility() const noexcept;
        // Recompute the age part of SubLabel. Called by the sidebar's 1s clock; a no-op (no
        // PropertyChanged) unless the visible text changed, and only states that show an
        // age (needs-you / done-unseen / error) ever change it.
        void RefreshAge(winrt::Windows::Foundation::DateTime now); // also a kept report's age
        bool ShowsAge() const noexcept;
        // "<1m", "4m", "2h", "3d" - the compact age the second line shows.
        static hstring FormatAge(winrt::Windows::Foundation::TimeSpan age);

        // axan #31: idle de-emphasis (see the idl).
        bool IsDeEmphasised() const noexcept { return _deEmphasised; }
        double LabelFontSize() const noexcept { return _deEmphasised ? 12.0 : 14.0; }
        double RowOpacity() const noexcept { return _deEmphasised ? 0.55 : 1.0; }
        double RowMinHeight() const noexcept { return _deEmphasised ? std::round(_rowHeightPx * 0.75) : _rowHeightPx; }
        // The full-size row floor (TreeViewItemMinHeight), pushed by the page at creation.
        void SetRowHeightPx(double px);
        // Re-evaluate against the threshold (0 = off). Returns true if the flag flipped.
        bool UpdateDeEmphasis(winrt::Windows::Foundation::DateTime now, int32_t idleAfterMinutes);

        // axan #33: section headers (see the idl).
        hstring SeparatorLabel() const noexcept { return _separatorLabel; }
        bool HasSectionHeader() const noexcept { return _isSeparator && !_separatorLabel.empty(); }
        winrt::Windows::UI::Xaml::Visibility SeparatorHeaderVisibility() const noexcept;
        winrt::Windows::UI::Xaml::Visibility SeparatorHeaderLineVisibility() const noexcept;
        bool IsCollapsed() const noexcept { return _collapsed; }
        hstring CollapseGlyph() const noexcept;
        hstring SectionSummary() const noexcept { return _sectionSummary; }
        hstring SectionBadge() const noexcept { return _sectionBadge; }
        winrt::Windows::UI::Xaml::Visibility SectionBadgeVisibility() const noexcept;
        void ActivateSectionBadge();
        bool IsSectionHidden() const noexcept { return _sectionHidden; }
        void SetSeparatorLabel(const hstring& label);
        void SetCollapsed(bool collapsed);
        void SetSectionSummary(const hstring& summary, const hstring& badge);
        bool SetSectionHidden(bool hidden); // returns true if it changed
        // Invoked by ActivateSectionBadge; the page sets it on header VMs (expand + select).
        std::function<void()> OnSectionBadgeActivated;
        // axan #30: "seen" bookkeeping. Per TermControl (keyed by its ContentId), the highest
        // OSC 7501 record Revision the user has been shown. A done/error record with a
        // revision at or below this is seen. Written only by TerminalPage's acknowledge
        // path (node selected / pane focused / window activated), never per keystroke.
        std::unordered_map<uint64_t, uint64_t> SeenRevisions;
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
        // axan #30: session state.
        hstring _state{ L"idle" };
        winrt::Windows::Foundation::DateTime _stateSince{};
        hstring _stateMessage;
        hstring _stateKind;
        winrt::Windows::Foundation::DateTime _lastActivity{};
        // axan #32
        hstring _subLabel;
        hstring _subLabelMessage;
        // axan: statusAfterSeen = keep / agent presence (see SessionRestingState). _reportSince
        // is when the report named by _reportKey first appeared; it survives acknowledgement,
        // so a kept "done" shows how long ago it finished, not how long ago it was opened.
        hstring _retainedWhat;
        hstring _retainedMessage;
        hstring _reportKey;
        winrt::Windows::Foundation::DateTime _reportSince{};
        bool _showPresence{ false };
        // axan #31
        bool _deEmphasised{ false };
        // axan #33
        hstring _separatorLabel;
        bool _collapsed{ false };
        bool _sectionHidden{ false };
        hstring _sectionSummary;
        hstring _sectionBadge;
        void _setDeEmphasised(bool value);
        winrt::Windows::UI::Color _textColor() const;
        hstring _computeSubLabel(winrt::Windows::Foundation::DateTime now) const;
        void _raiseStatePresentation();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(SessionNodeViewModel);
}

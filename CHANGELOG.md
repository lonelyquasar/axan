# Changelog

All notable changes to axan are documented here.

## [Unreleased]

### Session state in the sidebar

- The terminal now implements the OSC 7501 Program Status Protocol: it answers
  the `ESC ] 7501 ; ? ESC \` query and keeps per-pane records from programs
  that report `idle`, `working`, `done`, `blocked` or `error` (such as Claude
  Code). Windows only. (#29)
- Sidebar rows show session state: a dot on the icon, a second line (`kind ·
  age` or `message · age`; a long message is trimmed to the sidebar's width,
  never the age), a lightning-bolt glyph while working, a play glyph
  while a shell command runs (OSC 133), and a tooltip with the state and when
  it began. A done or error report counts as unread until you open the tab.
  Windows only. (#30, #32)
- `sidebar.statusAfterSeen` (`keep` by default, or `clear`) sets whether an
  opened tab keeps its last report as a muted line. A robot glyph at the right
  edge marks rows where a status-reporting program is present and at rest, and
  `sidebar.stateGlyphs` customizes the per-state glyphs (including separate
  ones for a blocked report waiting on permission, an answer or a login).
  Windows only.
- The right-edge slot also shows a small state word (`idle`, `working`,
  `needs answer`, `needs permission`, ...; `sidebar.stateLabels`, default on),
  hovering it explains the state in one sentence, and screen readers hear the
  state as part of the row's name. Windows only.
- An agent shows its session name as the row's muted second line whenever
  nothing more urgent is there, including while it works without a message
  (the terminal title Claude Code sets from `claude --name`;
  `sidebar.showAgentName`, default on). Windows only.
- Idle rows are de-emphasised after `sidebar.idleAfterMinutes` (default 15;
  `0` turns it off); the focused row never is. `sidebar.idleEffects` picks how:
  `dim`, `smaller` (the default pair), `gray` (a coloured session loses its
  colour) and `collapse` (a session folds its child sessions away once they're
  all idle, and reopens on activity), with a checkbox each on the Sidebar
  settings page. Windows only. (#31)
- The right-click menu on empty sidebar space has a Settings item. Windows only.
- Optional "Needs you" group above the session tree
  (`sidebar.needsYouGroup`, default off) listing sessions that are blocked,
  errored or finished and unseen, longest-waiting first. Windows only. (#34)
- The group's states are configurable: `sidebar.hoistStates` (default
  `["blocked", "error", "done"]`; add `running` to watch long commands), and
  each startup session can follow it, opt out (`"hoist": "never"`) or set its
  own list (the Sidebar settings page for the global list; the session's
  Hoisted group on the Startup sessions page, or the sidebar's node editor, for
  one session). The heading reads "Hoisted" once non-attention states are
  included. Windows only. (#34)
- Separators with a label are now collapsible section headers; a collapsed
  header shows how many sessions it hides and the most urgent state among
  them. Windows only. (#33)
- `tools/axan-status.sh` and `tools/axan-status.ps1` send OSC 7501 reports for
  manual testing, with `--query` and `--demo` modes.

### Settings

- New "Sessions" group in Settings with the Startup sessions page and a new
  Sidebar page. Every sidebar setting moves there from Appearance (sidebar
  matches profile transparency, status after seen, state labels, agent name,
  the hoisted group and its states, idle dimming, the state glyphs); the
  `settings.json` keys are unchanged. `sidebar.needsYouGroup` gets its first
  Settings toggle. Windows only. (#38)
- The Startup sessions page is now a list and a detail panel. The list shows
  each row's nesting, icon, colour and name, with separators as header rows;
  the panel edits the selected row: Identity (the same name, icon and colour
  editor as the sidebar's Edit node card, plus the icon as text), Launch
  (profile, directory, command) and Hoisted group for a session, or a
  separator's label, start collapsed, style, height and placement. A toolbar
  under the list moves, outdents, indents, duplicates and deletes the selected
  row; with the list focused, Alt+Up/Down, Alt+Left/Right, Ctrl+D and Delete do
  the same. Drag a row to reorder it; its children move with it. Narrow windows
  stack the panel under the list. Windows only. (#38)
- The icon picker offers the whole Segoe Fluent Icons font (about 1,400
  glyphs) instead of eleven, with search by name, the current icon and a Clear
  button above the grid. It is the same picker in the Edit node card, the
  Startup sessions panel and the Sidebar page's state glyphs. A picked icon is
  stored by name (`"icon": "robot"`); glyph characters, emoji, image paths and
  `builtin:NAME` keep working. Windows only. (#39)
- The Sidebar page has a "Text and icons" group that sizes the session name
  (`sidebar.labelFontSize`, default 14), the status line and state word
  (`sidebar.stateLineFontSize`, 11), the session icon (`sidebar.iconSize`, 16)
  and the right-edge status glyph (`sidebar.stateGlyphSize`, 12). Windows only.
- Egyptian hieroglyphs and runes used as icons are drawn larger, so they match
  emoji and Segoe Fluent glyphs instead of looking undersized. Windows only.
- A session's right-click menu has a "Preview state" submenu that shows the row
  as if it were in any state (needs answer, done, error, ...), for checking how
  sizes and glyphs look. It is purely visual, marked "Preview" in the tooltip,
  and ends on "Clear preview" or the session's next real report. Windows only.
- "Open profile in Settings" is gone from the titlebar app menu, which already
  has Settings, and from a session's right-click menu, where a plain Settings
  item replaces it. It opened Settings without going to the session's profile.
  Windows only.

### Security

- Cherry-picked upstream Windows Terminal #20409: DRCS soft-font sixel
  writes are bounds-checked (CVE-2026-59117). Windows only. (#25)

### Fixes

- Separators are drop targets during a drag, and the flat list is verified
  after drops. Windows only. (#28)
- Settings reloads coalesce theme refreshes and skip unchanged node brushes,
  and the Startup sessions page no longer does quadratic work on long lists.
  Windows only. (#26, #19)
- Duplicate on the Startup sessions page places the copy after the source's
  children instead of between the source and its children. Windows only. (#38)
- Copying from a text flyout no longer crashes the terminal when the clipboard
  is busy. Windows only.
- The bash shell wrapper no longer reports an idle WSL prompt as a running
  command. Windows only.

## [0.1.2] — 2026-09-01

### Session tree & menus

- Separator rows in the session tree: non-interactive divider rows, drawn as a
  line or as open space, with a height in session-row units and an optional
  pin-to-bottom placement. They are added and edited on the Startup sessions
  settings page (they also travel through `startup-sessions.toml`, which
  becomes schema version 2 once it contains one), and holding Ctrl+Alt unlocks
  them for drag-reordering in the sidebar. Windows only. (#3)
- Dragging a session onto a session that had no children no longer makes the
  dragged session vanish: the drop target is expanded (and its expander
  refreshed) as soon as the drop lands, instead of only once something else
  gave it a child. Windows only. (#16)

### Startup sessions

- Move up/down on the Startup sessions settings page no longer rebuilds every
  row on each click; only the moved rows are re-rendered, which removes the
  visible pause on longer lists and slower machines. Windows only.
- The Startup sessions page uses the available width: the Name, Directory and
  Command boxes grow with the window (up to a wider page cap) instead of
  clipping long paths and commands at a fixed width, and shrink first on a
  narrow window so the row's trailing buttons stay in view. Windows only.

### Appearance

- Themes can now set `sidebar.background` and `content.background` (the same
  value forms as `tabRow.background`, including alpha and `terminalBackground`)
  to make the sidebar and the terminal backdrop translucent or see-through;
  unset keeps today's opaque surfaces. Windows only. (#2)
- New "Sidebar matches profile transparency" toggle on the Appearance settings
  page (`sidebarMatchesProfileTransparency` global, default off): the sidebar
  takes the focused profile's background and opacity and the backdrop behind
  the terminal goes fully clear, so the whole window shares the profile's
  transparency without editing a theme. Overrides the theme keys above while
  on. Windows only. (#2)

### Build

- Every exe and dll in the Windows build now carries a Win32 version resource:
  FileDescription (e.g. "axan Console and PTY Host"), ProductName "axan", and
  FileVersion/ProductVersion read from the package manifest, so Task Manager
  and file properties can identify axan's binaries and which build they are.
  The remaining "Windows Terminal …" descriptions were rebranded to axan. (#9)

## [0.1.1] — 2026-07-11

Windows-only quality-of-life release for session menus and the startup-session
tools; the Linux frontend has no functional changes (version kept in
lockstep).

### Session tree & menus

- "New session" is now a split row everywhere it appears — the titlebar app
  menu, the sidebar node context menu, and empty-sidebar right-click:
  activating the row opens a default-profile session, and hovering (or
  keyboard-expanding) it opens a submenu with one entry per active profile,
  the default in bold. (#12)
- Right-clicking the empty sidebar space below the tree now opens a context
  menu; previously it did nothing. (#12)
- The Edit session node icon picker no longer clips to the first ~6 glyphs:
  the builtin glyphs wrap into rows of six, and a leading "No icon" cell
  clears the override back to the session's own icon. (#10)

### Startup sessions

- Rows on the Startup sessions settings page can be reordered with per-row
  Move up/down — sibling-scoped, carrying the row's whole subtree — and pick
  a session color from the shared theme-adaptive palette, with an
  icon/text/both apply-to choice. (#13)
- "Save current as startup", beside Import from TOML, replaces the list with
  a snapshot of the live session tree — hierarchy, profile, working
  directory, name, icon, and color per session — behind a confirmation
  prompt. Commands aren't recoverable from a live session and are not
  carried over (the same limitation as the Linux "Capture current window").
  (#14)

### Installing (Windows)

Installs over 0.1.0 with no certificate step (same signing certificate). New
machines: see the 0.1.0 instructions below.

## [0.1.0] — 2026-07-02

First installable release: the Windows frontend as a signed sideload MSIX
(x64). The Linux (GTK) frontend is at the same 0.1.0 and builds from source
(root README, Building); no Linux binary asset yet.

axan on Windows is a fork of Windows Terminal 1.24 that replaces the tab strip
with a persistent session tree. It installs side by side with stock Windows
Terminal: its own package identity, its own settings and state, and its own COM
class IDs for default-terminal handoff and the Explorer context-menu extension.

(The D-numbers below tag internal design decisions; they're kept so the
CHANGELOG and code comments stay cross-referenced.)

### The session tree (sidebar)

- The sidebar session tree is the navigator; the tab strip is retired (D20).
- Per-node context menu and an in-tree Edit session node overlay: rename with
  label templates, icon picker with persisted per-node override, per-node color
  from theme-adaptive palettes (8:1 contrast on dark; follows live theme
  switches).
- Node edits, colors, and icons persist across restart; moving a node keeps its
  identity. Label recompute runs off-thread.
- Keyboard and accessibility: `focusSidebar` action (default Ctrl+Shift+Y),
  Narrator-readable row names.
- Clicking a session in the sidebar focuses its terminal (both platforms).
- Titlebar app menu ("axan ▾") with New session, the active session's actions,
  sidebar toggle, Settings, Command palette, and About; the titlebar centers
  the active session's label with the terminal title as secondary text.

### Startup sessions (D19)

- A global startup-session tree replaces per-profile launch entries: each entry
  picks a profile (the shell), an optional directory and command, a name
  template, an icon, and a color; entries nest.
- A dedicated "Startup sessions" settings page edits the tree: profile
  dropdown, builtin-glyph icon picker (with image browse and emoji free-text),
  indent/outdent, duplicate, delete.

### Portable TOML interchange (D18)

- `startup-sessions.toml` is auto-exported on save and importable from the
  settings page (failed imports surface inline and log the reason). Icons
  round-trip as portable `builtin:NAME` tokens via a shared icon registry.
- Linux writes the same canonical key spellings (`directory`, `command`,
  `color-target`) and its CLI TOML import now persists to dconf (a floating-ref
  double-free silently discarded imported keys before).

### Platform & hygiene

- Structured file-only logging to `logs\axan.log` (5 MB rotation) under the
  app's LocalState, with startup breadcrumbs for launch diagnostics; Linux
  logs to `~/.cache/axan/axan.log`.
- Copy/paste scheme with a Ctrl+C input-safety guard.
- axan branding across the exe, MSIX assets, and app icons; honest copyright
  headers and third-party attribution for the vendored Windows Terminal tree
  (upstream v1.24.11321.0). The conpty host builds as `axan-console.exe`, so
  axan's console hosts are distinguishable from stock Windows Terminal's in
  Task Manager and safe from kill-by-image-name crossfire.

### Installing (Windows)

The release assets are a signed `.msix`, the public signing certificate
(`axan-signing.cer`), and a dependency bundle. One-time, as admin: import the
certificate into `LocalMachine\Trusted People`, then install the MSIX
(double-click, or `Add-AppxPackage`). Subsequent releases install over the top
with no certificate step.

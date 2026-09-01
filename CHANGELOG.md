# Changelog

All notable changes to axan are documented here.

## [Unreleased]

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

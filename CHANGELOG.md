# Changelog

All notable changes to axan are documented here.

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

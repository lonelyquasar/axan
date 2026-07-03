# Watch-outs

Patterns and gotchas encountered while building axan. Read this before assuming
"surely upstream got this right" — they often did not, and the same shape of
problem tends to recur.

## How to use this doc

Each entry names a **pattern** (not a one-off bug). When you hit a problem that
smells similar, check this list first. When you discover a new pattern, add it.

---

## 1. Upstream hardcodes its identity in many independent places

**First encountered:** 2026-05-19/20, while rebranding the gnome-terminal 3.52
fork from `org.gnome.Terminal` → `sh.axan.Axan`.

**The pattern.** Older C/C++ projects often spread their application identity
across many independent layers, with no single source of truth. Renaming the
project requires hunting down every layer — and each layer is invisible until a
specific symptom surfaces it.

**Surfaces we found in gnome-terminal:**

| Layer | Where | Example before → after |
|---|---|---|
| Meson identity variables | `meson.build` | `gt_name = 'gnome-terminal'` → `'axan'` |
| Asset `.in` filenames | `data/`, `src/` | `org.gnome.Terminal.service.in` → `sh.axan.Axan.service.in` |
| Asset `.in` contents | Same files | Schema IDs, interface IDs, exec lines inside the file body |
| C/C++ identity constants | `terminal-defines.hh` | `TERMINAL_APPLICATION_ID`, `TERMINAL_OBJECT_PATH_PREFIX` |
| Slash-form path strings | `terminal-defines.hh` | `"/org/gnome/Terminal/..."` — separate from dot-form |
| gdbus-codegen XML | `*.xml` interface definitions | `<interface name="org.gnome.Terminal.Factory0">` |
| GSettings schema IDs and paths | `*.gschema.xml.in` | Must match what C code looks up at runtime |
| D-Bus service activation file | `*.service.in` | `Name=`, `Exec=`, `SystemdService=` lines |
| Systemd user unit | `*-server.service.in` | Wants matching D-Bus name |
| Environment variable names | `terminal-defines.hh` | `GNOME_TERMINAL_SCREEN` → `AXAN_SCREEN` (client uses this to find parent screen; if stale value leaks in from the surrounding gnome-terminal, server gets a path it cannot resolve and returns `Object does not exist at path /sh/axan/Axan/Factory0` — a misleading error pointing at the factory, not the missing parent) |
| Env var prefix forwarded to spawned shells | `terminal-client-utils.cc` | `"GNOME_TERMINAL_"` filter list |
| Debug env var name | `terminal-debug.cc` | `GNOME_TERMINAL_DEBUG` → `AXAN_DEBUG` |
| GTK program class (`WM_CLASS`) | `terminal-app.cc` | `gdk_set_program_class("Gnome-terminal")` — windows with the old class get grouped under the system gnome-terminal's icon in the GNOME dock/taskbar |
| GTK program name | `server.cc`, `terminal.cc`, `prefs-main.cc` | `g_set_prgname("gnome-terminal")` |
| GTK application name | Same files | `g_set_application_name(_("Terminal"))` — shown in window title, alt-tab |
| Desktop file `StartupWMClass` | `*.desktop.in` | Must match the runtime `WM_CLASS`, else dock groups the window under the wrong (or no) desktop entry |
| Desktop file `Name`, `Exec`, `TryExec`, `Icon` | `*.desktop.in` | All independently hardcoded |
| Preferences binary name | `terminal-defines.hh` | `"gnome-terminal-preferences"` — used by server to spawn prefs |

**Why the rebrand kept stalling.** Each layer is invisible until something
specific breaks. The error messages are often misleading — a stale env var
causes the server to return an error mentioning the *factory* path, not the
parent-screen path actually missing. So you fix the layer you can see, ship,
hit a new symptom, fix the next layer. Repeat ~10 times.

**Mitigation for axan.** We have not deduplicated the C-side constants further
because doing so would only address ~3 of the surfaces. The build-system,
codegen, asset-file, and runtime-WM_CLASS layers cannot be unified from a
single C constant — they would need meson variables driving codegen inputs and
runtime defines together. That is a refactor with real cost and limited
payoff: the only beneficiary is *another* rebrand, which we are unlikely to
do.

**Mitigation for the eventual Rust port.** Day-one design: one `identity`
module with `APPLICATION_ID`, `OBJECT_PATH_PREFIX`, `WM_CLASS`,
`ENV_VAR_PREFIX` constants. Every other module derives from it. Asset
templates also reference it via build script. Refuse to merge any PR that
introduces a sibling string constant.

**How to find leftover identity surfaces.**

```sh
# Find any remaining upstream identity strings in source
grep -rn "org\.gnome\.Terminal\|org/gnome/Terminal\|Gnome-terminal\|gnome-terminal\|GNOME_TERMINAL_" src/ data/
```

This is the recommended check before declaring a rebrand done. It will produce
false positives (translatable strings, comments, file-path references) but the
real surfaces stand out.

---

## 2. Upstream embeds assumptions about its own release cadence

**First encountered:** 2026-05-20, About dialog showed "Version 0.0.3 for GNOME 21".

**The pattern.** Long-running projects sometimes encode their own release
numbering into runtime logic — version math, schema migration thresholds,
"this feature available since version X" gates. When you fork and renumber,
that logic produces nonsense without erroring, because the math still
"works," just on the wrong axis.

**The specific instance.** `src/terminal-util.cc` derived the displayed GNOME
version from `TERMINAL_MINOR_VERSION` using:

```cpp
auto const gnome_version = 40 + (TERMINAL_MINOR_VERSION - 40 + 1) / 2;
```

That formula is correct for gnome-terminal's versioning (3.40 → GNOME 40,
3.52 → GNOME 46) but produces 21 for our `0.0.3` because minor=0. The output
is plausible enough that it could easily ship.

**Fix.** Deleted the GNOME-version derivation. About dialog now just says
"Version %s" — axan isn't bound to any GNOME release line.

**How to find similar.** Grep for runtime use of `TERMINAL_MAJOR_VERSION`,
`TERMINAL_MINOR_VERSION`, or `VERSION` outside of pure display strings. Any
arithmetic on these is suspect. Migration thresholds keyed to old version
numbers are the other place to look — they'll silently treat axan's fresh
schema as ancient and "upgrade" it.

---

## 3. Drop-in widget replacement breaks unwritten contracts

**First encountered:** 2026-05-20, replacing `GtkNotebook` with a `GtkPaned +
GtkListBox + GtkStack` composite (`TerminalSidebar`) that implements the
codebase's existing `TerminalMdiContainer` GObject interface.

**The pattern.** When you replace widget A with widget B because they fill
the same architectural role, you assume the interface between them and the
surrounding code captures all the contract. It usually doesn't. Older
codebases lean on behaviors A provides "for free" that B doesn't — and
nobody wrote the assumption down, because for years there was only A.

**Three concrete unwritten contracts gnome-terminal had on GtkNotebook that
GtkStack does not honor:**

1. **First-inserted-page auto-becomes-current.** `gtk_notebook_insert_page`
   auto-selects when there's no current page. `gtk_stack_add_named` does
   not — `visible-child` stays null until you set it. Result: natural-size
   queries on the stack during early init return 0 width, the chrome-width
   math in `terminal_window_update_geometry` goes negative, the window
   opens at a tiny pixel size before VTE realizes.

2. **Container chrome is approximately tab-strip-sized.** `terminal_window_
   update_size` resizes the window on every screen-switch to fit the active
   terminal's 80×24 grid exactly. That math made sense when the only chrome
   was a thin tab strip; with a 320px sidebar it forces the window narrower
   on every switch, eating user-resized sidebar width.

3. **Natural size of an unrealized child propagates to the parent.** VTE's
   reported natural width depends on font metrics, which are stable before
   realization. GtkNotebook propagated this through unchanged so the window
   opened at the 80×24 grid's pixel size on first map. GtkStack does not —
   `gtk_widget_get_preferred_size(main_vbox, ...)` then computes
   `chrome_width = vbox_request.width - char_width * grid_width` and
   produces a negative number, and the window opens at scrollbar-overhead
   width. Initial v0 hack was `gtk_widget_set_size_request(stack, 640, 384)`
   as a permanent floor; this caused a second bug where the splitter hit an
   invisible wall on sidebar drag-out, and rapid window-resize wiggling
   compounded bash/readline prompt redraw artifacts that vanilla GNOME
   Terminal self-heals (because forcing the stack to a floor changes the
   size-allocate behavior in a way upstream didn't have). Real fix:
   `gtk_window_set_default_size(window, 960, 540)` at window init — it
   only applies at first map, leaves the runtime allocation path untouched,
   and the existing min-size geometry hints (MIN_WIDTH_CHARS=4) still allow
   shrinking freely afterwards.

**Fix shape.** Replace A→B, then for every observable behavior change,
either: (a) preserve the old behavior in your new container, (b) gate the
old-behavior-assuming code behind `GTK_IS_<A>(...)` checks so it only fires
when A is in use, or (c) drop the assumption A made and provide the answer
at a different layer where the runtime path is not affected. We did (b)
for the resize-on-switch and (c) for the startup-size question (moved it
from "child reports natural size at every measure" to "toplevel knows its
default size once").

**Meta-lesson.** When the cheap fix is a `size_request` floor or similar
"clamp at the wrong layer," the bug usually re-emerges as a downstream
regression. Resist the urge to ship the floor. The right fix is almost
always at a layer where the constraint is only consulted once, not on
every allocate cycle.

**How to find similar.** Watch for `gtk_widget_set_size_request`,
`gtk_window_resize`, `gtk_window_set_default_size` calls in surrounding
code. Those are the surfaces where the old widget's natural-size answer
was being implicitly trusted. Also watch for "first insert" auto-behaviors
documented only in the old widget's manpage.

**Sub-pattern (2026-05-26): SIGWINCH storms cause shell prompt corruption.**
After the original wiggle fix, the corruption recurred — caused by an
*unrelated downstream* of the same widget substitution. GtkPaned + GtkStack
fires far more size-allocate cycles per pixel of resize than GtkNotebook
did. The sidebar's expanded-row labels are ~274px wide and refuse to
shrink during window resize, so all compression lands on the terminal
side. During a 300ms wiggle, the terminal column gets allocated through
a dozen widths in the 60-660px range, each one sending TIOCSWINSZ to the
child PTY → SIGWINCH to bash → prompt redraw at that width. With ~12
redraws stacking inside 300ms, leftover prompt fragments accumulate on
the active line and don't clear when allocation settles, because the
final SIGWINCH is at the steady width and bash sees no reason to redraw
again. The "self-heal" trigger is any *subsequent* size change that
gives bash a width different enough from current to force a clean prompt
reset — dragging the sidebar wide and back does it.

**Mitigation we shipped:** raised `MIN_WIDTH_CHARS` from 4 → 24 in
`terminal-window.cc`. The window can't shrink below 24-column terminal
width, so short bash prompts (`user@host:cwd$ `) never get truncated.
Long prompts (SSH-to-IPv6, multi-line PS1) can still trigger the bug.

**Proper fix (deferred):** debounce SIGWINCH at the VTE/screen-container
layer so only the *settled* final size after a wiggle reaches the PTY.
Approaches: (a) intercept size-allocate on the screen container and
defer calling VTE's grid update via a 50-100ms settle timer; (b) patch
VTE itself. Both are substantially more code and have failure modes
(visible "wrong size" period after each resize, fragile across VTE
versions). Not worth pursuing until the mitigation visibly fails.

---

## 4. Forked configuration has correct identifiers but original storage

**First encountered:** 2026-05-20, axan-log revealed that `dconf_engine_
watch_fast` was pointing at `/org/gnome/terminal/legacy/...` paths while
the GSettings schema IDs were already correctly `sh.axan.Axan.*`.

**The pattern.** A configuration system that takes both an *identifier*
(schema ID, namespace) and a *storage location* (dconf path, file path)
can be partially renamed in a way that looks correct from the schema side
but still writes data to the original location. Source-grep for the
identifier finds the new names. Source-grep for the storage path finds
the leftover old names. Both have to move together.

**The specific instance.** `src/sh.axan.Axan.gschema.xml` had schemas
named `sh.axan.Axan.ProfilesList`, `sh.axan.Axan.Legacy.Settings`, etc.
(correct), but their `path="..."` attributes still pointed at `/org/gnome/
terminal/legacy/...` (wrong). Two corresponding C macros in
`terminal-schemas.hh` had the same split. The result: every preference
the user changed in axan was actually written to the system gnome-
terminal's dconf storage, and vice versa.

**How we caught it.** The first time we ran with `axan-log` enabled, the
log showed dconf watching `/org/gnome/terminal/legacy/`. Without
structured logging it would have stayed invisible until a user noticed
preferences inexplicably shared between two apps.

**How to find similar.** For any configuration system with separate ID
and storage axes:

- gsettings: grep `path=` in `*.gschema.xml` and any matching C constants
- file-based config (e.g., `~/.config/<app>/`): grep for the literal app
  name in path-construction code, separately from grepping for the
  configuration-format identifier
- D-Bus: ensure object paths and well-known names move together (we
  caught this earlier; see entry 1's row for D-Bus object paths)

**Mitigation for the eventual Rust port.** A single source of truth for
identity that drives both the namespace identifier AND the storage path,
so they can never disagree.

---

## 5. Some GTK3 widgets have no GdkWindow and silently swallow event signals

**First encountered:** 2026-05-20, sidebar row right-click context menu.

**The pattern.** Several GTK3 widgets (`GtkListBoxRow`, `GtkLabel`,
`GtkBox`, `GtkGrid`, and others) do not have their own `GdkWindow` — they
draw onto their parent's window. Signal connections to events that require
a window (button-press-event, button-release-event, motion-notify-event,
enter-notify-event, leave-notify-event) succeed silently, but the handler
never fires because the events go to the parent.

**The specific instance.** Connecting `button-press-event` to a
`GtkListBoxRow` to handle right-clicks looked correct, compiled clean, and
the connection actually attached. The handler simply never ran. The
`GtkListBox` parent was where the events arrived. The fix was to listen on
the list box and call `gtk_list_box_get_row_at_y(box, event->y)` to find
the row (or `nullptr` for empty space).

**Mitigation.** Either:

- Connect event handlers to the parent and discriminate by hit-testing
- Wrap the no-window widget in a `GtkEventBox` (which has its own window)
- Use `GtkGestureMultiPress` (gesture controllers attach to widgets and
  work regardless of whether the widget has a GdkWindow)

**How to find similar.** If you connect a button or motion signal and the
handler never seems to run, check the widget's docs for "no window" — it's
typically stated under the class description. Faster heuristic: any widget
that inherits from `GtkBin` directly and isn't a button-like / event-
catching widget probably has no window.

---

## 6. Upstream flag names lie about their semantics

**First encountered:** 2026-05-21, profile-driven multi-shell launch.

**The pattern.** A flag with a clear-sounding name has compound semantics
buried in its initializer — the name describes one of the conditions, not
the conjunction. Reading the flag's name and reading the code paths that
*use* it both produce a self-consistent model that happens to be wrong.

**The specific instance.** `InitialWindow::implicit_first_window` reads
as "this window was created implicitly because we needed at least one."
Reasonable. The actual initializer is:

```cpp
iw->implicit_first_window = (options->initial_windows == nullptr) &&
                            implicit_if_first_window;
```

…where `implicit_if_first_window` is passed by the caller as
`g_str_equal(new_terminal_mode_string, "tab")`. So the flag is only TRUE
when **both** the window is implicit AND the user's `new-terminal-mode`
GSettings key is "tab". With mode "window" (a perfectly normal default
on many installs) the flag is FALSE on a clearly-implicit window.

Our first cut of profile-driven launch used this flag as the "is this
window implicit" gate and silently no-op'd for every user whose mode
was "window."

**Fix shape.** Don't trust flag names. When you reach for a boolean
field as a predicate, read the initializer. If the initializer is `A &&
B` and you only care about `A`, derive your own predicate from `A`
directly rather than reusing the field.

**How to find similar.** When a feature works "for me" but not for other
testers, suspect a global setting somewhere in the trigger chain.
`gsettings` differences across machines are a common silent factor that
won't show up in source review. If you have a flag in a feature gate,
print or log its value during testing on the affected machine.

---

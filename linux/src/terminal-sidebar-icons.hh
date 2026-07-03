/*
 * Copyright © 2026 Lonely Quasar
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * axan sidebar icon registry + resolver.
 *
 * Resolves an `icon` field value (as stored in default-launch-entries) into
 * a renderable GtkImage. Icon strings come in four forms:
 *
 *   ""                 — no icon (caller falls back to status-glyph placeholder)
 *   "builtin:NAME"     — short name in the curated registry (terminal, ssh, …)
 *   "/abs/path.svg"    — absolute filesystem path to a custom image
 *   "filename.svg"     — bare filename, resolved against ~/.config/axan/icons/
 *
 * The resolver is GTK-aware (returns a configured GtkWidget) but stateless;
 * called per-cell at render time. The lookup is cheap (a tiny linear scan
 * for built-ins, a single gdk_pixbuf_new_from_file_at_size for paths), so
 * no caching layer is needed at this point — cell rebuilds aren't hot.
 *
 * Built-in names are deliberately a small curated set rather than the full
 * freedesktop icon namespace. The curation lets the eventual picker UI
 * show a finite grid; it also lets the auto-assign hash (piece 4) target a
 * stable index range. Custom SVG drops via the bare-filename form are the
 * escape hatch when the curated set doesn't cover something.
 */

#ifndef TERMINAL_SIDEBAR_ICONS_H
#define TERMINAL_SIDEBAR_ICONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Resolve an icon-string to a GtkImage configured for pixel_size square.
 * Returns a floating widget reference (caller adds to its container).
 *
 * Returns NULL when:
 *   - icon_id is NULL or empty
 *   - icon_id is malformed (e.g., "builtin:" with no name)
 *   - the referenced builtin name is unknown
 *   - the referenced file doesn't exist or can't be loaded as an image
 *
 * Callers should treat NULL as "no icon assigned" and render the fallback
 * glyph instead — that mirrors the empty-string semantics. */
GtkWidget *terminal_sidebar_icon_resolve (const char *icon_id, int pixel_size);

/* Resolve an icon-string to a GdkPixbuf at pixel_size square. Same input
 * grammar as terminal_sidebar_icon_resolve; used when a pixbuf is needed
 * directly (model columns, menu items) rather than a packed GtkImage.
 *
 * Returns a new reference the caller owns. NULL on any of the resolve-
 * failure conditions documented for terminal_sidebar_icon_resolve. */
GdkPixbuf *terminal_sidebar_icon_resolve_to_pixbuf (const char *icon_id, int pixel_size);

/* Like terminal_sidebar_icon_resolve, but paints a builtin/symbolic glyph in
 * @hex ("#RRGGBB") instead of the theme foreground. Custom image files keep
 * their own pixels (the node color only governs symbolic glyphs). Empty/NULL
 * or unparseable @hex falls through to terminal_sidebar_icon_resolve (the
 * theme-tracked path). Used by the per-node "Apply color to: Icon/Both"
 * recolor. Returns a floating widget reference, or NULL on resolve failure. */
GtkWidget *terminal_sidebar_icon_resolve_colored (const char *icon_id,
                                                  int pixel_size,
                                                  const char *hex);

/* Callback type for icon-option enumeration. `id` is the string the
 * caller should persist (e.g. "" / "builtin:terminal" / "my-icon.svg");
 * `label` is the display string (e.g. "(none)" / "Terminal" / "my-icon.svg").
 * Strings are valid only for the duration of the callback. */
typedef void (*TerminalSidebarIconOptionCb) (const char *id,
                                             const char *label,
                                             gpointer    user_data);

/* Enumerate the picker options in canonical order: empty/(none), curated
 * builtins (id "builtin:NAME"), discovered files under ~/.config/axan/icons/,
 * and — if `extra` is non-empty AND not already covered — the `extra` id
 * appended once at the end. Shared by every picker surface (prefs combobox,
 * right-click submenu) so the option list stays consistent. */
void terminal_sidebar_icon_enumerate_options (const char *extra,
                                              TerminalSidebarIconOptionCb cb,
                                              gpointer user_data);

/* Build a GtkComboBox pre-populated with the icon-picker options:
 *   - "(none)" entry with empty id
 *   - every curated built-in (id "builtin:NAME", labelled in title case)
 *   - every readable .svg/.png/.jpg under ~/.config/axan/icons/
 *   - if `current` is set and didn't match any of the above, the current
 *     value is appended raw so the round-trip preserves unknown stored
 *     icon strings (custom paths, future builtins, etc.)
 *
 * The combobox uses an internal GtkListStore with three columns (pixbuf,
 * label, id) and an id_column wired up so callers can use the standard
 * gtk_combo_box_set_active_id / get_active_id API. Active id is set to
 * `current` before the function returns.
 *
 * The returned widget is a floating reference; pack it into a container. */
GtkWidget *terminal_sidebar_icon_combo_new (const char *current);

/* Return a NULL-terminated array of built-in short names. Order is stable
 * across releases (additions go to the end), so deterministic-hash auto-
 * assign (piece 4) survives sidebar additions without re-shuffling already-
 * assigned icons. Caller does not own the array or its strings — both are
 * static. */
const char *const *terminal_sidebar_icon_builtin_names (void);

/* Count of built-in names (so callers don't have to NULL-walk). */
guint terminal_sidebar_icon_builtin_count (void);

/* TRUE when the active GTK theme is dark (its default text foreground is
 * light). Lets callers choose palettes that contrast against the sidebar. */
gboolean terminal_sidebar_icon_theme_is_dark (void);

/* Deterministic-hash auto-assign: given a non-empty seed string (typically
 * a screen UUID or display-name template), return one of the curated
 * built-in short names. Same seed → same name across runs. Returns NULL
 * when seed is empty/NULL — caller falls back to no-icon in that case.
 *
 * The returned pointer is statically owned by the registry; caller does
 * not free. Use it directly or pass it to a "builtin:%s" formatter when
 * a full icon-id string is needed. */
const char *terminal_sidebar_icon_auto_assign (const char *seed);

G_END_DECLS

#endif /* TERMINAL_SIDEBAR_ICONS_H */

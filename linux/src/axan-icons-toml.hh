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
 * axan icon registry import/export.
 *
 * Curates which SVGs in ~/.config/axan/icons/ are exposed in the icon
 * picker. The workflow is intentionally low-ceremony:
 *
 *   1. User drops an .svg into ~/.config/axan/icons/.
 *   2. User runs `axan --import-icons` or clicks the button in Prefs.
 *   3. axan walks the directory, validates each file, and writes the
 *      passing names to ~/.config/axan/icons.toml.
 *
 * Validation rules — all three must pass:
 *
 *   - **Filename**  matches ^[a-z0-9][a-z0-9_-]*\.svg$ (lowercase ASCII,
 *     starts alnum, dashes/underscores allowed, .svg extension). Rejects
 *     "My Cool Icon.svg" and the like so the registered names stay safe
 *     in TOML, dconf, and shell quoting.
 *   - **Size**  under 100 KiB. A reasonable icon SVG is a few KB; anything
 *     larger is probably embedded raster data or a misnamed asset.
 *   - **Renderable**  gdk_pixbuf_new_from_file_at_size() succeeds at the
 *     expected sidebar size. This is the same loader the resolver uses,
 *     so passing here means the picker will render it.
 *
 * Rejection policy: failing files stay in the staging folder; the importer
 * reports name + reason via the `rejected_out` array so the UI (or CLI)
 * can list them. Caller decides whether to nag, prompt, or silently
 * ignore.
 *
 * The picker (terminal-sidebar-icons.cc) reads icons.toml as the source
 * of truth — files in the folder that aren't in the TOML do NOT appear
 * in the dropdown. This is the validation gate's whole point.
 */

#ifndef AXAN_ICONS_TOML_H
#define AXAN_ICONS_TOML_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* $XDG_CONFIG_HOME/axan/icons.toml — registry file. */
char *axan_icons_toml_default_path (void);

/* $XDG_CONFIG_HOME/axan/icons/ — staging directory. Created lazily by
 * the importer; callers don't need to mkdir first. */
char *axan_icons_toml_staging_dir (void);

/* Outcome of one rejected file. `name` is the original filename (no
 * path); `reason` is a short human-readable explanation suitable for
 * direct display. Owned strings; free with g_free. Allocated together
 * via the helper below. */
typedef struct {
  char *name;
  char *reason;
} AxanIconRejection;

void axan_icon_rejection_free (gpointer rejection);

/* Run the import.
 *
 * `staging_dir` is the directory to scan; pass NULL for the default
 * ~/.config/axan/icons/. The TOML is always written to
 * axan_icons_toml_default_path() regardless of staging dir.
 *
 * `imported_out`, if non-NULL, is filled with the bare icon names that
 * passed validation and were written to the TOML — g_strdup'd, caller
 * should g_ptr_array_set_free_func(arr, g_free) first.
 *
 * `rejected_out`, if non-NULL, is filled with AxanIconRejection structs
 * for files that failed. Use axan_icon_rejection_free as the free func.
 *
 * Returns TRUE if the TOML was written successfully (even with zero
 * imports — that's a valid empty registry). Returns FALSE on
 * unrecoverable error (can't open staging dir, can't write TOML, …);
 * `error` is set in that case. Per-file validation failures don't
 * fail the run — they populate `rejected_out` instead. */
gboolean axan_icons_toml_import (const char *staging_dir,
                                 GPtrArray  *imported_out,
                                 GPtrArray  *rejected_out,
                                 GError    **error);

/* Read the current registry. `names_out` is filled with g_strdup'd
 * bare names (no .svg extension stripped, no path prefix — exactly
 * what's listed in the TOML, e.g. "docker"). Caller should
 * g_ptr_array_set_free_func(arr, g_free) first.
 *
 * Missing icons.toml is not an error — `names_out` stays empty and
 * the function returns TRUE. That's the first-run state. */
gboolean axan_icons_toml_load (GPtrArray *names_out, GError **error);

G_END_DECLS

#endif /* AXAN_ICONS_TOML_H */

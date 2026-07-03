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
 * axan keybindings import / export.
 *
 * Covers the sh.axan.Axan.Legacy.Keybindings schema (68 shortcut keys).
 * Same sparse-import semantics as profile-toml and settings-toml: keys
 * absent from the imported TOML are left untouched. See axan#401.
 *
 * Shortcut values are stored as strings in gschema (e.g.
 * "<Control><Shift>c"), or the string "disabled" to suppress a
 * binding — TOML round-trip is trivial since every value is a string.
 */

#ifndef AXAN_KEYBINDINGS_TOML_H
#define AXAN_KEYBINDINGS_TOML_H

#include <gio/gio.h>

G_BEGIN_DECLS

char *axan_keybindings_toml_default_path (void);

gboolean axan_keybindings_toml_export (const char *out_path, GError **error);

/* `changed_keys_out`, if non-NULL, gets a g_strdup'd copy of every key
 * whose value differed from current dconf. Caller should
 * g_ptr_array_set_free_func(arr, g_free) first. */
gboolean axan_keybindings_toml_import (const char *in_path,
                                       GPtrArray *changed_keys_out,
                                       GError **error);

G_END_DECLS

#endif /* AXAN_KEYBINDINGS_TOML_H */

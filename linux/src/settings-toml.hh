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
 * axan global-settings import / export.
 *
 * Covers the sh.axan.Axan.Legacy.Settings schema (21 keys: audio, font,
 * theme defaults, menu behavior, etc.) plus the ProfilesList.default
 * key (which profile axan opens new windows with). The active profile
 * list itself is intentionally NOT exported — it's auto-managed by
 * profile imports, and exposing it would let users break their own
 * config by referencing UUIDs that don't have backing profiles.
 *
 * Same sparse-import semantics as profile-toml: keys absent from the
 * imported TOML are left at their current dconf value, never reset.
 * See lonelyquasar/axan#401.
 */

#ifndef AXAN_SETTINGS_TOML_H
#define AXAN_SETTINGS_TOML_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* $XDG_CONFIG_HOME/axan/settings.toml — the canonical location. */
char *axan_settings_toml_default_path (void);

gboolean axan_settings_toml_export (const char *out_path,
                                    GError **error);

/* `changed_keys_out`, if non-NULL, gets a g_strdup'd copy of every key
 * that was actually changed (TOML value differed from current dconf
 * value). Caller should g_ptr_array_set_free_func(arr, g_free) first. */
gboolean axan_settings_toml_import (const char *in_path,
                                    GPtrArray *changed_keys_out,
                                    GError **error);

G_END_DECLS

#endif /* AXAN_SETTINGS_TOML_H */

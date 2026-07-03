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
 * axan profile import / export — round-trips a single profile's full state
 * to a human-readable TOML document at ~/.config/axan/profiles/<uuid>.toml
 * (or any explicit path).
 *
 * Design (see lonelyquasar/axan#401):
 *
 *   - **UUID is identity.** The TOML embeds the source UUID; reimporting the
 *     same file overwrites the matching dconf profile in-place. This is the
 *     declarative round-trip model — git-track the TOML and re-apply across
 *     machines to recreate the exact same profile.
 *
 *   - **Every key is written explicitly.** All 45 profile keys appear in the
 *     export, even those left at gschema defaults. An LLM reading the file
 *     sees the full state without consulting the schema.
 *
 *   - **Modified values carry a `# modified (default: ...)` comment.** Default
 *     values appear unannotated. The comments are the diff signal — they
 *     survive a future gschema default change and remain auditable.
 *
 *   - **TOML emission is hand-rolled** (not via tomlplusplus's serializer)
 *     because tomlplusplus v3 drops comments on serialize by design.
 *     tomlplusplus is used only on the import side, where its strict
 *     parsing-and-validation is the payoff.
 *
 *   - **Standalone code path.** Operates directly on dconf via GSettings;
 *     no D-Bus or running axan-server required. Invoked via main()'s
 *     `--export-profile` / `--import-profile` early-exit dispatch in
 *     terminal.cc.
 */

#ifndef AXAN_PROFILE_TOML_H
#define AXAN_PROFILE_TOML_H

#include <gio/gio.h>
#include <stdio.h>

G_BEGIN_DECLS

/* Resolve the default export path for a given profile UUID:
 *   $XDG_CONFIG_HOME/axan/profiles/<uuid>.toml
 * Caller owns the returned string. Never returns NULL.
 *
 * The parent directory is NOT created here; export logic creates it
 * lazily so the caller doesn't have to. */
char *axan_profile_toml_default_path (const char *uuid);

/* Export profile identified by `uuid` to `out_path`. If `out_path` is
 * NULL the default path (see above) is used; if it equals "-" the export
 * is written to stdout.
 *
 * The export contains every profile key explicitly. Keys that differ from
 * the gschema default get a trailing comment annotating the original
 * default; defaults are emitted unannotated.
 *
 * Returns TRUE on success. On failure returns FALSE and sets `error` —
 * typical failure modes: unknown UUID, write-permission failure on the
 * output path, parent-directory creation failure. */
gboolean axan_profile_toml_export (const char *uuid,
                                   const char *out_path,
                                   GError **error);

/* Import a profile from a TOML document at `in_path`. The UUID embedded
 * in the file determines which dconf profile is written; if a profile
 * with that UUID already exists it is overwritten key-by-key, otherwise
 * a new profile with that exact UUID is registered in the profiles list.
 *
 * Sparse-TOML semantics: only keys *present* in the file are written.
 * Keys absent from the TOML are left at their current dconf value —
 * NOT reset to default, NOT cleared. This lets users hand-author a
 * minimal TOML with just the keys they want to change. Use
 * `gsettings reset` (or a future Preferences-UI control) for explicit
 * reset-to-default.
 *
 * On success `out_uuid` (if non-NULL) is set to a newly-allocated copy
 * of the resolved UUID — caller owns the string.
 *
 * Returns TRUE on success. Typical failures: unreadable path, malformed
 * TOML, missing required `[profile]` table, missing `uuid` field, value
 * with the wrong type for its gschema key. */
/* `changed_keys_out`, if non-NULL, gets a g_strdup'd copy of every profile
 * key actually changed (TOML value differed from current dconf). The
 * pseudo-key "default-launch-entries" is appended when the launch-entries
 * tuple-array differs. Caller should g_ptr_array_set_free_func(arr,
 * g_free) first. */
gboolean axan_profile_toml_import (const char *in_path,
                                   char **out_uuid,
                                   GPtrArray *changed_keys_out,
                                   GError **error);

G_END_DECLS

#endif /* AXAN_PROFILE_TOML_H */

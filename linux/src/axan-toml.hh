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
 * axan TOML helpers shared by every schema exporter/importer (profile,
 * global settings, keybindings, …).
 *
 * The per-schema modules (profile-toml.cc, settings-toml.cc,
 * keybindings-toml.cc) own the schema-specific concerns — UUID identity,
 * tuple-array launch entries, profiles-list registration. Everything that
 * doesn't depend on which schema it is — TOML string escaping, GVariant
 * scalar/array marshalling, sparse-import semantics — lives here.
 *
 * Style B annotation: keys whose value equals the gschema default are
 * emitted unannotated; modified values carry a `# modified (default: X)`
 * comment. See lonelyquasar/axan#401.
 */

#ifndef AXAN_TOML_H
#define AXAN_TOML_H

#include <gio/gio.h>
#include <stdio.h>

/* toml++ headers are very heavy; include the parser only where needed
 * (the .cc that does the importing). The export side uses none of it. */

G_BEGIN_DECLS

/* Suppress the "axan: wrote X to PATH" / "imported N keys" stderr chatter
 * from per-schema exporters. Used by the auto-export watcher to keep the
 * server's stderr clean. Returns the previous value so callers can scope
 * the change with a stack of set/restore. Default is FALSE (verbose). */
gboolean axan_toml_set_quiet (gboolean quiet);
gboolean axan_toml_is_quiet (void);

/* Open an output path for write. If `out_path` is NULL the caller's
 * `default_path` is used. If `out_path` is "-" stdout is returned (no
 * close needed). On success returns the FILE * and `resolved_path_out`
 * points to a newly-allocated copy of the path actually opened (caller
 * frees with g_free; set to NULL for stdout). Parent directories are
 * created lazily. */
FILE *axan_toml_open_output (const char *out_path,
                             const char *default_path,
                             char **resolved_path_out,
                             GError **error);

/* Emit the standard axan-export header — three comment lines tailored
 * to `tool_args` so the file documents how to re-import itself. */
void axan_toml_emit_header (FILE *out, const char *tool_args);

/* Emit every scalar / scalar-array key of `schema` to `out`, with Style B
 * annotation (modified values get a trailing `# modified (default: X)`
 * comment). Keys are sorted alphabetically for deterministic diffs.
 *
 * `skip_keys` is a NULL-terminated array of key names the caller wants
 * to handle separately (e.g. profile UUID, default-launch-entries). Pass
 * NULL or empty to emit everything.
 *
 * The caller is expected to have already written the [table] header and
 * any leading identity keys before calling. */
gboolean axan_toml_emit_schema_scalars (FILE *out,
                                        GSettings *settings,
                                        GSettingsSchema *schema,
                                        const char *const *skip_keys,
                                        GError **error);

/* Apply scalar keys present in `toml_table_ptr` (a toml::table * — kept
 * as void * here so callers don't need to drag toml++ into their TU
 * compile graph for the function signature alone) onto `settings`.
 *
 * Sparse semantics: only keys *present* in the TOML are written; absent
 * keys are left at their current dconf value (never reset, never
 * cleared). Unknown keys log a warning and are skipped. Type mismatches
 * fail the operation; caller is expected to have wrapped `settings` in
 * g_settings_delay() and should call g_settings_revert() on failure.
 *
 * "Changed" semantics: a key is only written when its incoming TOML value
 * differs from the current dconf value (compared via g_variant_equal).
 * Re-importing a TOML that already matches dconf is a no-op.
 *
 * `skip_keys` lets the caller handle structural keys separately (UUID,
 * tuple-array entries, …) without us trying to scalar-decode them.
 *
 * `*changed_count_out` is incremented (not assigned) by the number of keys
 * actually written so callers can accumulate across multiple schemas.
 *
 * `changed_keys_out`, if non-NULL, gets a g_strdup'd copy of every key
 * name that was actually changed. Caller frees with
 * g_ptr_array_free (arr, TRUE) after a g_ptr_array_set_free_func (arr,
 * g_free). */
gboolean axan_toml_import_schema_scalars (const void *toml_table_ptr,
                                          GSettings *settings,
                                          GSettingsSchema *schema,
                                          const char *const *skip_keys,
                                          guint *changed_count_out,
                                          GPtrArray *changed_keys_out,
                                          GError **error);

G_END_DECLS

#endif /* AXAN_TOML_H */

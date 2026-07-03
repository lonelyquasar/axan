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
 * Global-settings import/export — see settings-toml.hh.
 *
 * Two schemas are bridged into one TOML file:
 *   [settings]            sh.axan.Axan.Legacy.Settings  (21 keys)
 *   [profiles].default    sh.axan.Axan.ProfilesList    (which profile is default)
 *
 * The ProfilesList `list` key is omitted from export and ignored on
 * import — it's auto-managed by profile-toml.cc's import path and
 * exposing it would let a sparse TOML accidentally remove profiles.
 */

#include "config.h"
#include "settings-toml.hh"

#include "axan-toml.hh"
#include "terminal-libgsystem.hh"
#include "terminal-schemas.hh"

#include <toml++/toml.hpp>

#include <string>

namespace {

constexpr const char *kSettingsTable = "settings";
constexpr const char *kProfilesTable = "profiles";
constexpr const char *kProfilesListListKey = "list";

GSettingsSchema *
schema_lookup_or_error (const char *schema_id, GError **error)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = g_settings_schema_source_lookup (src, schema_id, TRUE);
  if (schema == nullptr) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Schema %s not installed", schema_id);
  }
  return schema;
}

} // namespace

char *
axan_settings_toml_default_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                           "axan", "settings.toml", nullptr);
}

gboolean
axan_settings_toml_export (const char *out_path, GError **error)
{
  gs_unref_settings_schema GSettingsSchema *settings_schema =
    schema_lookup_or_error (TERMINAL_SETTING_SCHEMA, error);
  if (settings_schema == nullptr)
    return FALSE;

  gs_unref_settings_schema GSettingsSchema *plist_schema =
    schema_lookup_or_error (TERMINAL_PROFILES_LIST_SCHEMA, error);
  if (plist_schema == nullptr)
    return FALSE;

  gs_unref_object GSettings *settings = g_settings_new (TERMINAL_SETTING_SCHEMA);
  gs_unref_object GSettings *plist = g_settings_new (TERMINAL_PROFILES_LIST_SCHEMA);

  gs_free char *default_path = axan_settings_toml_default_path ();
  gs_free char *resolved_path = nullptr;
  FILE *stream = axan_toml_open_output (out_path, default_path,
                                        &resolved_path, error);
  if (stream == nullptr)
    return FALSE;
  bool close_stream = stream != stdout;

  axan_toml_emit_header (stream, "settings");

  fprintf (stream, "[%s]\n", kSettingsTable);
  if (!axan_toml_emit_schema_scalars (stream, settings, settings_schema,
                                      nullptr, error)) {
    if (close_stream) fclose (stream);
    return FALSE;
  }

  fputc ('\n', stream);
  fprintf (stream, "[%s]\n", kProfilesTable);
  /* Skip the `list` key on export — it's auto-managed by profile imports.
   * Only `default` is meaningful user-controllable state at this scope. */
  const char *skip[] = { kProfilesListListKey, nullptr };
  if (!axan_toml_emit_schema_scalars (stream, plist, plist_schema,
                                      skip, error)) {
    if (close_stream) fclose (stream);
    return FALSE;
  }

  if (close_stream)
    fclose (stream);

  if (resolved_path != nullptr && !axan_toml_is_quiet ())
    g_printerr ("axan: wrote global settings to %s\n", resolved_path);
  return TRUE;
}

gboolean
axan_settings_toml_import (const char *in_path,
                           GPtrArray *changed_keys_out,
                           GError **error)
{
  g_return_val_if_fail (in_path != nullptr, FALSE);

  toml::table tbl;
  try {
    tbl = toml::parse_file (in_path);
  } catch (const toml::parse_error &e) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "TOML parse error in %s: %s", in_path, e.what ());
    return FALSE;
  }

  gs_unref_settings_schema GSettingsSchema *settings_schema =
    schema_lookup_or_error (TERMINAL_SETTING_SCHEMA, error);
  if (settings_schema == nullptr) return FALSE;

  gs_unref_settings_schema GSettingsSchema *plist_schema =
    schema_lookup_or_error (TERMINAL_PROFILES_LIST_SCHEMA, error);
  if (plist_schema == nullptr) return FALSE;

  gs_unref_object GSettings *settings = g_settings_new (TERMINAL_SETTING_SCHEMA);
  gs_unref_object GSettings *plist = g_settings_new (TERMINAL_PROFILES_LIST_SCHEMA);

  g_settings_delay (settings);
  g_settings_delay (plist);

  guint applied = 0;

  if (auto *settings_tbl = tbl[kSettingsTable].as_table ()) {
    if (!axan_toml_import_schema_scalars (settings_tbl, settings,
                                          settings_schema,
                                          nullptr, &applied,
                                          changed_keys_out, error)) {
      g_settings_revert (settings);
      g_settings_revert (plist);
      return FALSE;
    }
  }

  if (auto *profiles_tbl = tbl[kProfilesTable].as_table ()) {
    /* `list` is auto-managed; refuse silently rather than letting a user
     * accidentally orphan profiles via a sparse hand-edited TOML. */
    const char *skip[] = { kProfilesListListKey, nullptr };
    if (!axan_toml_import_schema_scalars (profiles_tbl, plist, plist_schema,
                                          skip, &applied,
                                          changed_keys_out, error)) {
      g_settings_revert (settings);
      g_settings_revert (plist);
      return FALSE;
    }
  }

  g_settings_apply (settings);
  g_settings_apply (plist);
  g_settings_sync ();

  if (!axan_toml_is_quiet ())
    g_printerr ("axan: imported %u global setting%s\n",
                applied, applied == 1 ? "" : "s");
  return TRUE;
}

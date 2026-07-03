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
 * Keybindings import/export — see keybindings-toml.hh.
 */

#include "config.h"
#include "keybindings-toml.hh"

#include "axan-toml.hh"
#include "terminal-libgsystem.hh"
#include "terminal-schemas.hh"

#include <toml++/toml.hpp>

namespace {
constexpr const char *kTable = "keybindings";
} // namespace

char *
axan_keybindings_toml_default_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                           "axan", "keybindings.toml", nullptr);
}

gboolean
axan_keybindings_toml_export (const char *out_path, GError **error)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  gs_unref_settings_schema GSettingsSchema *schema =
    g_settings_schema_source_lookup (src, TERMINAL_KEYBINDINGS_SCHEMA, TRUE);
  if (schema == nullptr) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Schema %s not installed", TERMINAL_KEYBINDINGS_SCHEMA);
    return FALSE;
  }
  /* sh.axan.Axan.Legacy.Keybindings is a relocatable schema (accessed via
   * the Settings.keybindings child binding), so it needs an explicit path. */
  gs_unref_object GSettings *kb =
    g_settings_new_with_path (TERMINAL_KEYBINDINGS_SCHEMA,
                              TERMINAL_KEYBINDINGS_SCHEMA_PATH);

  gs_free char *default_path = axan_keybindings_toml_default_path ();
  gs_free char *resolved_path = nullptr;
  FILE *stream = axan_toml_open_output (out_path, default_path,
                                        &resolved_path, error);
  if (stream == nullptr)
    return FALSE;
  bool close_stream = stream != stdout;

  axan_toml_emit_header (stream, "keybindings");
  fprintf (stream, "[%s]\n", kTable);
  if (!axan_toml_emit_schema_scalars (stream, kb, schema, nullptr, error)) {
    if (close_stream) fclose (stream);
    return FALSE;
  }

  if (close_stream)
    fclose (stream);

  if (resolved_path != nullptr && !axan_toml_is_quiet ())
    g_printerr ("axan: wrote keybindings to %s\n", resolved_path);
  return TRUE;
}

gboolean
axan_keybindings_toml_import (const char *in_path,
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

  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  gs_unref_settings_schema GSettingsSchema *schema =
    g_settings_schema_source_lookup (src, TERMINAL_KEYBINDINGS_SCHEMA, TRUE);
  if (schema == nullptr) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Schema %s not installed", TERMINAL_KEYBINDINGS_SCHEMA);
    return FALSE;
  }
  /* sh.axan.Axan.Legacy.Keybindings is a relocatable schema (accessed via
   * the Settings.keybindings child binding), so it needs an explicit path. */
  gs_unref_object GSettings *kb =
    g_settings_new_with_path (TERMINAL_KEYBINDINGS_SCHEMA,
                              TERMINAL_KEYBINDINGS_SCHEMA_PATH);
  g_settings_delay (kb);

  guint applied = 0;
  if (auto *kb_tbl = tbl[kTable].as_table ()) {
    if (!axan_toml_import_schema_scalars (kb_tbl, kb, schema,
                                          nullptr, &applied,
                                          changed_keys_out, error)) {
      g_settings_revert (kb);
      return FALSE;
    }
  } else {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "%s: missing [keybindings] table", in_path);
    return FALSE;
  }

  g_settings_apply (kb);
  g_settings_sync ();

  if (!axan_toml_is_quiet ())
    g_printerr ("axan: imported %u keybinding%s\n",
                applied, applied == 1 ? "" : "s");
  return TRUE;
}

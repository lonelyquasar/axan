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
 * axan profile import / export — see profile-toml.hh for the contract.
 *
 * Most of the value-marshaling work lives in axan-toml.{cc,hh}; this
 * file handles the profile-specific concerns:
 *   - the [profile].uuid identity field (the file's identity for
 *     overwrite-by-UUID re-imports),
 *   - the default-launch-entries array-of-tables (the one tuple-array
 *     in the profile schema), and
 *   - registering imported UUIDs in the ProfilesList so the new profile
 *     appears in the UI.
 *
 * See lonelyquasar/axan#401.
 */

#include "config.h"
#include "profile-toml.hh"

#include "axan-toml.hh"
#include "terminal-libgsystem.hh"
#include "terminal-schemas.hh"

#include <toml++/toml.hpp>

#include <errno.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

/* The 8 tuple positions in default-launch-entries, in canonical order
 * (matches profile-editor.cc's GVariantBuilder add order). Used as the
 * field names in the TOML array-of-tables representation. `color` is the
 * per-node recolor token (palette name like "blue", or a literal "#RRGGBB");
 * `color-target` is "icon"/"text"/"both".
 *
 * These spellings are the canonical cross-platform interchange dictionary —
 * they must match the Windows build's TomlKey table in
 * windows/src/inc/AxanLaunchEntryWire.h exactly (kebab-case, `directory`
 * not `dir`). Either side renaming a key without the other breaks import
 * silently: unknown keys are skipped with a logged warning, so the value
 * just vanishes on the other OS. */
constexpr const char *kLaunchEntryFields[8] = {
  "id", "parent-id", "name", "directory", "command", "icon", "color", "color-target",
};
constexpr const char *kLaunchEntriesKey = "default-launch-entries";
constexpr const char *kProfileTable = "profile";
constexpr const char *kUuidKey = "uuid";

char *
profile_path_for_uuid (const char *uuid)
{
  return g_strconcat (TERMINAL_PROFILES_PATH_PREFIX, ":", uuid, "/", nullptr);
}

GSettings *
profile_settings_open (const char *uuid)
{
  gs_free char *path = profile_path_for_uuid (uuid);
  return g_settings_new_with_path (TERMINAL_PROFILE_SCHEMA, path);
}

/* Minimal TOML-quote duplicate — only used for the two literals we emit
 * outside of axan_toml_emit_schema_scalars (the uuid field and launch-
 * entry strings). Keeps profile-toml.cc from depending on the .cc-local
 * helper in axan-toml.cc. */
std::string
quote (const char *s)
{
  std::string out = "\"";
  for (const char *p = s; *p != '\0'; ++p) {
    char c = *p;
    if (c == '"' || c == '\\') { out.push_back ('\\'); out.push_back (c); }
    else if (c == '\n')        { out += "\\n"; }
    else if (c == '\t')        { out += "\\t"; }
    else                       { out.push_back (c); }
  }
  out.push_back ('"');
  return out;
}

void
write_launch_entry (FILE *out, GVariant *entry)
{
  fputs ("[[profile.default-launch-entries]]\n", out);
  /* The first six fields are always emitted (id..icon). */
  for (int i = 0; i < 6; ++i) {
    gs_unref_variant GVariant *field = g_variant_get_child_value (entry, i);
    const char *s = g_variant_get_string (field, nullptr);
    fprintf (out, "%-12s = %s\n", kLaunchEntryFields[i], quote (s).c_str ());
  }
  /* color and color-target are sparse: omit `color` when empty, omit
   * `color-target` when empty or the "both" default — keeps exported TOML
   * clean and matches the Windows writer's sparseness. */
  {
    gs_unref_variant GVariant *field = g_variant_get_child_value (entry, 6);
    const char *s = g_variant_get_string (field, nullptr);
    if (s != nullptr && *s != '\0')
      fprintf (out, "%-12s = %s\n", kLaunchEntryFields[6], quote (s).c_str ());
  }
  {
    gs_unref_variant GVariant *field = g_variant_get_child_value (entry, 7);
    const char *s = g_variant_get_string (field, nullptr);
    if (s != nullptr && *s != '\0' && strcmp (s, "both") != 0)
      fprintf (out, "%-12s = %s\n", kLaunchEntryFields[7], quote (s).c_str ());
  }
  fputc ('\n', out);
}

void
ensure_uuid_in_profiles_list (const char *uuid)
{
  gs_unref_object GSettings *list_settings =
    g_settings_new (TERMINAL_PROFILES_LIST_SCHEMA);
  gs_strfreev gchar **uuids =
    g_settings_get_strv (list_settings, TERMINAL_SETTINGS_LIST_LIST_KEY);

  for (gsize i = 0; uuids[i] != nullptr; ++i) {
    if (g_str_equal (uuids[i], uuid))
      return;
  }

  gsize n = g_strv_length (uuids);
  std::vector<const char *> next;
  next.reserve (n + 2);
  for (gsize i = 0; i < n; ++i)
    next.push_back (uuids[i]);
  next.push_back (uuid);
  next.push_back (nullptr);

  g_settings_set_strv (list_settings,
                       TERMINAL_SETTINGS_LIST_LIST_KEY,
                       next.data ());
}

} // namespace

char *
axan_profile_toml_default_path (const char *uuid)
{
  return g_build_filename (g_get_user_config_dir (),
                           "axan", "profiles",
                           (std::string (uuid) + ".toml").c_str (),
                           nullptr);
}

gboolean
axan_profile_toml_export (const char *uuid,
                          const char *out_path,
                          GError **error)
{
  g_return_val_if_fail (uuid != nullptr && *uuid != '\0', FALSE);

  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  gs_unref_settings_schema GSettingsSchema *schema =
    g_settings_schema_source_lookup (source, TERMINAL_PROFILE_SCHEMA, TRUE);
  if (schema == nullptr) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Profile schema %s not installed", TERMINAL_PROFILE_SCHEMA);
    return FALSE;
  }

  gs_unref_object GSettings *profile = profile_settings_open (uuid);

  gs_free char *default_path = axan_profile_toml_default_path (uuid);
  gs_free char *resolved_path = nullptr;
  FILE *stream = axan_toml_open_output (out_path, default_path,
                                        &resolved_path, error);
  if (stream == nullptr)
    return FALSE;
  bool close_stream = stream != stdout;

  axan_toml_emit_header (stream, "profile");
  fprintf (stream, "[%s]\n", kProfileTable);
  fprintf (stream, "%-9s = %s  # identity — re-import overwrites this UUID\n",
           kUuidKey, quote (uuid).c_str ());

  /* default-launch-entries is the one structural key — its tuple-array
   * gets emitted separately below as [[profile.default-launch-entries]]. */
  const char *skip[] = { kLaunchEntriesKey, nullptr };
  if (!axan_toml_emit_schema_scalars (stream, profile, schema, skip, error)) {
    if (close_stream) fclose (stream);
    return FALSE;
  }

  fputc ('\n', stream);

  gs_unref_variant GVariant *entries =
    g_settings_get_value (profile, kLaunchEntriesKey);
  gsize n = g_variant_n_children (entries);
  if (n == 0) {
    fputs ("# (no startup launch entries)\n", stream);
  } else {
    for (gsize i = 0; i < n; ++i) {
      gs_unref_variant GVariant *entry = g_variant_get_child_value (entries, i);
      write_launch_entry (stream, entry);
    }
  }

  if (close_stream)
    fclose (stream);

  if (resolved_path != nullptr && !axan_toml_is_quiet ())
    g_printerr ("axan: wrote profile %s to %s\n", uuid, resolved_path);
  return TRUE;
}

gboolean
axan_profile_toml_import (const char *in_path,
                          char **out_uuid,
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

  auto profile_node = tbl[kProfileTable];
  auto *profile_tbl = profile_node.as_table ();
  if (!profile_tbl) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "%s: missing [profile] table", in_path);
    return FALSE;
  }

  auto uuid_node = (*profile_tbl)[kUuidKey];
  auto *uuid_str = uuid_node.as_string ();
  if (!uuid_str) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "%s: missing or non-string [profile].uuid", in_path);
    return FALSE;
  }
  std::string uuid = uuid_str->get ();

  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  gs_unref_settings_schema GSettingsSchema *schema =
    g_settings_schema_source_lookup (source, TERMINAL_PROFILE_SCHEMA, TRUE);
  if (schema == nullptr) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Profile schema %s not installed", TERMINAL_PROFILE_SCHEMA);
    return FALSE;
  }

  gs_unref_object GSettings *profile = profile_settings_open (uuid.c_str ());
  g_settings_delay (profile);

  /* uuid and default-launch-entries are handled outside the scalar loop. */
  const char *skip[] = { kUuidKey, kLaunchEntriesKey, nullptr };
  guint applied = 0;
  if (!axan_toml_import_schema_scalars (profile_tbl, profile, schema,
                                        skip, &applied,
                                        changed_keys_out, error)) {
    g_settings_revert (profile);
    return FALSE;
  }

  bool launch_entries_changed = false;
  auto entries_node = (*profile_tbl)[kLaunchEntriesKey];
  if (auto *arr = entries_node.as_array ()) {
    GVariantBuilder vb;
    g_variant_builder_init (&vb, G_VARIANT_TYPE ("a(ssssssss)"));
    for (auto &&el : *arr) {
      auto *entry_tbl = el.as_table ();
      if (!entry_tbl) {
        g_settings_revert (profile);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "%s: default-launch-entries element is not a table",
                     in_path);
        return FALSE;
      }
      /* color/color-target are optional — a 6-field TOML (pre-color, or a
       * hand-written entry without them) reads back with empty defaults. */
      const char *fields[8] = { "", "", "", "", "", "", "", "" };
      std::string holders[8];
      for (int i = 0; i < 8; ++i) {
        auto field = (*entry_tbl)[kLaunchEntryFields[i]];
        if (auto *s = field.as_string ()) {
          holders[i] = s->get ();
          fields[i] = holders[i].c_str ();
        }
      }
      g_variant_builder_add (&vb, "(ssssssss)",
                             fields[0], fields[1], fields[2], fields[3],
                             fields[4], fields[5], fields[6], fields[7]);
    }
    /* g_variant_builder_end returns FLOATING — sink before autocleanup
     * ownership, or the g_settings_set_value below (which consumes floating
     * refs) plus the scope-exit unref double-free it. See the matching
     * comment in axan-toml.cc's import helper. */
    gs_unref_variant GVariant *new_entries =
      g_variant_ref_sink (g_variant_builder_end (&vb));
    gs_unref_variant GVariant *current =
      g_settings_get_value (profile, kLaunchEntriesKey);
    if (current == nullptr || !g_variant_equal (current, new_entries)) {
      g_settings_set_value (profile, kLaunchEntriesKey, new_entries);
      launch_entries_changed = true;
      if (changed_keys_out != nullptr)
        g_ptr_array_add (changed_keys_out, g_strdup (kLaunchEntriesKey));
    }
  }

  g_settings_apply (profile);
  g_settings_sync ();

  ensure_uuid_in_profiles_list (uuid.c_str ());
  g_settings_sync ();

  if (!axan_toml_is_quiet ())
    g_printerr ("axan: imported %u key%s%s into profile %s\n",
                applied, applied == 1 ? "" : "s",
                launch_entries_changed ? " + launch entries" : "",
                uuid.c_str ());

  if (out_uuid != nullptr)
    *out_uuid = g_strdup (uuid.c_str ());

  return TRUE;
}

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
 * Shared TOML marshaling — see axan-toml.hh for the contract.
 *
 * Lifted from profile-toml.cc when global-settings and keybindings
 * exporters joined the picture. The schema-specific concerns stay in
 * each per-schema module; everything here is concerned only with
 * value-level translation between GVariant and TOML.
 */

#include "config.h"
#include "axan-toml.hh"

#include "terminal-libgsystem.hh"

#include <toml++/toml.hpp>

#include <errno.h>
#include <string.h>

#include <string>
#include <vector>

/* ---------------- quiet flag (used by auto-export) ----------------- */

static gboolean s_quiet = FALSE;

gboolean
axan_toml_set_quiet (gboolean quiet)
{
  gboolean prev = s_quiet;
  s_quiet = quiet;
  return prev;
}

gboolean
axan_toml_is_quiet (void)
{
  return s_quiet;
}

/* ---------------- emission helpers (used by export) ---------------- */

static std::string
toml_quote_string (const char *s)
{
  std::string out;
  out.reserve (strlen (s) + 2);
  out.push_back ('"');
  for (const char *p = s; *p != '\0'; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\t': out += "\\t";  break;
      case '\n': out += "\\n";  break;
      case '\f': out += "\\f";  break;
      case '\r': out += "\\r";  break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char esc[8];
          g_snprintf (esc, sizeof esc, "\\u%04X", c);
          out += esc;
        } else {
          out.push_back (static_cast<char>(c));
        }
    }
  }
  out.push_back ('"');
  return out;
}

/* Format a GVariant as a TOML literal. Handles the scalar and scalar-array
 * types the axan schemas use. Returns "" for ms-Nothing — caller is
 * expected to omit the key entirely in that case.
 *
 * Fallback (unknown variant type): emits the GVariant text form as a
 * quoted string so we survive a schema addition without crashing — the
 * Style B annotation will make the imperfect mapping visible. */
static std::string
format_variant (GVariant *value)
{
  if (value == nullptr)
    return {};

  const GVariantType *vt = g_variant_get_type (value);

  if (g_variant_type_equal (vt, G_VARIANT_TYPE_BOOLEAN))
    return g_variant_get_boolean (value) ? "true" : "false";

  if (g_variant_type_equal (vt, G_VARIANT_TYPE_INT32) ||
      g_variant_type_equal (vt, G_VARIANT_TYPE_INT16) ||
      g_variant_type_equal (vt, G_VARIANT_TYPE_INT64) ||
      g_variant_type_equal (vt, G_VARIANT_TYPE_UINT32) ||
      g_variant_type_equal (vt, G_VARIANT_TYPE_UINT16) ||
      g_variant_type_equal (vt, G_VARIANT_TYPE_UINT64) ||
      g_variant_type_equal (vt, G_VARIANT_TYPE_BYTE)) {
    gs_free char *txt = g_variant_print (value, FALSE);
    return std::string (txt);
  }

  if (g_variant_type_equal (vt, G_VARIANT_TYPE_DOUBLE)) {
    double d = g_variant_get_double (value);
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr (buf, sizeof buf, d);
    if (!strchr (buf, '.') && !strchr (buf, 'e') && !strchr (buf, 'E') &&
        !strchr (buf, 'n')) {
      return std::string (buf) + ".0";
    }
    return std::string (buf);
  }

  if (g_variant_type_equal (vt, G_VARIANT_TYPE_STRING)) {
    const char *s = g_variant_get_string (value, nullptr);
    return toml_quote_string (s);
  }

  if (g_variant_type_equal (vt, G_VARIANT_TYPE ("ms"))) {
    gs_unref_variant GVariant *inner = g_variant_get_maybe (value);
    if (inner == nullptr)
      return {};
    const char *s = g_variant_get_string (inner, nullptr);
    return toml_quote_string (s);
  }

  if (g_variant_type_equal (vt, G_VARIANT_TYPE ("mb"))) {
    /* Same Nothing-as-omission convention as ms. The headerbar setting
     * is the practical case here. */
    gs_unref_variant GVariant *inner = g_variant_get_maybe (value);
    if (inner == nullptr)
      return {};
    return g_variant_get_boolean (inner) ? "true" : "false";
  }

  if (g_variant_type_equal (vt, G_VARIANT_TYPE_STRING_ARRAY)) {
    std::string out = "[";
    gsize n = g_variant_n_children (value);
    for (gsize i = 0; i < n; ++i) {
      if (i > 0) out += ", ";
      gs_unref_variant GVariant *child = g_variant_get_child_value (value, i);
      out += toml_quote_string (g_variant_get_string (child, nullptr));
    }
    out += "]";
    return out;
  }

  gs_free char *txt = g_variant_print (value, TRUE);
  return toml_quote_string (txt);
}

static bool
is_in_skip_list (const char *const *skip_keys, const char *key)
{
  if (skip_keys == nullptr) return false;
  for (gsize i = 0; skip_keys[i] != nullptr; ++i) {
    if (g_str_equal (skip_keys[i], key))
      return true;
  }
  return false;
}

FILE *
axan_toml_open_output (const char *out_path,
                       const char *default_path,
                       char **resolved_path_out,
                       GError **error)
{
  g_return_val_if_fail (default_path != nullptr || out_path != nullptr, nullptr);
  if (resolved_path_out != nullptr)
    *resolved_path_out = nullptr;

  if (out_path != nullptr && g_str_equal (out_path, "-"))
    return stdout;

  const char *path = (out_path != nullptr) ? out_path : default_path;

  gs_free char *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0755) != 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Cannot create directory %s: %s", dir, g_strerror (errno));
    return nullptr;
  }

  FILE *stream = fopen (path, "w");
  if (stream == nullptr) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Cannot write %s: %s", path, g_strerror (errno));
    return nullptr;
  }

  if (resolved_path_out != nullptr)
    *resolved_path_out = g_strdup (path);
  return stream;
}

void
axan_toml_emit_header (FILE *out, const char *tool_args)
{
  fputs ("# axan TOML export\n", out);
  fprintf (out, "# Re-import with: axan --import-%s <this-file>\n", tool_args);
  fputs ("# Every key is written explicitly. Lines without a comment match\n", out);
  fputs ("# the gschema default; `# modified (default: X)` flags edits.\n", out);
  fputs ("#\n", out);
  fputs ("# Sparse imports are honored: omit a key to leave it untouched.\n", out);
  fputs ("\n", out);
}

gboolean
axan_toml_emit_schema_scalars (FILE *out,
                               GSettings *settings,
                               GSettingsSchema *schema,
                               const char *const *skip_keys,
                               GError ** /* error */)
{
  gs_strfreev gchar **keys = g_settings_schema_list_keys (schema);

  /* Sort alphabetically — g_settings_schema_list_keys returns in hash-
   * bucket order which isn't stable across glib versions. */
  gsize key_count = g_strv_length (keys);
  if (key_count > 1)
    qsort (keys, key_count, sizeof (char *),
           [](const void *a, const void *b) {
             return strcmp (*static_cast<const char *const *>(a),
                            *static_cast<const char *const *>(b));
           });

  /* Compute max key width for `=` alignment. */
  gsize max_key_w = 0;
  for (gsize i = 0; keys[i] != nullptr; ++i) {
    if (is_in_skip_list (skip_keys, keys[i])) continue;
    gsize w = strlen (keys[i]);
    if (w > max_key_w) max_key_w = w;
  }

  for (gsize i = 0; keys[i] != nullptr; ++i) {
    const char *key = keys[i];
    if (is_in_skip_list (skip_keys, key))
      continue;

    gs_unref_variant GVariant *current = g_settings_get_value (settings, key);

    gs_unref_settings_schema_key GSettingsSchemaKey *schema_key =
      g_settings_schema_get_key (schema, key);
    gs_unref_variant GVariant *def =
      g_settings_schema_key_get_default_value (schema_key);

    std::string current_str = format_variant (current);
    bool is_default = def != nullptr && g_variant_equal (current, def) != FALSE;

    if (current_str.empty ()) {
      /* `m*` Nothing — TOML has no null, and omitting the key entirely
       * would hide the key's existence from a reader auditing the file
       * ("ALL settings" was the user-stated export goal). Emit as a
       * commented placeholder so the key is visible but parseable as a
       * no-op on import. Uncomment and assign a value to set it. */
      const char *vt_str =
        g_variant_type_peek_string (g_variant_get_type (current));
      fprintf (out, "# %-*s = ?  # unset (type: %s)\n",
               static_cast<int>(max_key_w), key, vt_str);
      continue;
    }

    fprintf (out, "%-*s = %s",
             static_cast<int>(max_key_w), key, current_str.c_str ());

    if (!is_default) {
      std::string def_str = format_variant (def);
      if (def_str.empty ()) def_str = "<unset>";
      fprintf (out, "  # modified (default: %s)", def_str.c_str ());
    }
    fputc ('\n', out);
  }
  return TRUE;
}

/* ---------------- import-side helper ---------------- */

/* Convert a tomlplusplus node to a GVariant matching the schema-key's
 * expected type. Returns nullptr on type mismatch. */
static GVariant *
toml_to_variant (const toml::node &node, const GVariantType *expected)
{
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_BOOLEAN)) {
    auto *b = node.as_boolean ();
    return b ? g_variant_new_boolean (b->get ()) : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_INT32)) {
    auto *i = node.as_integer ();
    return i ? g_variant_new_int32 (static_cast<gint32>(i->get ())) : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_UINT32)) {
    auto *i = node.as_integer ();
    return i ? g_variant_new_uint32 (static_cast<guint32>(i->get ())) : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_INT64)) {
    auto *i = node.as_integer ();
    return i ? g_variant_new_int64 (i->get ()) : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_UINT64)) {
    auto *i = node.as_integer ();
    return i ? g_variant_new_uint64 (static_cast<guint64>(i->get ())) : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_DOUBLE)) {
    if (auto *d = node.as_floating_point ())
      return g_variant_new_double (d->get ());
    /* Integer literal in a double slot — accept it. */
    if (auto *i = node.as_integer ())
      return g_variant_new_double (static_cast<double>(i->get ()));
    return nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_STRING)) {
    auto *s = node.as_string ();
    return s ? g_variant_new_string (s->get ().c_str ()) : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE ("ms"))) {
    auto *s = node.as_string ();
    return s ? g_variant_new_maybe (G_VARIANT_TYPE_STRING,
                                    g_variant_new_string (s->get ().c_str ()))
             : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE ("mb"))) {
    auto *b = node.as_boolean ();
    return b ? g_variant_new_maybe (G_VARIANT_TYPE_BOOLEAN,
                                    g_variant_new_boolean (b->get ()))
             : nullptr;
  }
  if (g_variant_type_equal (expected, G_VARIANT_TYPE_STRING_ARRAY)) {
    auto *arr = node.as_array ();
    if (!arr) return nullptr;
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE_STRING_ARRAY);
    for (auto &&el : *arr) {
      auto *s = el.as_string ();
      if (!s) {
        g_variant_builder_clear (&b);
        return nullptr;
      }
      g_variant_builder_add (&b, "s", s->get ().c_str ());
    }
    return g_variant_builder_end (&b);
  }
  return nullptr;
}

gboolean
axan_toml_import_schema_scalars (const void *toml_table_ptr,
                                 GSettings *settings,
                                 GSettingsSchema *schema,
                                 const char *const *skip_keys,
                                 guint *changed_count_out,
                                 GPtrArray *changed_keys_out,
                                 GError **error)
{
  g_return_val_if_fail (toml_table_ptr != nullptr, FALSE);
  const toml::table &tbl = *static_cast<const toml::table *>(toml_table_ptr);

  /* IMPORTANT: This iterates the TOML table, NOT the gschema. Keys absent
   * from the TOML are silently left at their current dconf value — they
   * are NOT reset to default, NOT cleared, NOT errored on. See axan#401
   * for the rationale.
   *
   * "changed" semantics: a key is only reported (and only written) when
   * its incoming TOML value differs from the current dconf value. Idempotent
   * imports of a TOML that already matches dconf report zero changes. */
  for (auto &&[key_sv, val] : tbl) {
    std::string key (key_sv.str ());
    if (is_in_skip_list (skip_keys, key.c_str ()))
      continue;

    if (!g_settings_schema_has_key (schema, key.c_str ())) {
      g_warning ("axan: skipping unknown key '%s' for schema %s",
                 key.c_str (),
                 g_settings_schema_get_id (schema));
      continue;
    }

    gs_unref_settings_schema_key GSettingsSchemaKey *schema_key =
      g_settings_schema_get_key (schema, key.c_str ());
    const GVariantType *expected =
      g_settings_schema_key_get_value_type (schema_key);

    /* toml_to_variant returns a FLOATING variant (g_variant_new_*).
     * Sink it before the autocleanup takes ownership: g_settings_set_value
     * consumes floating refs, so without the sink the set_value below plus
     * the scope-exit unref double-free the variant while the delayed
     * changeset still holds it. The corpse then poisons the whole batch at
     * g_settings_apply() — dconf silently dropped every profile key on CLI
     * import (the old #420), and the keyfile backend segfaulted outright. */
    GVariant *floating = toml_to_variant (val, expected);
    gs_unref_variant GVariant *variant =
      floating != nullptr ? g_variant_ref_sink (floating) : nullptr;
    if (variant == nullptr) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "value for '%s' doesn't match expected type %s",
                   key.c_str (),
                   g_variant_type_peek_string (expected));
      return FALSE;
    }

    gs_unref_variant GVariant *current =
      g_settings_get_value (settings, key.c_str ());
    if (current != nullptr && g_variant_equal (current, variant))
      continue;

    g_settings_set_value (settings, key.c_str (), variant);
    if (changed_count_out != nullptr)
      (*changed_count_out)++;
    if (changed_keys_out != nullptr)
      g_ptr_array_add (changed_keys_out, g_strdup (key.c_str ()));
  }
  return TRUE;
}

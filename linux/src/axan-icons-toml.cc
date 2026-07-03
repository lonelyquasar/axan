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
 * Icon registry import — see axan-icons-toml.hh.
 *
 * Implementation notes:
 *
 *   - We hand-roll a minimal TOML reader for the single `icons = [...]`
 *     array and a hand-rolled writer. tomlplusplus would be overkill
 *     for a flat array of strings, and dragging it in here would pull
 *     the heavy parser into the server's link graph for no benefit.
 *
 *   - Validation runs gdk_pixbuf_new_from_file_at_size at 24px, the
 *     sidebar's "medium" size. That's both the cheapest renderer
 *     check we have and the exact path the picker uses, so success
 *     here guarantees success at picker time. (rsvg is loaded as a
 *     pixbuf loader plugin on Linux GTK systems; if it's missing
 *     the validation will fail honestly.)
 *
 *   - The TOML rewrite is destructive: previously-imported icons that
 *     no longer pass validation (e.g., user deleted the file) drop
 *     out of the registry on the next import run. That's the desired
 *     "registry mirrors what passes" semantic.
 */

#include "config.h"
#include "axan-icons-toml.hh"

#include "terminal-libgsystem.hh"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>

namespace {

constexpr gsize  kMaxBytes      = 100 * 1024;
constexpr int    kValidateSize  = 24;
constexpr const char *kFilenameRegex = "^[a-z0-9][a-z0-9_-]*\\.svg$";

/* Strip the .svg extension. Caller owns. */
char *
strip_svg_ext (const char *name)
{
  gsize n = strlen (name);
  if (n > 4 && g_str_has_suffix (name, ".svg"))
    return g_strndup (name, n - 4);
  return g_strdup (name);
}

gboolean
validate_filename (const char *name, char **reason_out)
{
  static GRegex *re = nullptr;
  if (re == nullptr)
    re = g_regex_new (kFilenameRegex, G_REGEX_OPTIMIZE, GRegexMatchFlags (0), nullptr);
  if (g_regex_match (re, name, GRegexMatchFlags (0), nullptr))
    return TRUE;
  *reason_out = g_strdup ("filename must be lowercase a-z0-9 with -/_, ending in .svg");
  return FALSE;
}

gboolean
validate_size (const char *path, char **reason_out)
{
  GStatBuf st;
  if (g_stat (path, &st) != 0) {
    *reason_out = g_strdup_printf ("stat failed: %s", g_strerror (errno));
    return FALSE;
  }
  if (st.st_size > (goffset) kMaxBytes) {
    *reason_out = g_strdup_printf ("%lld bytes; limit is %u",
                                   (long long) st.st_size, (unsigned) kMaxBytes);
    return FALSE;
  }
  if (st.st_size == 0) {
    *reason_out = g_strdup ("file is empty");
    return FALSE;
  }
  return TRUE;
}

gboolean
validate_renderable (const char *path, char **reason_out)
{
  gs_free_error GError *err = nullptr;
  gs_unref_object GdkPixbuf *pb =
    gdk_pixbuf_new_from_file_at_size (path, kValidateSize, kValidateSize, &err);
  if (pb == nullptr) {
    *reason_out = g_strdup_printf ("not a renderable image: %s",
                                   err ? err->message : "(unknown)");
    return FALSE;
  }
  return TRUE;
}

void
write_registry (FILE *out, GPtrArray *names)
{
  fputs ("# axan icon registry — written by `axan --import-icons`.\n", out);
  fputs ("# Drop SVGs into ~/.config/axan/icons/, then re-import to refresh.\n", out);
  fputs ("# Files in that directory that aren't listed here will NOT appear\n", out);
  fputs ("# in the picker — usually because they failed validation.\n", out);
  fputs ("\n", out);

  if (names->len == 0) {
    fputs ("icons = []\n", out);
    return;
  }

  fputs ("icons = [\n", out);
  for (guint i = 0; i < names->len; i++) {
    const char *n = (const char *) g_ptr_array_index (names, i);
    fprintf (out, "  \"%s\",\n", n);
  }
  fputs ("]\n", out);
}

gboolean
parse_registry (const char *path, GPtrArray *names_out, GError **error)
{
  gs_free char *contents = nullptr;
  gsize len = 0;
  if (!g_file_get_contents (path, &contents, &len, error))
    return FALSE;

  /* Minimal parser: find `icons = [`, walk through, pick up each
   * quoted entry. Anything not in that array is ignored. Robust enough
   * for a file we own end-to-end. */
  const char *p = strstr (contents, "icons");
  if (p == nullptr) return TRUE;          /* empty / first-run */
  p = strchr (p, '[');
  if (p == nullptr) return TRUE;
  p++;

  while (*p && *p != ']') {
    while (*p && (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',' || *p == '\r'))
      p++;
    if (*p == '#') {
      while (*p && *p != '\n') p++;
      continue;
    }
    if (*p != '"') {
      if (*p == ']' || *p == '\0') break;
      p++;
      continue;
    }
    p++; /* past opening quote */
    const char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s: unterminated string in icons array", path);
      return FALSE;
    }
    g_ptr_array_add (names_out, g_strndup (start, p - start));
    p++; /* past closing quote */
  }
  return TRUE;
}

int
strcmp_qsort (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const char *const *) a, *(const char *const *) b);
}

} // namespace

void
axan_icon_rejection_free (gpointer rejection)
{
  AxanIconRejection *r = (AxanIconRejection *) rejection;
  if (r == nullptr) return;
  g_free (r->name);
  g_free (r->reason);
  g_free (r);
}

char *
axan_icons_toml_default_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "axan", "icons.toml", nullptr);
}

char *
axan_icons_toml_staging_dir (void)
{
  return g_build_filename (g_get_user_config_dir (), "axan", "icons", nullptr);
}

gboolean
axan_icons_toml_import (const char *staging_dir_in,
                       GPtrArray  *imported_out,
                       GPtrArray  *rejected_out,
                       GError    **error)
{
  gs_free char *staging = (staging_dir_in != nullptr)
    ? g_strdup (staging_dir_in)
    : axan_icons_toml_staging_dir ();

  /* Create the staging dir if missing so subsequent drops have a home. */
  g_mkdir_with_parents (staging, 0755);

  gs_unref_object GFile *dir = g_file_new_for_path (staging);
  gs_unref_object GFileEnumerator *en =
    g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               nullptr, error);
  if (en == nullptr)
    return FALSE;

  GPtrArray *accepted = g_ptr_array_new_with_free_func (g_free);

  while (TRUE) {
    gs_unref_object GFileInfo *info =
      g_file_enumerator_next_file (en, nullptr, error);
    if (info == nullptr) {
      if (error != nullptr && *error != nullptr) {
        g_ptr_array_free (accepted, TRUE);
        return FALSE;
      }
      break;
    }
    const char *name = g_file_info_get_name (info);
    if (name == nullptr || *name == '.' || *name == '\0')
      continue;
    /* Pre-filter: ignore non-svg silently so the staging dir can hold
     * incidental files (README, .gitignore, …) without spamming the
     * rejection list. */
    if (!g_str_has_suffix (name, ".svg"))
      continue;

    gs_free char *reason = nullptr;
    gs_free char *full = g_build_filename (staging, name, nullptr);

    if (!validate_filename (name, &reason) ||
        !validate_size (full, &reason) ||
        !validate_renderable (full, &reason)) {
      if (rejected_out != nullptr) {
        AxanIconRejection *r = g_new0 (AxanIconRejection, 1);
        r->name = g_strdup (name);
        r->reason = g_strdup (reason);
        g_ptr_array_add (rejected_out, r);
      }
      continue;
    }
    g_ptr_array_add (accepted, strip_svg_ext (name));
  }

  /* Sort for deterministic TOML output. */
  g_ptr_array_sort (accepted, strcmp_qsort);

  /* Write registry. */
  gs_free char *out_path = axan_icons_toml_default_path ();
  gs_free char *out_dir = g_path_get_dirname (out_path);
  g_mkdir_with_parents (out_dir, 0755);
  FILE *stream = fopen (out_path, "w");
  if (stream == nullptr) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Cannot write %s: %s", out_path, g_strerror (errno));
    g_ptr_array_free (accepted, TRUE);
    return FALSE;
  }
  write_registry (stream, accepted);
  fclose (stream);

  if (imported_out != nullptr) {
    for (guint i = 0; i < accepted->len; i++)
      g_ptr_array_add (imported_out,
                       g_strdup ((const char *) g_ptr_array_index (accepted, i)));
  }
  g_ptr_array_free (accepted, TRUE);
  return TRUE;
}

gboolean
axan_icons_toml_load (GPtrArray *names_out, GError **error)
{
  g_return_val_if_fail (names_out != nullptr, FALSE);

  gs_free char *path = axan_icons_toml_default_path ();
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return TRUE; /* first-run; empty registry */

  return parse_registry (path, names_out, error);
}

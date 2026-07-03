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
 * axan sidebar icon registry + resolver. See header for the icon-string
 * grammar and overall design.
 */

#include "config.h"

#include "terminal-sidebar-icons.hh"
#include "axan-icons-toml.hh"
#include "terminal-app.hh"
#include "terminal-schemas.hh"
#include "terminal-libgsystem.hh"
#include "axan-log.h"

#include <glib/gi18n.h>
#include <string.h>

/* Built-in icon registry. Each entry maps a stable short name (what users
 * write in the `icon` field as `builtin:NAME`) to a freedesktop symbolic
 * icon name (what GTK resolves through the active icon theme).
 *
 * The symbolic suffix triggers GTK's recoloring path so the icon takes on
 * @theme_fg_color rather than rendering as a fixed-color raster — important
 * for theme adaptability and for the active-cell highlight pass.
 *
 * Curation rationale:
 *   - `terminal`/`ssh`/`server` cover the most common shell purposes
 *   - `database`/`logs`/`build`/`test` cover frequent dev workflows
 *   - `git`/`editor`/`package`/`container` round out the dev-tool surface
 *
 * Order is **stable across releases** — additions append at the end so the
 * deterministic-hash auto-assign (piece 4) survives growth without re-
 * shuffling already-assigned icons. Don't reorder existing entries even
 * when a "more logical" grouping is obvious.
 *
 * Symbolic-icon picks are pragmatic — Adwaita doesn't ship perfectly-
 * matching icons for every role (e.g., no canonical "container" or "ssh"
 * symbolic), so we lean on the closest stand-in. When the curated set
 * outgrows freedesktop coverage, we'll ship custom SVGs as gresource. */
typedef struct {
  const char *short_name;     /* what the user writes after `builtin:` */
  const char *icon_name;      /* freedesktop name passed to GtkImage */
} BuiltinIconEntry;

static const BuiltinIconEntry s_builtin_icons[] = {
  { "terminal",  "utilities-terminal-symbolic" },
  { "ssh",       "network-server-symbolic" },
  { "server",    "network-workgroup-symbolic" },
  { "database",  "drive-multidisk-symbolic" },
  { "package",   "package-x-generic-symbolic" },
  { "container", "drive-harddisk-symbolic" },
  { "git",       "folder-remote-symbolic" },
  { "editor",    "text-editor-symbolic" },
  { "logs",      "text-x-generic-symbolic" },
  { "build",     "applications-engineering-symbolic" },
  /* emblem-default-symbolic was dropped from adwaita-icon-theme's pruned
   * symbolic set (missing on Fedora 44), which sent builtin:test to the
   * placeholder. object-select-symbolic is the surviving Adwaita check. */
  { "test",      "object-select-symbolic" },
};

#define BUILTIN_COUNT G_N_ELEMENTS (s_builtin_icons)

/* Lazily-populated NULL-terminated array of short names, returned by
 * terminal_sidebar_icon_builtin_names. Built once on first call; the
 * builtin table is static so we never need to refresh it. */
static const char **s_builtin_names_cache = nullptr;

/* Resolve "builtin:foo" → entry, or NULL when foo isn't in the registry.
 * Skips the "builtin:" prefix in-line; caller has already matched it. */
static const BuiltinIconEntry *
find_builtin (const char *short_name)
{
  if (short_name == nullptr || *short_name == '\0')
    return nullptr;
  for (guint i = 0; i < BUILTIN_COUNT; i++) {
    if (g_strcmp0 (s_builtin_icons[i].short_name, short_name) == 0)
      return &s_builtin_icons[i];
  }
  return nullptr;
}

/* Resolve a bare filename to an absolute path under ~/.config/axan/icons/.
 * Returns a newly-allocated string the caller must free. The file is NOT
 * verified to exist here — the load step will fail gracefully if it
 * doesn't, with a single log line. */
static char *
resolve_custom_icon_path (const char *filename)
{
  const char *cfg = g_get_user_config_dir ();
  return g_build_filename (cfg, "axan", "icons", filename, nullptr);
}

/* Build a GtkImage from a freedesktop icon name at pixel_size square. The
 * INVALID icon-size sentinel + set_pixel_size is the idiom for pixel-
 * precise sizing of named icons (the GtkIconSize enum is named buckets;
 * we want a concrete pixel count). */
static GtkWidget *
image_from_icon_name (const char *icon_name, int pixel_size)
{
  GtkWidget *img = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_INVALID);
  gtk_image_set_pixel_size (GTK_IMAGE (img), pixel_size);
  return img;
}

/* Whether custom-file icons should be recolored to the theme foreground.
 * Reads the global recolor-icons key (default true). Built-in icons are
 * symbolic and recolored by GTK regardless — this gate only governs the
 * file-load path. Returns true if the app/settings aren't reachable yet so
 * the documented default holds rather than silently falling to off. */
static gboolean
icon_recolor_enabled (void)
{
  TerminalApp *app = terminal_app_get ();
  if (app == nullptr)
    return TRUE;
  GSettings *settings = terminal_app_get_global_settings (app);
  if (settings == nullptr)
    return TRUE;
  return g_settings_get_boolean (settings, TERMINAL_SETTING_RECOLOR_ICONS_KEY);
}

/* Resolve the current theme's foreground text color. Uses a screen-attached,
 * label-pathed style context so the lookup reflects the active GTK theme
 * (including the light/dark variant) without needing a realized widget.
 * Falls back to opaque mid-grey if no screen is available. */
static void
icon_resolve_theme_fg (GdkRGBA *fg)
{
  fg->red = fg->green = fg->blue = 0.745; /* #bebebe fallback */
  fg->alpha = 1.0;

  GdkScreen *screen = gdk_screen_get_default ();
  if (screen == nullptr)
    return;

  GtkWidgetPath *path = gtk_widget_path_new ();
  gtk_widget_path_append_type (path, GTK_TYPE_LABEL);
  gs_unref_object GtkStyleContext *ctx = gtk_style_context_new ();
  gtk_style_context_set_screen (ctx, screen);
  gtk_style_context_set_path (ctx, path);
  gtk_widget_path_unref (path);
  gtk_style_context_get_color (ctx, GTK_STATE_FLAG_NORMAL, fg);
}

gboolean
terminal_sidebar_icon_theme_is_dark (void)
{
  GdkRGBA fg;
  icon_resolve_theme_fg (&fg);
  /* A dark theme pairs a light default text foreground with a dark
   * background; a light theme does the reverse. Threshold the fg luminance
   * (sRGB-weighted, no need to linearize for a binary decision). */
  double lum = 0.2126 * fg.red + 0.7152 * fg.green + 0.0722 * fg.blue;
  return lum > 0.5;
}

/* Return a new pixbuf with every pixel's RGB replaced by @fg, preserving the
 * source alpha as a mask — the standard symbolic recolor. The baked-in source
 * color is discarded entirely, so this works on any imported SVG/PNG (and on
 * future imports) without per-file editing. The returned pixbuf is a fresh
 * owned reference; the caller frees it. */
static GdkPixbuf *
icon_recolor_to_fg (GdkPixbuf *src, const GdkRGBA *fg)
{
  /* Guarantee an alpha channel so the mask exists, then own the result. */
  GdkPixbuf *pb = gdk_pixbuf_get_has_alpha (src)
                    ? gdk_pixbuf_copy (src)
                    : gdk_pixbuf_add_alpha (src, FALSE, 0, 0, 0);
  if (pb == nullptr)
    return nullptr;

  const guchar r = (guchar) (CLAMP (fg->red,   0.0, 1.0) * 255.0 + 0.5);
  const guchar g = (guchar) (CLAMP (fg->green, 0.0, 1.0) * 255.0 + 0.5);
  const guchar b = (guchar) (CLAMP (fg->blue,  0.0, 1.0) * 255.0 + 0.5);

  const int width = gdk_pixbuf_get_width (pb);
  const int height = gdk_pixbuf_get_height (pb);
  const int rowstride = gdk_pixbuf_get_rowstride (pb);
  const int channels = gdk_pixbuf_get_n_channels (pb); /* 4 after add_alpha */
  guchar *pixels = gdk_pixbuf_get_pixels (pb);

  for (int y = 0; y < height; y++) {
    guchar *p = pixels + y * rowstride;
    for (int x = 0; x < width; x++, p += channels) {
      p[0] = r;
      p[1] = g;
      p[2] = b;
      /* p[3] (alpha) left as the source mask */
    }
  }
  return pb;
}

/* Apply theme recoloring to a freshly-loaded custom-file pixbuf if the
 * recolor-icons setting is on. Takes ownership of @pb and returns either a
 * recolored replacement (with @pb freed) or @pb unchanged. */
static GdkPixbuf *
icon_maybe_recolor (GdkPixbuf *pb)
{
  if (pb == nullptr || !icon_recolor_enabled ())
    return pb;
  GdkRGBA fg;
  icon_resolve_theme_fg (&fg);
  GdkPixbuf *recolored = icon_recolor_to_fg (pb, &fg);
  if (recolored == nullptr)
    return pb;
  g_object_unref (pb);
  return recolored;
}

/* Build a GtkImage from an SVG/PNG file at pixel_size square. Scales via
 * gdk-pixbuf — supports any format gdk-pixbuf can load including SVG (if
 * librsvg's loader is installed). Returns NULL on any load failure so the
 * caller can fall back to the no-icon path. */
static GtkWidget *
image_from_file (const char *path, int pixel_size)
{
  gs_free_error GError *err = nullptr;
  GdkPixbuf *pb =
    gdk_pixbuf_new_from_file_at_size (path, pixel_size, pixel_size, &err);
  if (pb == nullptr) {
    axan_log_warn ("sidebar.icons",
                   "couldn't load icon at %s: %s",
                   path, err ? err->message : "unknown error");
    return nullptr;
  }
  pb = icon_maybe_recolor (pb);
  GtkWidget *img = gtk_image_new_from_pixbuf (pb);
  g_object_unref (pb);
  return img;
}

GtkWidget *
terminal_sidebar_icon_resolve (const char *icon_id, int pixel_size)
{
  if (icon_id == nullptr || *icon_id == '\0')
    return nullptr;
  if (pixel_size <= 0)
    pixel_size = 16; /* sane fallback; cells request real sizes */

  /* builtin:NAME — look up in the curated registry. */
  if (g_str_has_prefix (icon_id, "builtin:")) {
    const char *short_name = icon_id + strlen ("builtin:");
    const BuiltinIconEntry *entry = find_builtin (short_name);
    if (entry == nullptr) {
      axan_log_warn ("sidebar.icons",
                     "unknown builtin icon '%s' — falling back to no icon",
                     short_name);
      return nullptr;
    }
    return image_from_icon_name (entry->icon_name, pixel_size);
  }

  /* Absolute path — load directly. */
  if (g_path_is_absolute (icon_id))
    return image_from_file (icon_id, pixel_size);

  /* Bare filename — resolve against ~/.config/axan/icons/. */
  gs_free char *path = resolve_custom_icon_path (icon_id);
  return image_from_file (path, pixel_size);
}

GtkWidget *
terminal_sidebar_icon_resolve_colored (const char *icon_id, int pixel_size, const char *hex)
{
  /* Empty/unparseable color → the unchanged theme-tracked path (a GtkImage
   * from the icon name auto-recolors with the active theme via its style
   * context, which a fixed-pixbuf recolor would forfeit). Only when a real
   * per-node color is set do we take the pixbuf-recolor branch below. */
  GdkRGBA color;
  if (hex == nullptr || *hex == '\0' || !gdk_rgba_parse (&color, hex))
    return terminal_sidebar_icon_resolve (icon_id, pixel_size);

  if (icon_id == nullptr || *icon_id == '\0')
    return nullptr;
  if (pixel_size <= 0)
    pixel_size = 16;

  /* builtin:NAME — symbolic glyph. Paint it in the node color (overriding
   * the theme foreground the resolve-to-pixbuf path would use). */
  if (g_str_has_prefix (icon_id, "builtin:")) {
    const char *short_name = icon_id + strlen ("builtin:");
    const BuiltinIconEntry *entry = find_builtin (short_name);
    if (entry == nullptr)
      return nullptr;
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    GdkPixbuf *pb = gtk_icon_theme_load_icon (theme, entry->icon_name,
                                              pixel_size,
                                              GTK_ICON_LOOKUP_FORCE_SYMBOLIC,
                                              nullptr);
    if (pb == nullptr)
      return nullptr;
    GdkPixbuf *tinted = icon_recolor_to_fg (pb, &color);
    g_object_unref (pb);
    if (tinted == nullptr)
      return nullptr;
    GtkWidget *img = gtk_image_new_from_pixbuf (tinted);
    g_object_unref (tinted);
    return img;
  }

  /* Custom image file (absolute path or bare filename) — "image files keep
   * their own colors" (per the Apply-color-to hint): load the raw pixels
   * without any recolor, ignoring the node color and the recolor-icons
   * setting both. The node color only governs symbolic glyphs. */
  const char *path = icon_id;
  gs_free char *resolved = nullptr;
  if (!g_path_is_absolute (icon_id)) {
    resolved = resolve_custom_icon_path (icon_id);
    path = resolved;
  }
  gs_unref_object GdkPixbuf *pb =
    gdk_pixbuf_new_from_file_at_size (path, pixel_size, pixel_size, nullptr);
  if (pb == nullptr)
    return nullptr;
  return gtk_image_new_from_pixbuf (pb);
}

const char *const *
terminal_sidebar_icon_builtin_names (void)
{
  if (s_builtin_names_cache == nullptr) {
    /* One-time build: NULL-terminated dup of the short_name pointers.
     * The strings themselves are static const, so we don't strdup. */
    s_builtin_names_cache = g_new0 (const char *, BUILTIN_COUNT + 1);
    for (guint i = 0; i < BUILTIN_COUNT; i++)
      s_builtin_names_cache[i] = s_builtin_icons[i].short_name;
    s_builtin_names_cache[BUILTIN_COUNT] = nullptr;
  }
  return s_builtin_names_cache;
}

guint
terminal_sidebar_icon_builtin_count (void)
{
  return BUILTIN_COUNT;
}

const char *
terminal_sidebar_icon_auto_assign (const char *seed)
{
  if (seed == nullptr || *seed == '\0')
    return nullptr;
  /* g_str_hash is a Murmur-style stable hash; modding by BUILTIN_COUNT
   * partitions the seed space evenly across the curated set. Since the
   * builtin list is append-only (see s_builtin_icons documentation), an
   * existing seed keeps its assigned icon across axan upgrades that add
   * new builtins — the modulo result drifts only when the count grows
   * past where the seed's hash falls. That's acceptable; users who want
   * absolute stability set an explicit icon. */
  guint h = g_str_hash (seed);
  return s_builtin_icons[h % BUILTIN_COUNT].short_name;
}

GdkPixbuf *
terminal_sidebar_icon_resolve_to_pixbuf (const char *icon_id, int pixel_size)
{
  if (icon_id == nullptr || *icon_id == '\0')
    return nullptr;
  if (pixel_size <= 0)
    pixel_size = 16;

  /* builtin:NAME — symbolic icons go through the icon theme rather than
   * a file load. We force-symbolic so the recoloring path applies (same
   * as the icon-name path in terminal_sidebar_icon_resolve) even when
   * the loader fallback would prefer a non-symbolic variant. */
  if (g_str_has_prefix (icon_id, "builtin:")) {
    const char *short_name = icon_id + strlen ("builtin:");
    const BuiltinIconEntry *entry = find_builtin (short_name);
    if (entry == nullptr)
      return nullptr;
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    gs_free_error GError *err = nullptr;
    GdkPixbuf *pb = gtk_icon_theme_load_icon (theme, entry->icon_name,
                                              pixel_size,
                                              GTK_ICON_LOOKUP_FORCE_SYMBOLIC,
                                              &err);
    if (pb == nullptr) {
      axan_log_warn ("sidebar.icons",
                     "couldn't load builtin icon '%s' (%s): %s",
                     short_name, entry->icon_name,
                     err ? err->message : "unknown error");
      return nullptr;
    }
    /* gtk_icon_theme_load_icon bakes the symbolic glyph in GTK's default
     * symbolic color, which ignores the active theme's foreground — in a
     * dark menu the icons come out dark and mismatch the label beside them.
     * The GtkImage-from-icon-name path (terminal_sidebar_icon_resolve) gets
     * per-state recoloring from the widget's style context for free; this
     * raw-pixbuf path, used by the right-click icon picker menu, does not.
     * Tint to the resolved theme fg so the builtin icons track the text
     * color. Unconditional — builtins are symbolic by definition, so this is
     * independent of the recolor-icons setting that governs custom files. */
    GdkRGBA fg;
    icon_resolve_theme_fg (&fg);
    GdkPixbuf *tinted = icon_recolor_to_fg (pb, &fg);
    if (tinted != nullptr) {
      g_object_unref (pb);
      pb = tinted;
    }
    return pb;
  }

  /* Absolute path — load directly. */
  if (g_path_is_absolute (icon_id)) {
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size (icon_id, pixel_size, pixel_size, nullptr);
    return icon_maybe_recolor (pb);
  }

  /* Bare filename — resolve against ~/.config/axan/icons/. */
  gs_free char *path = resolve_custom_icon_path (icon_id);
  GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size (path, pixel_size, pixel_size, nullptr);
  return icon_maybe_recolor (pb);
}

/* Track ids we've already emitted during enumeration so the `extra`
 * dedupe at the end is O(1) per check rather than re-walking emissions. */
typedef struct {
  TerminalSidebarIconOptionCb cb;
  gpointer user_data;
  GHashTable *seen; /* str → unowned sentinel */
} EnumerateCtx;

static void
enumerate_emit (EnumerateCtx *ctx, const char *id, const char *label)
{
  ctx->cb (id, label, ctx->user_data);
  g_hash_table_add (ctx->seen, g_strdup (id ? id : ""));
}

/* Emit one row per icon listed in ~/.config/axan/icons.toml. The bare
 * filename (e.g. "docker.svg") is the icon-string the resolver expects,
 * so we re-attach .svg to the registered name. Raw files in the staging
 * folder that haven't been imported through axan_icons_toml_import are
 * intentionally NOT emitted — the registry is the validation gate. */
static void
enumerate_custom_files (EnumerateCtx *ctx)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  gs_free_error GError *err = nullptr;
  if (!axan_icons_toml_load (names, &err)) {
    g_ptr_array_free (names, TRUE);
    return;
  }
  for (guint i = 0; i < names->len; i++) {
    const char *bare = (const char *) g_ptr_array_index (names, i);
    gs_free char *filename = g_strconcat (bare, ".svg", nullptr);
    enumerate_emit (ctx, filename, bare);
  }
  g_ptr_array_free (names, TRUE);
}

void
terminal_sidebar_icon_enumerate_options (const char *extra,
                                         TerminalSidebarIconOptionCb cb,
                                         gpointer user_data)
{
  if (cb == nullptr)
    return;

  EnumerateCtx ctx;
  ctx.cb = cb;
  ctx.user_data = user_data;
  ctx.seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);

  enumerate_emit (&ctx, "", _("(none)"));

  for (guint i = 0; i < BUILTIN_COUNT; i++) {
    gs_free char *id_str = g_strconcat ("builtin:", s_builtin_icons[i].short_name, nullptr);
    gs_free char *label = g_strdup (s_builtin_icons[i].short_name);
    if (label != nullptr && *label != '\0')
      label[0] = g_ascii_toupper (label[0]);
    enumerate_emit (&ctx, id_str, label ? label : s_builtin_icons[i].short_name);
  }

  enumerate_custom_files (&ctx);

  if (extra != nullptr && *extra != '\0' && !g_hash_table_contains (ctx.seen, extra))
    enumerate_emit (&ctx, extra, extra);

  g_hash_table_destroy (ctx.seen);
}

/* Column indexes for the picker model. The id column is what
 * gtk_combo_box_set_id_column expects — a plain string the user-supplied
 * icon value round-trips through (and the public API of get_active_id /
 * set_active_id keys off). */
enum {
  COMBO_COL_PIXBUF = 0,
  COMBO_COL_LABEL  = 1,
  COMBO_COL_ID     = 2,
  COMBO_N_COLS
};

/* Append one row to the picker model. The pixbuf reference is taken by
 * gtk_list_store_set (the store retains its own ref), so the local one
 * goes through gs_unref_object. */
static void
combo_append_row (GtkListStore *store, const char *id, const char *label, int icon_size)
{
  gs_unref_object GdkPixbuf *pb = terminal_sidebar_icon_resolve_to_pixbuf (id, icon_size);
  GtkTreeIter iter;
  gtk_list_store_append (store, &iter);
  gtk_list_store_set (store, &iter,
                      COMBO_COL_PIXBUF, pb,
                      COMBO_COL_LABEL,  label,
                      COMBO_COL_ID,     id ? id : "",
                      -1);
}

/* Callback handed to enumerate_options when populating the combobox.
 * Resolves a pixbuf at the model's icon size and appends a model row. */
typedef struct {
  GtkListStore *store;
  int icon_size;
} ComboBuildCtx;

static void
combo_collect_option (const char *id, const char *label, gpointer user_data)
{
  ComboBuildCtx *ctx = (ComboBuildCtx *) user_data;
  combo_append_row (ctx->store, id, label, ctx->icon_size);
}

GtkWidget *
terminal_sidebar_icon_combo_new (const char *current)
{
  /* 16px is enough that a freedesktop symbolic icon reads clearly at
   * the row height GtkComboBox lays out by default (~24-28px including
   * padding). Bigger values inflate the popdown rows and the visible
   * cell. */
  const int icon_size = 16;

  gs_unref_object GtkListStore *store =
    gtk_list_store_new (COMBO_N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);

  ComboBuildCtx ctx = { store, icon_size };
  terminal_sidebar_icon_enumerate_options (current, combo_collect_option, &ctx);

  GtkWidget *combo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (store));

  /* Pixbuf renderer first so the icon sits to the left of the label.
   * expand=FALSE so it takes only the icon's natural width. */
  GtkCellRenderer *pb_renderer = gtk_cell_renderer_pixbuf_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), pb_renderer, FALSE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), pb_renderer,
                                  "pixbuf", COMBO_COL_PIXBUF, nullptr);

  /* Add a small gap between icon and label by giving the pixbuf renderer
   * right-side padding. Looks balanced against the combobox's own arrow
   * padding on the other side. */
  g_object_set (pb_renderer, "xpad", 4, nullptr);

  GtkCellRenderer *txt_renderer = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), txt_renderer, TRUE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), txt_renderer,
                                  "text", COMBO_COL_LABEL, nullptr);

  gtk_combo_box_set_id_column (GTK_COMBO_BOX (combo), COMBO_COL_ID);

  /* set_active_id silently no-ops on a missing id; we appended `current`
   * above whenever it didn't already exist, so this branch always finds
   * a hit. The empty string is a legitimate id (the "(none)" entry). */
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), current ? current : "");

  return combo;
}

/*
 * Copyright © 2002 Havoc Pennington
 * Copyright © 2002 Mathias Hasselmann
 * Copyright © 2008, 2011, 2017 Christian Persch
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

#include <config.h>

#include <string.h>
#include <math.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "axan-log.h"
#include "terminal-app.hh"
#include "terminal-defines.hh"
#include "terminal-enums.hh"
#include "terminal-gdbus-generated.h"
#include "profile-editor.hh"
#include "terminal-prefs.hh"
#include "terminal-schemas.hh"
#include "terminal-type-builtins.hh"
#include "terminal-util.hh"
#include "terminal-profiles-list.hh"
#include "terminal-libgsystem.hh"
#include "terminal-sidebar-icons.hh"


/* Wrapper around g_signal_connect that maintains a list of the
 * handlers installed, and can disconnect them all. */
typedef struct {
  gpointer instance;
  gulong handler_id;
} ProfilePrefsSignal;

static void
profile_prefs_register_signal_handler (gpointer instance,
                                       gulong handler_id)
{
  ProfilePrefsSignal sig;
  sig.instance = instance;
  sig.handler_id = handler_id;
  g_array_append_val (the_pref_data->profile_signals, sig);
}

static gulong
profile_prefs_signal_connect (gpointer instance,
                              const gchar *detailed_signal,
                              GCallback c_handler,
                              gpointer data)
{
  gulong handler_id = g_signal_connect(instance, detailed_signal, c_handler, data);
  profile_prefs_register_signal_handler (instance, handler_id);
  return handler_id;
}

static void
profile_prefs_signal_handlers_disconnect_all (void)
{
  for (guint i = 0; i < the_pref_data->profile_signals->len; i++) {
    ProfilePrefsSignal *sig = &g_array_index (the_pref_data->profile_signals, ProfilePrefsSignal, i);
    g_signal_handler_disconnect (sig->instance, sig->handler_id);
  }
  g_array_set_size (the_pref_data->profile_signals, 0);
}


/* Wrappers around g_settings_bind and friends that maintain a list of the
 * bindings installed, and can unbind them all. */
typedef struct {
  gpointer object;
  char *property;
} ProfilePrefsBinding;

static void
profile_prefs_register_settings_binding (gpointer object,
                                         const char *property)
{
  ProfilePrefsBinding bind;
  bind.object = object;
  bind.property = g_strdup (property);
  g_array_append_val (the_pref_data->profile_bindings, bind);
}

static void
profile_prefs_settings_bind (GSettings *settings,
                             const gchar *key,
                             gpointer object,
                             const gchar *property,
                             GSettingsBindFlags flags)
{
  profile_prefs_register_settings_binding (object, property);
  g_settings_bind (settings, key, object, property, flags);
}

static void
profile_prefs_settings_bind_with_mapping (GSettings *settings,
                                          const gchar *key,
                                          gpointer object,
                                          const gchar *property,
                                          GSettingsBindFlags flags,
                                          GSettingsBindGetMapping get_mapping,
                                          GSettingsBindSetMapping set_mapping,
                                          GType (*user_data)(void),
                                          GDestroyNotify destroy)
{
  profile_prefs_register_settings_binding (object, property);
  g_settings_bind_with_mapping (settings, key, object, property, flags, get_mapping, set_mapping, (void*)user_data, destroy);
}

static void
profile_prefs_settings_bind_writable (GSettings *settings,
                                      const gchar *key,
                                      gpointer object,
                                      const gchar *property,
                                      gboolean inverted)
{
  profile_prefs_register_settings_binding (object, property);
  g_settings_bind_writable (settings, key, object, property, inverted);
}

static void
profile_prefs_settings_unbind_all (void)
{
  for (guint i = 0; i < the_pref_data->profile_bindings->len; i++) {
    ProfilePrefsBinding *bind = &g_array_index (the_pref_data->profile_bindings, ProfilePrefsBinding, i);
    g_settings_unbind (bind->object, bind->property);
    g_free (bind->property);
  }
  g_array_set_size (the_pref_data->profile_bindings, 0);
}


typedef struct _TerminalColorScheme TerminalColorScheme;

struct _TerminalColorScheme
{
  const char *name;
  const GdkRGBA foreground;
  const GdkRGBA background;
};

#define COLOR(r, g, b) { .red = (r) / 255.0, .green = (g) / 255.0, .blue = (b) / 255.0, .alpha = 1.0 }

static const TerminalColorScheme color_schemes[] = {
  { N_("Black on light yellow"),
    COLOR (0x00, 0x00, 0x00),
    COLOR (0xff, 0xff, 0xdd)
  },
  { N_("Black on white"),
    COLOR (0x00, 0x00, 0x00),
    COLOR (0xff, 0xff, 0xff)
  },
  { N_("Gray on black"),
    COLOR (0xaa, 0xaa, 0xaa),
    COLOR (0x00, 0x00, 0x00)
  },
  { N_("Green on black"),
    COLOR (0x00, 0xff, 0x00),
    COLOR (0x00, 0x00, 0x00)
  },
  { N_("White on black"),
    COLOR (0xff, 0xff, 0xff),
    COLOR (0x00, 0x00, 0x00)
  },
  /* Translators: "GNOME" is the name of a colour scheme, "light" can be translated */
  { N_("GNOME light"),
    COLOR (0x17, 0x14, 0x21), /* Palette entry 0 */
    COLOR (0xff, 0xff, 0xff)  /* Palette entry 15 */
  },
  /* Translators: "GNOME" is the name of a colour scheme, "dark" can be translated */
  { N_("GNOME dark"),
    COLOR (0xd0, 0xcf, 0xcc), /* Palette entry 7 */
    COLOR (0x17, 0x14, 0x21)  /* Palette entry 0 */
  },
  /* Translators: "Tango" is the name of a colour scheme, "light" can be translated */
  { N_("Tango light"),
    COLOR (0x2e, 0x34, 0x36),
    COLOR (0xee, 0xee, 0xec)
  },
  /* Translators: "Tango" is the name of a colour scheme, "dark" can be translated */
  { N_("Tango dark"),
    COLOR (0xd3, 0xd7, 0xcf),
    COLOR (0x2e, 0x34, 0x36)
  },
  /* Translators: "Solarized" is the name of a colour scheme, "light" can be translated */
  { N_("Solarized light"),
    COLOR (0x65, 0x7b, 0x83),  /* 11: base00 */
    COLOR (0xfd, 0xf6, 0xe3)   /* 15: base3  */
  },
  /* Translators: "Solarized" is the name of a colour scheme, "dark" can be translated */
  { N_("Solarized dark"),
    COLOR (0x83, 0x94, 0x96),  /* 12: base0  */
    COLOR (0x00, 0x2b, 0x36)   /*  8: base03 */
  },
};

#define TERMINAL_PALETTE_SIZE (16)

enum
{
  TERMINAL_PALETTE_GNOME     = 0,
  TERMINAL_PALETTE_TANGO     = 1,
  TERMINAL_PALETTE_LINUX     = 2,
  TERMINAL_PALETTE_XTERM     = 3,
  TERMINAL_PALETTE_RXVT      = 4,
  TERMINAL_PALETTE_SOLARIZED = 5,
  TERMINAL_PALETTE_N_BUILTINS
};

static const GdkRGBA terminal_palettes[TERMINAL_PALETTE_N_BUILTINS][TERMINAL_PALETTE_SIZE] =
{
  /* Based on GNOME 3.32 palette: https://developer.gnome.org/hig/stable/icon-design.html.en#palette */
  {
    COLOR (0x17, 0x14, 0x21),  /* Blend of Dark 4 and Black */
    COLOR (0xc0, 0x1c, 0x28),  /* Red 4 */
    COLOR (0x26, 0xa2, 0x69),  /* Green 5 */
    COLOR (0xa2, 0x73, 0x4c),  /* Blend of Brown 2 and Brown 3 */
    COLOR (0x12, 0x48, 0x8b),  /* Blend of Blue 5 and Dark 4 */
    COLOR (0xa3, 0x47, 0xba),  /* Purple 3 */
    COLOR (0x2a, 0xa1, 0xb3),  /* Linear addition Blue 5 + Green 5, darkened slightly */
    COLOR (0xd0, 0xcf, 0xcc),  /* Blend of Light 3 and Light 4 */
    COLOR (0x5e, 0x5c, 0x64),  /* Dark 2 */
    COLOR (0xf6, 0x61, 0x51),  /* Red 1 */
    COLOR (0x33, 0xd1, 0x7a),  /* Green 3 */
    COLOR (0xe9, 0xad, 0x0c),  /* Blend of Yellow 4 and Yellow 5 */
    COLOR (0x2a, 0x7b, 0xde),  /* Blend of Blue 3 and Blue 4 */
    COLOR (0xc0, 0x61, 0xcb),  /* Purple 2 */
    COLOR (0x33, 0xc7, 0xde),  /* Linear addition Blue 4 + Green 4, darkened slightly */
    COLOR (0xff, 0xff, 0xff)   /* Light 1 */
  },

  /* Tango palette */
  {
    COLOR (0x2e, 0x34, 0x36),
    COLOR (0xcc, 0x00, 0x00),
    COLOR (0x4e, 0x9a, 0x06),
    COLOR (0xc4, 0xa0, 0x00),
    COLOR (0x34, 0x65, 0xa4),
    COLOR (0x75, 0x50, 0x7b),
    COLOR (0x06, 0x98, 0x9a),
    COLOR (0xd3, 0xd7, 0xcf),
    COLOR (0x55, 0x57, 0x53),
    COLOR (0xef, 0x29, 0x29),
    COLOR (0x8a, 0xe2, 0x34),
    COLOR (0xfc, 0xe9, 0x4f),
    COLOR (0x72, 0x9f, 0xcf),
    COLOR (0xad, 0x7f, 0xa8),
    COLOR (0x34, 0xe2, 0xe2),
    COLOR (0xee, 0xee, 0xec)
  },

  /* Linux palette */
  {
    COLOR (0x00, 0x00, 0x00),
    COLOR (0xaa, 0x00, 0x00),
    COLOR (0x00, 0xaa, 0x00),
    COLOR (0xaa, 0x55, 0x00),
    COLOR (0x00, 0x00, 0xaa),
    COLOR (0xaa, 0x00, 0xaa),
    COLOR (0x00, 0xaa, 0xaa),
    COLOR (0xaa, 0xaa, 0xaa),
    COLOR (0x55, 0x55, 0x55),
    COLOR (0xff, 0x55, 0x55),
    COLOR (0x55, 0xff, 0x55),
    COLOR (0xff, 0xff, 0x55),
    COLOR (0x55, 0x55, 0xff),
    COLOR (0xff, 0x55, 0xff),
    COLOR (0x55, 0xff, 0xff),
    COLOR (0xff, 0xff, 0xff)
  },

  /* XTerm palette */
  {
    COLOR (0x00, 0x00, 0x00),
    COLOR (0xcd, 0x00, 0x00),
    COLOR (0x00, 0xcd, 0x00),
    COLOR (0xcd, 0xcd, 0x00),
    COLOR (0x00, 0x00, 0xee),
    COLOR (0xcd, 0x00, 0xcd),
    COLOR (0x00, 0xcd, 0xcd),
    COLOR (0xe5, 0xe5, 0xe5),
    COLOR (0x7f, 0x7f, 0x7f),
    COLOR (0xff, 0x00, 0x00),
    COLOR (0x00, 0xff, 0x00),
    COLOR (0xff, 0xff, 0x00),
    COLOR (0x5c, 0x5c, 0xff),
    COLOR (0xff, 0x00, 0xff),
    COLOR (0x00, 0xff, 0xff),
    COLOR (0xff, 0xff, 0xff)
  },

  /* RXVT palette */
  {
    COLOR (0x00, 0x00, 0x00),
    COLOR (0xcd, 0x00, 0x00),
    COLOR (0x00, 0xcd, 0x00),
    COLOR (0xcd, 0xcd, 0x00),
    COLOR (0x00, 0x00, 0xcd),
    COLOR (0xcd, 0x00, 0xcd),
    COLOR (0x00, 0xcd, 0xcd),
    COLOR (0xfa, 0xeb, 0xd7),
    COLOR (0x40, 0x40, 0x40),
    COLOR (0xff, 0x00, 0x00),
    COLOR (0x00, 0xff, 0x00),
    COLOR (0xff, 0xff, 0x00),
    COLOR (0x00, 0x00, 0xff),
    COLOR (0xff, 0x00, 0xff),
    COLOR (0x00, 0xff, 0xff),
    COLOR (0xff, 0xff, 0xff)
  },

  /* Solarized palette (1.0.0beta2): http://ethanschoonover.com/solarized */
  {
    COLOR (0x07, 0x36, 0x42),  /*  0: base02  */
    COLOR (0xdc, 0x32, 0x2f),  /*  1: red     */
    COLOR (0x85, 0x99, 0x00),  /*  2: green   */
    COLOR (0xb5, 0x89, 0x00),  /*  3: yellow  */
    COLOR (0x26, 0x8b, 0xd2),  /*  4: blue    */
    COLOR (0xd3, 0x36, 0x82),  /*  5: magenta */
    COLOR (0x2a, 0xa1, 0x98),  /*  6: cyan    */
    COLOR (0xee, 0xe8, 0xd5),  /*  7: base2   */
    COLOR (0x00, 0x2b, 0x36),  /*  8: base03  */
    COLOR (0xcb, 0x4b, 0x16),  /*  9: orange  */
    COLOR (0x58, 0x6e, 0x75),  /* 10: base01  */
    COLOR (0x65, 0x7b, 0x83),  /* 11: base00  */
    COLOR (0x83, 0x94, 0x96),  /* 12: base0   */
    COLOR (0x6c, 0x71, 0xc4),  /* 13: violet  */
    COLOR (0x93, 0xa1, 0xa1),  /* 14: base1   */
    COLOR (0xfd, 0xf6, 0xe3)   /* 15: base3   */
  },
};

#undef COLOR

static void profile_colors_notify_scheme_combo_cb (GSettings *profile,
                                                   const char *key,
                                                   GtkComboBox *combo);

static void profile_palette_notify_scheme_combo_cb (GSettings *profile,
                                                    const char *key,
                                                    GtkComboBox *combo);

static void profile_palette_notify_colorpickers_cb (GSettings *profile,
                                                    const char *key,
                                                    gpointer user_data);

static void profile_notify_encoding_combo_cb (GSettings *profile,
                                              const char *key,
                                              GtkComboBox *combo);

enum {
        ENCODINGS_COL_ID,
        ENCODINGS_COL_TEXT
};

/* gdk_rgba_equal is too strict! */
static gboolean
rgba_equal (const GdkRGBA *a,
            const GdkRGBA *b)
{
  gdouble dr, dg, db;

  dr = a->red - b->red;
  dg = a->green - b->green;
  db = a->blue - b->blue;

  return (dr * dr + dg * dg + db * db) < 1e-4;
}

static gboolean
palette_cmp (const GdkRGBA *ca,
             const GdkRGBA *cb)
{
  guint i;

  for (i = 0; i < TERMINAL_PALETTE_SIZE; ++i)
    if (!rgba_equal (&ca[i], &cb[i]))
      return FALSE;

  return TRUE;
}

static gboolean
palette_is_builtin (const GdkRGBA *colors,
                    gsize n_colors,
                    guint *n)
{
  guint i;

  if (n_colors != TERMINAL_PALETTE_SIZE)
    return FALSE;

  for (i = 0; i < TERMINAL_PALETTE_N_BUILTINS; ++i)
    {
      if (palette_cmp (colors, terminal_palettes[i]))
        {
          *n = i;
          return TRUE;
        }
    }

  return FALSE;
}

static void
modify_palette_entry (GSettings       *profile,
                      guint            i,
                      const GdkRGBA   *color)
{
  gs_free GdkRGBA *colors;
  gsize n_colors;

  /* FIXMEchpe: this can be optimised, don't really need to parse the colours! */

  colors = terminal_g_settings_get_rgba_palette (profile, TERMINAL_PROFILE_PALETTE_KEY, &n_colors);

  if (i < n_colors)
    {
      colors[i] = *color;
      terminal_g_settings_set_rgba_palette (profile, TERMINAL_PROFILE_PALETTE_KEY,
                                            colors, n_colors);
    }
}

static void
color_scheme_combo_changed_cb (GtkWidget *combo,
                               GParamSpec *pspec,
                               GSettings *profile)
{
  guint i;

  i = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

  if (i < G_N_ELEMENTS (color_schemes))
    {
      g_signal_handlers_block_by_func (profile, (void*)profile_colors_notify_scheme_combo_cb, combo);
      terminal_g_settings_set_rgba (profile, TERMINAL_PROFILE_FOREGROUND_COLOR_KEY, &color_schemes[i].foreground);
      terminal_g_settings_set_rgba (profile, TERMINAL_PROFILE_BACKGROUND_COLOR_KEY, &color_schemes[i].background);
      g_signal_handlers_unblock_by_func (profile, (void*)profile_colors_notify_scheme_combo_cb, combo);
    }
  else
    {
      /* "custom" selected, no change */
    }
}

static void
profile_colors_notify_scheme_combo_cb (GSettings *profile,
                                       const char *key,
                                       GtkComboBox *combo)
{
  GdkRGBA fg, bg;
  guint i;

  terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_FOREGROUND_COLOR_KEY, &fg);
  terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_BACKGROUND_COLOR_KEY, &bg);

  for (i = 0; i < G_N_ELEMENTS (color_schemes); ++i)
    {
      if (rgba_equal (&fg, &color_schemes[i].foreground) &&
          rgba_equal (&bg, &color_schemes[i].background))
        break;
    }
  /* If we didn't find a match, then we get the last combo box item which is "custom" */

  g_signal_handlers_block_by_func (combo, (void*)color_scheme_combo_changed_cb, profile);
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), i);
  g_signal_handlers_unblock_by_func (combo, (void*)color_scheme_combo_changed_cb, profile);
}

static void
palette_scheme_combo_changed_cb (GtkComboBox *combo,
                                 GParamSpec *pspec,
                                 GSettings *profile)
{
  int i;

  i = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

  g_signal_handlers_block_by_func (profile, (void*)profile_colors_notify_scheme_combo_cb, combo);
  if (i < TERMINAL_PALETTE_N_BUILTINS)
    terminal_g_settings_set_rgba_palette (profile, TERMINAL_PROFILE_PALETTE_KEY,
                                          terminal_palettes[i], TERMINAL_PALETTE_SIZE);
  else
    {
      /* "custom" selected, no change */
    }
  g_signal_handlers_unblock_by_func (profile, (void*)profile_colors_notify_scheme_combo_cb, combo);
}

static void
profile_palette_notify_scheme_combo_cb (GSettings *profile,
                                        const char *key,
                                        GtkComboBox *combo)
{
  gs_free GdkRGBA *colors;
  gsize n_colors;
  guint i;

  colors = terminal_g_settings_get_rgba_palette (profile, TERMINAL_PROFILE_PALETTE_KEY, &n_colors);
  if (!palette_is_builtin (colors, n_colors, &i))
    /* If we didn't find a match, then we want the last combo
     * box item which is "custom"
     */
    i = TERMINAL_PALETTE_N_BUILTINS;

  g_signal_handlers_block_by_func (combo, (void*)palette_scheme_combo_changed_cb, profile);
  gtk_combo_box_set_active (combo, i);
  g_signal_handlers_unblock_by_func (combo, (void*)palette_scheme_combo_changed_cb, profile);
}

static void
palette_color_notify_cb (GtkColorButton *button,
                         GParamSpec *pspec,
                         GSettings *profile)
{
  GdkRGBA color;
  guint i;

  gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (button), &color);
  i = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "palette-entry-index"));

  g_signal_handlers_block_by_func (profile, (void*)profile_palette_notify_colorpickers_cb, nullptr);
  modify_palette_entry (profile, i, &color);
  g_signal_handlers_unblock_by_func (profile, (void*)profile_palette_notify_colorpickers_cb, nullptr);
}

static void
profile_palette_notify_colorpickers_cb (GSettings *profile,
                                        const char *key,
                                        gpointer user_data)
{
  GtkWidget *w;
  GtkBuilder *builder = the_pref_data->builder;
  gs_free GdkRGBA *colors;
  gsize n_colors, i;

  g_assert (strcmp (key, TERMINAL_PROFILE_PALETTE_KEY) == 0);

  colors = terminal_g_settings_get_rgba_palette (profile, TERMINAL_PROFILE_PALETTE_KEY, &n_colors);

  n_colors = MIN (n_colors, TERMINAL_PALETTE_SIZE);
  for (i = 0; i < n_colors; i++)
    {
      char name[32];

      g_snprintf (name, sizeof (name), "palette-colorpicker-%" G_GSIZE_FORMAT, i);
      w = (GtkWidget *) gtk_builder_get_object (builder, name);

      g_signal_handlers_block_by_func (w, (void*)palette_color_notify_cb, profile);
      gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (w), &colors[i]);
      g_signal_handlers_unblock_by_func (w, (void*)palette_color_notify_cb, profile);
    }
}

static void
profile_scrollback_warning_update_cb (GSettings *profile,
                                      const char *key,
                                      GtkWidget *infobar)
{
  gboolean unlimited = g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY);
  gint lines = g_settings_get_int (profile, TERMINAL_PROFILE_SCROLLBACK_LINES_KEY);

  gtk_widget_set_visible (infobar, unlimited || lines >= 1000000);
}

static void
custom_command_entry_changed_cb (GtkEntry *entry)
{
  const char *command;
  gs_free_error GError *error = nullptr;

  command = gtk_entry_get_text (entry);

  if (command[0] == '\0' ||
      g_shell_parse_argv (command, nullptr, nullptr, &error))
    {
      gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, nullptr);
    }
  else
    {
      gs_free char *tooltip;

      gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, "dialog-warning");

      tooltip = g_strdup_printf (_("Error parsing command: %s"), error->message);
      gtk_entry_set_icon_tooltip_text (entry, GTK_ENTRY_ICON_SECONDARY, tooltip);
    }
}

static void
default_size_reset_cb (GtkWidget *button,
                       GSettings *profile)
{
  g_settings_reset (profile, TERMINAL_PROFILE_DEFAULT_SIZE_COLUMNS_KEY);
  g_settings_reset (profile, TERMINAL_PROFILE_DEFAULT_SIZE_ROWS_KEY);
}

static void
cell_scale_reset_cb (GtkWidget *button,
                     GSettings *profile)
{
  g_settings_reset (profile, TERMINAL_PROFILE_CELL_HEIGHT_SCALE_KEY);
  g_settings_reset (profile, TERMINAL_PROFILE_CELL_WIDTH_SCALE_KEY);
}

static void
reset_compat_defaults_cb (GtkWidget *button,
                          GSettings *profile)
{
  g_settings_reset (profile, TERMINAL_PROFILE_DELETE_BINDING_KEY);
  g_settings_reset (profile, TERMINAL_PROFILE_BACKSPACE_BINDING_KEY);
  g_settings_reset (profile, TERMINAL_PROFILE_ENCODING_KEY);
  g_settings_reset (profile, TERMINAL_PROFILE_CJK_UTF8_AMBIGUOUS_WIDTH_KEY);
  g_settings_reset (profile, TERMINAL_PROFILE_ENABLE_SIXEL_KEY);
}

static gboolean
tree_model_id_to_iter_recurse (GtkTreeModel *model,
                               int id_column,
                               const char *active_id,
                               GtkTreeIter *iter,
                               GtkTreeIter *result_iter)
{
  do {
    /* Descend the tree */
    GtkTreeIter child_iter;
    if (gtk_tree_model_iter_children(model, &child_iter, iter) &&
        tree_model_id_to_iter_recurse (model, id_column, active_id, &child_iter, result_iter))
      return TRUE;

    gs_free char *id = nullptr;
    gtk_tree_model_get (model, iter, id_column, &id, -1);
    if (g_strcmp0 (id, active_id) == 0) {
      *result_iter = *iter;
      return TRUE;
    }
  } while (gtk_tree_model_iter_next (model, iter));

  return FALSE;
}

static gboolean
tree_model_id_to_iter (GtkTreeModel *model,
                       int id_column,
                       const char *active_id,
                       GtkTreeIter *iter)
{
  GtkTreeIter first_iter;

  return gtk_tree_model_get_iter_first(model, &first_iter) &&
    tree_model_id_to_iter_recurse(model, id_column, active_id, &first_iter, iter);
}

static void
profile_encoding_combo_changed_cb (GtkComboBox *combo,
                                   GSettings *profile)
{
  GtkTreeIter iter;

  if (!gtk_combo_box_get_active_iter(combo, &iter))
    return;

  gs_free char *encoding = nullptr;
  gtk_tree_model_get(gtk_combo_box_get_model(combo),
                     &iter,
                     ENCODINGS_COL_ID, &encoding,
                     -1);
  if (encoding == nullptr)
    return;

  g_signal_handlers_block_by_func (profile, (void*)profile_notify_encoding_combo_cb, combo);
  g_settings_set_string(profile, TERMINAL_PROFILE_ENCODING_KEY, encoding);
  g_signal_handlers_unblock_by_func (profile, (void*)profile_notify_encoding_combo_cb, combo);
}

static void
profile_notify_encoding_combo_cb (GSettings *profile,
                                  const char *key,
                                  GtkComboBox *combo)
{
  gs_free char *encoding = nullptr;
  g_settings_get(profile, key, "s", &encoding);

  g_signal_handlers_block_by_func (combo, (void*)profile_encoding_combo_changed_cb, profile);

  GtkTreeIter iter;
  if (tree_model_id_to_iter(gtk_combo_box_get_model(combo),
                            ENCODINGS_COL_ID,
                            encoding,
                            &iter)) {
    gtk_combo_box_set_active_iter(combo, &iter);
  } else {
    gtk_combo_box_set_active(combo, -1);
  }

  g_signal_handlers_unblock_by_func (combo, (void*)profile_encoding_combo_changed_cb, profile);
}

/*
 * initialize widgets
 */

static void
init_color_scheme_menu (GtkWidget *widget)
{
  GtkCellRenderer *renderer;
  GtkTreeIter iter;
  gs_unref_object GtkListStore *store;
  guint i;

  store = gtk_list_store_new (1, G_TYPE_STRING);
  for (i = 0; i < G_N_ELEMENTS (color_schemes); ++i)
    gtk_list_store_insert_with_values (store, &iter, -1,
                                       0, _(color_schemes[i].name),
                                       -1);
  gtk_list_store_insert_with_values (store, &iter, -1,
                                      0, _("Custom"),
                                      -1);

  gtk_combo_box_set_model (GTK_COMBO_BOX (widget), GTK_TREE_MODEL (store));

  renderer = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (widget), renderer, TRUE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (widget), renderer, "text", 0, nullptr);
}

typedef enum {
        GROUP_UTF8,
        GROUP_CJKV,
        GROUP_OBSOLETE,
        LAST_GROUP
} EncodingGroup;

typedef struct {
  const char *charset;
  const char *name;
  EncodingGroup group;
} EncodingEntry;

/* These MUST be sorted by charset so that bsearch can work! */
static const EncodingEntry encodings[] = {
  { "BIG5",           N_("Chinese Traditional"), GROUP_CJKV },
  { "BIG5-HKSCS",     N_("Chinese Traditional"), GROUP_CJKV },
  { "CP866",          N_("Cyrillic/Russian"),    GROUP_OBSOLETE },
  { "EUC-JP",         N_("Japanese"),            GROUP_CJKV },
  { "EUC-KR",         N_("Korean"),              GROUP_CJKV },
  { "EUC-TW",         N_("Chinese Traditional"), GROUP_CJKV },
  { "GB18030",        N_("Chinese Simplified"),  GROUP_CJKV },
  { "GB2312",         N_("Chinese Simplified"),  GROUP_CJKV },
  { "GBK",            N_("Chinese Simplified"),  GROUP_CJKV },
  { "IBM850",         N_("Western"),             GROUP_OBSOLETE },
  { "IBM852",         N_("Central European"),    GROUP_OBSOLETE },
  { "IBM855",         N_("Cyrillic"),            GROUP_OBSOLETE },
  { "IBM857",         N_("Turkish"),             GROUP_OBSOLETE },
  { "IBM862",         N_("Hebrew"),              GROUP_OBSOLETE },
  { "IBM864",         N_("Arabic"),              GROUP_OBSOLETE },
  { "ISO-8859-1",     N_("Western"),             GROUP_OBSOLETE },
  { "ISO-8859-10",    N_("Nordic"),              GROUP_OBSOLETE },
  { "ISO-8859-13",    N_("Baltic"),              GROUP_OBSOLETE },
  { "ISO-8859-14",    N_("Celtic"),              GROUP_OBSOLETE },
  { "ISO-8859-15",    N_("Western"),             GROUP_OBSOLETE },
  { "ISO-8859-16",    N_("Romanian"),            GROUP_OBSOLETE },
  { "ISO-8859-2",     N_("Central European"),    GROUP_OBSOLETE },
  { "ISO-8859-3",     N_("South European"),      GROUP_OBSOLETE },
  { "ISO-8859-4",     N_("Baltic"),              GROUP_OBSOLETE },
  { "ISO-8859-5",     N_("Cyrillic"),            GROUP_OBSOLETE },
  { "ISO-8859-6",     N_("Arabic"),              GROUP_OBSOLETE },
  { "ISO-8859-7",     N_("Greek"),               GROUP_OBSOLETE },
  { "ISO-8859-8",     N_("Hebrew Visual"),       GROUP_OBSOLETE },
  { "ISO-8859-8-I",   N_("Hebrew"),              GROUP_OBSOLETE },
  { "ISO-8859-9",     N_("Turkish"),             GROUP_OBSOLETE },
  { "KOI8-R",         N_("Cyrillic"),            GROUP_OBSOLETE },
  { "KOI8-U",         N_("Cyrillic/Ukrainian"),  GROUP_OBSOLETE },
  { "MAC-CYRILLIC",   N_("Cyrillic"),            GROUP_OBSOLETE },
  { "MAC_ARABIC",     N_("Arabic"),              GROUP_OBSOLETE },
  { "MAC_CE",         N_("Central European"),    GROUP_OBSOLETE },
  { "MAC_CROATIAN",   N_("Croatian"),            GROUP_OBSOLETE },
  { "MAC_GREEK",      N_("Greek"),               GROUP_OBSOLETE },
  { "MAC_HEBREW",     N_("Hebrew"),              GROUP_OBSOLETE },
  { "MAC_ROMAN",      N_("Western"),             GROUP_OBSOLETE },
  { "MAC_ROMANIAN",   N_("Romanian"),            GROUP_OBSOLETE },
  { "MAC_TURKISH",    N_("Turkish"),             GROUP_OBSOLETE },
  { "MAC_UKRAINIAN",  N_("Cyrillic/Ukrainian"),  GROUP_OBSOLETE },
  { "SHIFT_JIS",      N_("Japanese"),            GROUP_CJKV },
  { "TIS-620",        N_("Thai"),                GROUP_OBSOLETE },
  { "UHC",            N_("Korean"),              GROUP_CJKV },
  { "UTF-8",          N_("Unicode"),             GROUP_UTF8 },
  { "WINDOWS-1250",   N_("Central European"),    GROUP_OBSOLETE },
  { "WINDOWS-1251",   N_("Cyrillic"),            GROUP_OBSOLETE },
  { "WINDOWS-1252",   N_("Western"),             GROUP_OBSOLETE },
  { "WINDOWS-1253",   N_("Greek"),               GROUP_OBSOLETE },
  { "WINDOWS-1254",   N_("Turkish"),             GROUP_OBSOLETE },
  { "WINDOWS-1255",   N_("Hebrew"),              GROUP_OBSOLETE},
  { "WINDOWS-1256",   N_("Arabic"),              GROUP_OBSOLETE },
  { "WINDOWS-1257",   N_("Baltic"),              GROUP_OBSOLETE },
  { "WINDOWS-1258",   N_("Vietnamese"),          GROUP_OBSOLETE },
};

static const struct {
  EncodingGroup group;
  const char *name;
} encodings_group_names[] = {
  { GROUP_UTF8,     N_("Unicode") },
  { GROUP_CJKV,     N_("Legacy CJK Encodings") },
  { GROUP_OBSOLETE, N_("Obsolete Encodings") },
};

#define EM_DASH "—"

static void
append_encodings_for_group (GtkTreeStore *store,
                            EncodingGroup group,
                            gboolean submenu)
{
  GtkTreeIter parent_iter;

  if (submenu) {
    gtk_tree_store_insert_with_values (store,
                                       &parent_iter,
                                       nullptr,
                                       -1,
                                       ENCODINGS_COL_ID, nullptr,
                                       ENCODINGS_COL_TEXT, _(encodings_group_names[group].name),
                                       -1);
  }

  for (guint i = 0; i < G_N_ELEMENTS (encodings); i++) {
    if (encodings[i].group != group)
      continue;

    /* Skip encodings not supported by ICU */
    if (terminal_util_translate_encoding (encodings[i].charset) == nullptr)
      continue;

    gs_free char *name = g_strdup_printf ("%s " EM_DASH " %s",
                                          _(encodings[i].name), encodings[i].charset);

    GtkTreeIter iter;
    gtk_tree_store_insert_with_values (store,
                                       &iter,
                                       submenu ? &parent_iter : nullptr,
                                       -1,
                                       ENCODINGS_COL_ID, encodings[i].charset,
                                       ENCODINGS_COL_TEXT, name,
                                       -1);
  }
}

static GtkTreeStore *
encodings_tree_store_new (void)
{
  GtkTreeStore *store = gtk_tree_store_new (2, G_TYPE_STRING, G_TYPE_STRING);

  append_encodings_for_group(store, GROUP_UTF8, FALSE); /* UTF-8 in main menu */
  append_encodings_for_group(store, GROUP_CJKV, TRUE);
  append_encodings_for_group(store, GROUP_OBSOLETE, TRUE);

  return store;
}

static void
init_encodings_combo (GtkWidget *widget)
{
  gs_unref_object GtkTreeStore *store = encodings_tree_store_new ();
  gtk_combo_box_set_model (GTK_COMBO_BOX (widget), GTK_TREE_MODEL (store));
}

static gboolean
s_to_rgba (GValue *value,
           GVariant *variant,
           gpointer user_data)
{
  const char *s;
  GdkRGBA color;

  g_variant_get (variant, "&s", &s);
  if (!gdk_rgba_parse (&color, s))
    return FALSE;

  color.alpha = 1.0;
  g_value_set_boxed (value, &color);
  return TRUE;
}

static GVariant *
rgba_to_s (const GValue *value,
           const GVariantType *expected_type,
           gpointer user_data)
{
  GdkRGBA *color;
  gs_free char *s = nullptr;

  color = reinterpret_cast<GdkRGBA*>(g_value_get_boxed (value));
  if (color == nullptr)
    return nullptr;

  s = gdk_rgba_to_string (color);
  return g_variant_new_string (s);
}

static gboolean
string_to_enum (GValue *value,
                GVariant *variant,
                gpointer user_data)
{
  GType (* get_type) (void) = (GType (*)(void))user_data;
  GEnumClass *klass;
  GEnumValue *eval = nullptr;
  const char *s;
  guint i;

  g_variant_get (variant, "&s", &s);

  klass = reinterpret_cast<GEnumClass*>(g_type_class_ref (get_type ()));
  for (i = 0; i < klass->n_values; ++i) {
    if (strcmp (klass->values[i].value_nick, s) != 0)
      continue;

    eval = &klass->values[i];
    break;
  }

  if (eval)
    g_value_set_int (value, eval->value);

  g_type_class_unref (klass);

  return eval != nullptr;
}

static GVariant *
enum_to_string (const GValue *value,
                const GVariantType *expected_type,
                gpointer user_data)
{
  GType (* get_type) (void) = (GType (*)(void))user_data;
  GEnumClass *klass;
  GEnumValue *eval = nullptr;
  int val;
  guint i;
  GVariant *variant = nullptr;

  val = g_value_get_int (value);

  klass = reinterpret_cast<GEnumClass*>(g_type_class_ref (get_type ()));
  for (i = 0; i < klass->n_values; ++i) {
    if (klass->values[i].value != val)
      continue;

    eval = &klass->values[i];
    break;
  }

  if (eval)
    variant = g_variant_new_string (eval->value_nick);

  g_type_class_unref (klass);

  return variant;
}

static gboolean
scrollbar_policy_to_bool (GValue *value,
                          GVariant *variant,
                          gpointer user_data)
{
  const char *str;

  g_variant_get (variant, "&s", &str);
  g_value_set_boolean (value, g_str_equal (str, "always"));

  return TRUE;
}

static GVariant *
bool_to_scrollbar_policy (const GValue *value,
                          const GVariantType *expected_type,
                          gpointer user_data)
{
  return g_variant_new_string (g_value_get_boolean (value) ? "always" : "never");
}

static gboolean
monospace_filter (const PangoFontFamily *family,
                  const PangoFontFace   *face,
                  gpointer data)
{
  return pango_font_family_is_monospace ((PangoFontFamily *) family);
}

/* Called once per Preferences window, to initialize stuff that doesn't depend on the profile being edited */
void
profile_prefs_init (void)
{
  GtkWidget *w;
  GtkBuilder *builder = the_pref_data->builder;
  char *text;

  the_pref_data->profile_signals = g_array_new (FALSE, FALSE, sizeof (ProfilePrefsSignal));
  the_pref_data->profile_bindings = g_array_new (FALSE, FALSE, sizeof (ProfilePrefsBinding));

  w = (GtkWidget *) gtk_builder_get_object (builder, "color-scheme-combobox");
  init_color_scheme_menu (w);

  w = (GtkWidget *) gtk_builder_get_object (builder, "encoding-combobox");
  init_encodings_combo (w);

  /* Translators: Appears as: [numeric entry] × width */
  text = g_strdup_printf ("× %s", _("width"));
  gtk_label_set_text ((GtkLabel *) gtk_builder_get_object (builder, "cell-width-scale-label"),
                      text);
  g_free (text);
  /* Translators: Appears as: [numeric entry] × height */
  text = g_strdup_printf ("× %s", _("height"));
  gtk_label_set_text ((GtkLabel *) gtk_builder_get_object (builder, "cell-height-scale-label"),
                      text);
  g_free (text);
}

/* ============================================================================
 * Startup tab — per-profile default-launch-entries editor
 *
 * The Startup tab edits a profile's `default-launch-entries` setting: an array
 * of `(id, parent_id, directory, command)` tuples that axan expands into one
 * shell per entry when launched with this profile active (see
 * terminal_options_expand_profile_launch_entries in terminal-options.cc).
 *
 * Identity model: every entry carries a stable UUID `id`. Children reference
 * their parent by UUID, NOT by row index — this means reorder, rename, and
 * edit operations cannot accidentally re-target parentage. An empty `parent_id`
 * marks a root entry. Forward-reference only: `parent_id` MUST reference an
 * entry that appears earlier in the array, so hierarchy can be applied
 * incrementally as shells are spawned (a child can't depend on a parent
 * that hasn't been created yet).
 *
 * The editor builds on a GtkListBox in preferences.ui. Each entry is a
 * GtkListBoxRow whose left column carries the row's id and parent_id stashed
 * via g_object_set_data_full, two always-editable GtkEntry widgets, indent
 * (▶) / outdent (◀) buttons that modify parent_id, and a remove button. An
 * Add button below the list creates a new empty root row.
 *
 * Save model: every change (Add, Remove, GtkEntry "changed", indent/outdent
 * click) rebuilds the full a(ssss) GVariant from the current listbox rows
 * and writes it back via g_settings_set_value. We do NOT listen for external
 * changed signals here; the listbox is treated as the source of truth while
 * the dialog is open. Live re-population on external `gsettings set` is a v0
 * limitation, acceptable because the common case is one editor at a time.
 * ============================================================================
 */

static void startup_save_entries (GtkBuilder *builder, GSettings *profile);
static void startup_refresh_layout (GtkBuilder *builder);
static void startup_update_count (GtkBuilder *builder, guint count);
static void startup_update_empty_state (GtkBuilder *builder, guint count);

/* Marker key set on each GtkListBoxRow we create — used to find our rows in
 * the listbox at save time, and to skip any future foreign children. */
#define STARTUP_ROW_TAG "axan-startup-row"

/* Per-step horizontal indent applied to each level of nesting. Mirrors the
 * runtime sidebar's INDENT_STEP_PX so the editor visually previews the tree
 * shape the user will see at launch. */
#define STARTUP_INDENT_STEP_PX 16

/* Read the three GtkEntry widgets out of a row built by startup_create_row. */
static void
startup_row_get_fields (GtkListBoxRow *row, GtkEntry **name_entry,
                        GtkEntry **dir_entry, GtkEntry **cmd_entry)
{
  if (name_entry) *name_entry = GTK_ENTRY (g_object_get_data (G_OBJECT (row), "name-entry"));
  if (dir_entry) *dir_entry = GTK_ENTRY (g_object_get_data (G_OBJECT (row), "dir-entry"));
  if (cmd_entry) *cmd_entry = GTK_ENTRY (g_object_get_data (G_OBJECT (row), "cmd-entry"));
}

/* Recover the row's stable UUID and its parent UUID. Both are owned by the
 * row via g_object_set_data_full(g_free), so the returned pointers are valid
 * only as long as the row lives — callers must g_strdup if they need to
 * outlive the row (rare; the only such consumer is startup_save_entries
 * which marshals immediately into a GVariant builder). */
static const char *
startup_row_get_id (GtkListBoxRow *row)
{
  const char *id = (const char *) g_object_get_data (G_OBJECT (row), "axan-entry-id");
  return id ? id : "";
}

static const char *
startup_row_get_parent_id (GtkListBoxRow *row)
{
  const char *p = (const char *) g_object_get_data (G_OBJECT (row), "axan-entry-parent-id");
  return p ? p : "";
}

static void
startup_row_set_parent_id (GtkListBoxRow *row, const char *parent_id)
{
  g_object_set_data_full (G_OBJECT (row), "axan-entry-parent-id",
                          g_strdup (parent_id ? parent_id : ""), g_free);
}

/* Icon assignment for the entry — see schema description on
 * default-launch-entries for accepted forms (empty, `builtin:NAME`,
 * absolute path, bare filename resolved against ~/.config/axan/icons/).
 * Read by startup_save_entries when marshaling rows back to gsettings;
 * written by the icon-picker combobox change handler. */
static const char *
startup_row_get_icon (GtkListBoxRow *row)
{
  const char *icon = (const char *) g_object_get_data (G_OBJECT (row), "axan-entry-icon");
  return icon ? icon : "";
}

static void
startup_row_set_icon (GtkListBoxRow *row, const char *icon)
{
  g_object_set_data_full (G_OBJECT (row), "axan-entry-icon",
                          g_strdup (icon ? icon : ""), g_free);
}

/* Build a hash table id → GtkListBoxRow* for the current listbox state.
 * Used to walk the parent chain when computing depth or evaluating button
 * sensitivity. Caller owns the table; values are unowned (the listbox owns
 * the rows). */
static GHashTable *
startup_build_id_map (GtkListBox *listbox)
{
  GHashTable *map = g_hash_table_new (g_str_hash, g_str_equal);
  GList *children = gtk_container_get_children (GTK_CONTAINER (listbox));
  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW (l->data);
    if (g_object_get_data (G_OBJECT (row), STARTUP_ROW_TAG) == nullptr)
      continue;
    const char *id = startup_row_get_id (row);
    if (*id != '\0')
      g_hash_table_insert (map, (gpointer) id, row);
  }
  g_list_free (children);
  return map;
}

/* Depth of a row in the tree, derived by walking parent_id pointers. Returns
 * 0 for a root. A capped iteration guard prevents an infinite loop in the
 * pathological case of malformed data on disk (which shouldn't happen given
 * the forward-reference invariant + save-time validation, but defensive). */
static int
startup_row_depth (GtkListBoxRow *row, GHashTable *id_map)
{
  int depth = 0;
  const char *pid = startup_row_get_parent_id (row);
  while (*pid != '\0' && depth < 128) {
    GtkListBoxRow *parent = GTK_LIST_BOX_ROW (g_hash_table_lookup (id_map, pid));
    if (parent == nullptr)
      break;
    depth++;
    pid = startup_row_get_parent_id (parent);
  }
  return depth;
}

/* GtkEntry "changed" signal handler — pushes the whole list back to GSettings.
 * The full-rebuild approach is O(n) per keystroke but n is tiny (entries are
 * counted in handfuls) and GSettings batches IO. Simpler than per-row diffing. */
static void
startup_entry_changed_cb (GtkEntry *entry G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  startup_save_entries (the_pref_data->builder, the_pref_data->selected_profile);
}

/* Remove button click — destroy this row, then save. Any children of the
 * removed row become orphaned (their parent_id no longer resolves); save_entries
 * will drop them as a defensive measure. This matches the sidebar's runtime
 * behavior of hoisting children, but in the editor we don't reparent because
 * the user explicitly removed the entry — orphan-drop is the closest analog
 * to "the subtree goes with it." If the user wanted to keep the children
 * they can outdent them to root first. */
static void
startup_remove_clicked_cb (GtkButton *button, gpointer user_data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW));
  if (row == nullptr)
    return;
  GtkWidget *listbox = gtk_widget_get_parent (GTK_WIDGET (row));
  if (listbox == nullptr)
    return;
  gtk_container_remove (GTK_CONTAINER (listbox), GTK_WIDGET (row));

  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  startup_save_entries (the_pref_data->builder, the_pref_data->selected_profile);
}

/* Rebuild the listbox in valid pre-order based on each row's parent_id
 * pointer, preserving the current sibling order under each parent.
 *
 * Why this exists: move_row's subtree_end uses depth as the subtree-extent
 * proxy. That proxy is correct only if the listbox is already in pre-order
 * (a node's subtree is a contiguous depth-greater range immediately after
 * the node). Indent/outdent change parent_id WITHOUT moving the row, so they
 * can leave a row's listbox position inconsistent with its tree position —
 * e.g., a child stranded outside its parent's subtree, or a depth-1 row
 * between two depth-0 roots that are NOT its parents. After such a state,
 * a subsequent move_row will mis-identify subtree extents and damage the
 * tree.
 *
 * Calling this after every parent_id mutation (indent, outdent, and on
 * initial load — defensive against externally-edited gsettings that
 * already violated the invariant) keeps pre-order true at all times so
 * move_row can rely on it.
 *
 * Algorithm:
 *   1. Collect rows in current listbox order; build (parent_id -> ordered
 *      children) map and (id -> row) set.
 *   2. Treat any parent_id that doesn't resolve in the id set as root —
 *      defensive against dangling references.
 *   3. Pre-order DFS from the "" (root) bucket, recording the desired
 *      sequence.
 *   4. Compare desired vs current; if identical, no-op (avoids gratuitous
 *      reflow flicker). Otherwise remove all rows, re-insert in desired
 *      order. Refs are held across the remove so widgets survive.
 *
 * Cost: O(n) walk + O(n) compare + O(n) remove-insert. n is tens at most. */
static gboolean
startup_reorder_to_pre_order (GtkListBox *listbox)
{
  if (listbox == nullptr)
    return FALSE;

  GList *children = gtk_container_get_children (GTK_CONTAINER (listbox));
  if (children == nullptr)
    return FALSE;

  /* Build known_ids and kids_by_parent (children stored as GPtrArray). Use
   * the row's stashed pid as the key — same string lifetime as the row, so
   * no g_strdup needed for the lookup key. */
  GHashTable *known_ids = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTable *kids_by_parent = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      nullptr,
                                                      (GDestroyNotify) g_ptr_array_unref);

  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (l->data);
    if (g_object_get_data (G_OBJECT (r), STARTUP_ROW_TAG) == nullptr)
      continue;
    const char *id = startup_row_get_id (r);
    if (*id != '\0')
      g_hash_table_add (known_ids, (gpointer) id);
  }

  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (l->data);
    if (g_object_get_data (G_OBJECT (r), STARTUP_ROW_TAG) == nullptr)
      continue;
    const char *pid = startup_row_get_parent_id (r);
    /* Orphans treated as roots. */
    if (*pid != '\0' && !g_hash_table_contains (known_ids, pid))
      pid = "";
    GPtrArray *kids = (GPtrArray *) g_hash_table_lookup (kids_by_parent, pid);
    if (kids == nullptr) {
      kids = g_ptr_array_new ();
      g_hash_table_insert (kids_by_parent, (gpointer) pid, kids);
    }
    g_ptr_array_add (kids, r);
  }

  /* Iterative DFS via a stack of "next sibling under this parent" cursors.
   * We push children of each visited node in reverse so the leftmost child
   * pops first. Output is the pre-order traversal. */
  GPtrArray *desired = g_ptr_array_new ();
  GQueue *stack = g_queue_new ();
  GPtrArray *roots = (GPtrArray *) g_hash_table_lookup (kids_by_parent, "");
  if (roots != nullptr) {
    for (int i = (int) roots->len - 1; i >= 0; i--)
      g_queue_push_head (stack, g_ptr_array_index (roots, i));
  }
  while (!g_queue_is_empty (stack)) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (g_queue_pop_head (stack));
    g_ptr_array_add (desired, r);
    const char *id = startup_row_get_id (r);
    GPtrArray *kids = (GPtrArray *) g_hash_table_lookup (kids_by_parent, id);
    if (kids != nullptr) {
      for (int i = (int) kids->len - 1; i >= 0; i--)
        g_queue_push_head (stack, g_ptr_array_index (kids, i));
    }
  }
  g_queue_free (stack);

  /* Compare desired vs current; if identical, nothing to do. */
  gboolean differs = FALSE;
  {
    guint k = 0;
    for (GList *l = children; l != nullptr; l = l->next) {
      GtkListBoxRow *r = GTK_LIST_BOX_ROW (l->data);
      if (g_object_get_data (G_OBJECT (r), STARTUP_ROW_TAG) == nullptr)
        continue;
      if (k >= desired->len || g_ptr_array_index (desired, k) != r) {
        differs = TRUE;
        break;
      }
      k++;
    }
    if (!differs && k != desired->len)
      differs = TRUE;
  }

  if (differs) {
    /* Ref all desired rows so they survive the remove. */
    for (guint i = 0; i < desired->len; i++)
      g_object_ref (G_OBJECT (g_ptr_array_index (desired, i)));
    /* Remove only our tagged rows; leave foreign children (none today) alone. */
    for (GList *l = children; l != nullptr; l = l->next) {
      GtkListBoxRow *r = GTK_LIST_BOX_ROW (l->data);
      if (g_object_get_data (G_OBJECT (r), STARTUP_ROW_TAG) == nullptr)
        continue;
      gtk_container_remove (GTK_CONTAINER (listbox), GTK_WIDGET (r));
    }
    /* Re-insert in desired order. */
    for (guint i = 0; i < desired->len; i++) {
      GtkListBoxRow *r = GTK_LIST_BOX_ROW (g_ptr_array_index (desired, i));
      gtk_list_box_insert (listbox, GTK_WIDGET (r), -1);
      g_object_unref (G_OBJECT (r));
    }
  }

  g_list_free (children);
  g_ptr_array_free (desired, TRUE);
  g_hash_table_destroy (kids_by_parent);
  g_hash_table_destroy (known_ids);
  return differs;
}

/* Subtree-aware sibling lookup: walk the listbox in the given direction and
 * return the index of the nearest row that shares this row's parent_id
 * (i.e., is a sibling in the tree). Returns -1 if none exists in that
 * direction. Used by the up/down move handlers — moving among siblings
 * preserves hierarchy by construction, so we never need a cycle/forward-ref
 * check at this layer. */
static int
startup_find_sibling_index (GtkListBox *listbox, int from_idx, int direction,
                            const char *parent_id)
{
  int i = from_idx;
  for (;;) {
    i += direction;
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (listbox, i);
    if (r == nullptr)
      return -1;
    if (g_object_get_data (G_OBJECT (r), STARTUP_ROW_TAG) == nullptr)
      continue;
    if (g_strcmp0 (startup_row_get_parent_id (r), parent_id) == 0)
      return i;
  }
}

/* End index (exclusive) of the subtree rooted at start_idx, computed by
 * walking forward over the contiguous range of rows whose depth exceeds
 * the source's depth. Mirrors terminal-sidebar.cc's subtree_end_index. */
static int
startup_subtree_end (GtkListBox *listbox, int start_idx, GHashTable *id_map)
{
  GtkListBoxRow *source = gtk_list_box_get_row_at_index (listbox, start_idx);
  if (source == nullptr)
    return start_idx;
  int source_depth = startup_row_depth (source, id_map);
  int i = start_idx + 1;
  for (;;) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (listbox, i);
    if (r == nullptr || startup_row_depth (r, id_map) <= source_depth)
      return i;
    i++;
  }
}

/* Move the subtree rooted at this row up (direction = -1) or down (+1) by
 * one sibling slot under the same parent. The entire subtree travels as a
 * unit so children stay attached to the moved root. No-op if there's no
 * sibling in that direction — the button is desensitized in refresh_layout
 * for that case but we check defensively here too. */
static void
startup_move_row (GtkListBoxRow *row, int direction)
{
  GtkListBox *listbox = GTK_LIST_BOX (gtk_widget_get_parent (GTK_WIDGET (row)));
  if (listbox == nullptr)
    return;
  GHashTable *id_map = startup_build_id_map (listbox);

  int my_idx = gtk_list_box_row_get_index (row);
  int my_end = startup_subtree_end (listbox, my_idx, id_map);
  const char *my_parent = startup_row_get_parent_id (row);

  /* For "up", we swap our subtree with the previous sibling's subtree —
   * implemented as moving our subtree to start where the previous sibling
   * starts. For "down", we move the next sibling's subtree to start where
   * ours starts. Symmetric, and each branch only moves one subtree. */
  GPtrArray *moving = nullptr;
  int remove_start = 0, remove_end = 0, insert_at = 0;

  if (direction < 0) {
    int prev_idx = startup_find_sibling_index (listbox, my_idx, -1, my_parent);
    if (prev_idx < 0) {
      g_hash_table_destroy (id_map);
      return;
    }
    remove_start = my_idx;
    remove_end = my_end;
    insert_at = prev_idx;
  } else {
    int next_idx = startup_find_sibling_index (listbox, my_end - 1, 1, my_parent);
    if (next_idx < 0) {
      g_hash_table_destroy (id_map);
      return;
    }
    int next_end = startup_subtree_end (listbox, next_idx, id_map);
    remove_start = next_idx;
    remove_end = next_end;
    insert_at = my_idx;
  }
  g_hash_table_destroy (id_map);

  /* Collect (with refs), remove back-to-front, re-insert. Identical pattern
   * to the sidebar's subtree-move; refs hold rows alive across the remove. */
  int subtree_size = remove_end - remove_start;
  moving = g_ptr_array_new_full (subtree_size, nullptr);
  for (int i = remove_start; i < remove_end; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (listbox, i);
    g_object_ref (r);
    g_ptr_array_add (moving, r);
  }
  for (int i = remove_end - 1; i >= remove_start; i--) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (listbox, i);
    gtk_container_remove (GTK_CONTAINER (listbox), GTK_WIDGET (r));
  }
  for (guint i = 0; i < moving->len; i++) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (g_ptr_array_index (moving, i));
    gtk_list_box_insert (listbox, GTK_WIDGET (r), insert_at + (int) i);
    g_object_unref (r);
  }
  g_ptr_array_free (moving, TRUE);

  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  startup_save_entries (the_pref_data->builder, the_pref_data->selected_profile);
}

static void
startup_move_up_clicked_cb (GtkButton *button, gpointer user_data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW));
  if (row != nullptr)
    startup_move_row (row, -1);
}

static void
startup_move_down_clicked_cb (GtkButton *button, gpointer user_data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW));
  if (row != nullptr)
    startup_move_row (row, 1);
}

/* Indent click: nest this row under the immediately preceding row in the
 * listbox. The forward-reference invariant holds automatically because the
 * previous row, by definition, appears earlier in the array. No-op if this
 * is row 0 (the button is desensitized in that case but check defensively). */
static void
startup_indent_clicked_cb (GtkButton *button, gpointer user_data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW));
  if (row == nullptr)
    return;
  GtkListBox *listbox = GTK_LIST_BOX (gtk_widget_get_parent (GTK_WIDGET (row)));
  if (listbox == nullptr)
    return;
  int idx = gtk_list_box_row_get_index (row);
  if (idx <= 0)
    return;
  GtkListBoxRow *prev = gtk_list_box_get_row_at_index (listbox, idx - 1);
  if (prev == nullptr)
    return;
  startup_row_set_parent_id (row, startup_row_get_id (prev));
  /* Pre-order may now be violated (row should sit at the end of prev's
   * existing descendants, not immediately after prev when prev already had
   * children). Re-sort to restore the invariant. */
  startup_reorder_to_pre_order (listbox);

  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  startup_save_entries (the_pref_data->builder, the_pref_data->selected_profile);
}

/* Outdent click: pop this row out one level — its new parent_id becomes its
 * current parent's parent_id. If the current parent was a root (parent_id ""),
 * this row becomes a root too. No-op if already at root (button desensitized). */
static void
startup_outdent_clicked_cb (GtkButton *button, gpointer user_data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW));
  if (row == nullptr)
    return;
  GtkListBox *listbox = GTK_LIST_BOX (gtk_widget_get_parent (GTK_WIDGET (row)));
  if (listbox == nullptr)
    return;
  const char *pid = startup_row_get_parent_id (row);
  if (*pid == '\0')
    return;

  GHashTable *id_map = startup_build_id_map (listbox);
  GtkListBoxRow *parent_row = GTK_LIST_BOX_ROW (g_hash_table_lookup (id_map, pid));
  const char *grandparent_id = (parent_row != nullptr) ? startup_row_get_parent_id (parent_row) : "";
  startup_row_set_parent_id (row, grandparent_id);
  g_hash_table_destroy (id_map);
  /* Outdent leaves the row stranded inside the old parent's subtree. Re-sort
   * to put it as a sibling of the old parent (immediately after the old
   * parent's remaining descendants). */
  startup_reorder_to_pre_order (listbox);

  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  startup_save_entries (the_pref_data->builder, the_pref_data->selected_profile);
}

/* Combobox "changed" handler — push the active id (the stored icon value,
 * e.g. "" / "builtin:terminal" / "my-custom.svg") onto the row stash and
 * persist. Mirrors startup_entry_changed_cb but reads from a combobox
 * instead of a GtkEntry, and writes the icon stash rather than the entry
 * widgets that startup_save_entries reads directly. */
static void
startup_icon_changed_cb (GtkComboBox *combo, gpointer user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_widget_get_ancestor (GTK_WIDGET (combo),
                                                                   GTK_TYPE_LIST_BOX_ROW));
  if (row == nullptr)
    return;
  const char *active_id = gtk_combo_box_get_active_id (combo);
  startup_row_set_icon (row, active_id ? active_id : "");
  startup_save_entries (the_pref_data->builder, the_pref_data->selected_profile);
}

/* Build one entry row. The two GtkEntry widgets and the indent/outdent buttons
 * are stashed on the row via g_object_set_data so subsequent layout passes
 * (depth recompute, button sensitivity) can find them without re-traversing
 * the row's widget tree. The row's stable UUID id and parent_id are also
 * stashed (owned: freed via g_object_set_data_full). Style classes are tagged
 * so the theme pass can give the row the "card" chrome from v3. */
/* Per-node color tokens offered in the Startup editor. The id is what gets
 * persisted (a palette name, or "" for no color); the resolver in
 * terminal-sidebar.cc maps the name to a theme-appropriate hex at render
 * time. A literal "#RRGGBB" is also accepted on a node but isn't offered here
 * (advanced users hand-edit the TOML). Keep in sync with node_edit_swatches_*
 * in terminal-sidebar.cc. */
static const struct { const char *id; const char *label; } kStartupColors[] = {
  { "",       N_("(none)") },
  { "red",    N_("Red")    },
  { "orange", N_("Orange") },
  { "yellow", N_("Yellow") },
  { "green",  N_("Green")  },
  { "blue",   N_("Blue")   },
  { "purple", N_("Purple") },
};
static const struct { const char *id; const char *label; } kStartupColorTargets[] = {
  { "both", N_("Both") },
  { "icon", N_("Icon") },
  { "text", N_("Text") },
};

static GtkWidget *
startup_create_row (const char *id, const char *parent_id, const char *name,
                    const char *dir, const char *cmd, const char *icon,
                    const char *color, const char *color_target)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  g_object_set_data (G_OBJECT (row), STARTUP_ROW_TAG, GINT_TO_POINTER (1));
  gtk_style_context_add_class (gtk_widget_get_style_context (row), "axan-startup-card");

  /* Stash id + parent_id + icon on the row. g_strdup'd because the source
   * strings (often from a g_variant_iter_loop &s borrow) can be
   * invalidated when the iter advances; we need our own lifetime. Freed
   * by g_object_set_data_full when the row is destroyed. */
  g_object_set_data_full (G_OBJECT (row), "axan-entry-id",
                          g_strdup (id ? id : ""), g_free);
  g_object_set_data_full (G_OBJECT (row), "axan-entry-parent-id",
                          g_strdup (parent_id ? parent_id : ""), g_free);
  g_object_set_data_full (G_OBJECT (row), "axan-entry-icon",
                          g_strdup (icon ? icon : ""), g_free);

  /* row_box is the indented content container — its margin-start gets bumped
   * per depth level so nested rows visually offset to the right. Keep the
   * row itself flush so the card chrome (background, borders) spans the
   * full row width; only the inner content shifts. */
  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_end (row_box, 4);
  gtk_widget_set_margin_top (row_box, 4);
  gtk_widget_set_margin_bottom (row_box, 4);
  gtk_container_add (GTK_CONTAINER (row), row_box);
  g_object_set_data (G_OBJECT (row), "axan-row-box", row_box);

  /* Up / Down buttons replace the v3 wireframe's "⋮⋮" drag grip. Drag-to-
   * reorder was considered but two explicit arrows are simpler, accessible,
   * and unambiguous about the unit of motion (subtree, not row). Up moves
   * this row's subtree to the position of the previous sibling; Down moves
   * the next sibling above us. Sensitivity is recomputed in refresh_layout
   * based on whether a sibling exists in that direction. */
  GtkWidget *up_btn = gtk_button_new_from_icon_name ("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_valign (up_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (up_btn, _("Move this shell up"));
  gtk_style_context_add_class (gtk_widget_get_style_context (up_btn), "flat");
  gtk_style_context_add_class (gtk_widget_get_style_context (up_btn), "axan-startup-move");
  gtk_box_pack_start (GTK_BOX (row_box), up_btn, FALSE, FALSE, 0);

  GtkWidget *down_btn = gtk_button_new_from_icon_name ("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_valign (down_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (down_btn, _("Move this shell down"));
  gtk_style_context_add_class (gtk_widget_get_style_context (down_btn), "flat");
  gtk_style_context_add_class (gtk_widget_get_style_context (down_btn), "axan-startup-move");
  gtk_box_pack_start (GTK_BOX (row_box), down_btn, FALSE, FALSE, 0);

  /* Outdent (◀) and indent (▶) buttons. Sensitivity is recomputed on every
   * layout refresh (see startup_refresh_layout) based on row index and
   * current parent_id. We use go-previous/go-next symbolic icons so they
   * render with the user's icon theme rather than as raw text. */
  GtkWidget *outdent_btn = gtk_button_new_from_icon_name ("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_valign (outdent_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (outdent_btn, _("Outdent — move this shell out of its group"));
  gtk_style_context_add_class (gtk_widget_get_style_context (outdent_btn), "flat");
  gtk_style_context_add_class (gtk_widget_get_style_context (outdent_btn), "axan-startup-indent");
  gtk_box_pack_start (GTK_BOX (row_box), outdent_btn, FALSE, FALSE, 0);

  GtkWidget *indent_btn = gtk_button_new_from_icon_name ("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_valign (indent_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (indent_btn, _("Indent — nest this shell under the one above"));
  gtk_style_context_add_class (gtk_widget_get_style_context (indent_btn), "flat");
  gtk_style_context_add_class (gtk_widget_get_style_context (indent_btn), "axan-startup-indent");
  gtk_box_pack_start (GTK_BOX (row_box), indent_btn, FALSE, FALSE, 0);

  /* Name field: optional display-name template for the sidebar row. Empty
   * means "use the OSC-title fallback chain"; non-empty overrides the title
   * with the user-supplied string after expanding $pwd/$user/etc. — see
   * the variable expander in terminal-sidebar.cc. Placeholder advertises a
   * couple of the more useful variables so the feature is discoverable. */
  GtkWidget *name_field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (name_field, TRUE);
  GtkWidget *name_label = gtk_label_new (_("NAME"));
  gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (name_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (name_field), name_label, FALSE, FALSE, 0);
  GtkWidget *name_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (name_entry), name ? name : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (name_entry), _("optional"));
  gtk_box_pack_start (GTK_BOX (name_field), name_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row_box), name_field, TRUE, TRUE, 0);

  /* Directory field: vertical box with uppercase mini-label + GtkEntry. */
  GtkWidget *dir_field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (dir_field, TRUE);
  GtkWidget *dir_label = gtk_label_new (_("DIRECTORY"));
  gtk_label_set_xalign (GTK_LABEL (dir_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (dir_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (dir_field), dir_label, FALSE, FALSE, 0);
  GtkWidget *dir_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (dir_entry), dir ? dir : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (dir_entry), "~");
  gtk_box_pack_start (GTK_BOX (dir_field), dir_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row_box), dir_field, TRUE, TRUE, 0);

  /* Command field: same shape. Placeholder hints "your shell" so an empty
   * command reads as intentional rather than blank. */
  GtkWidget *cmd_field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (cmd_field, TRUE);
  GtkWidget *cmd_label = gtk_label_new (_("COMMAND"));
  gtk_label_set_xalign (GTK_LABEL (cmd_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (cmd_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (cmd_field), cmd_label, FALSE, FALSE, 0);
  GtkWidget *cmd_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (cmd_entry), cmd ? cmd : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (cmd_entry), _("your shell"));
  gtk_box_pack_start (GTK_BOX (cmd_field), cmd_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row_box), cmd_field, TRUE, TRUE, 0);

  /* Icon picker: compact non-expanding field. The combobox itself is built
   * by terminal_sidebar_icon_combo_new — shared with future call sites
   * (sidebar context menu, etc.). hexpand=FALSE so the picker takes its
   * natural width and leaves the three text fields above to share the
   * rest of the row. */
  GtkWidget *icon_field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *icon_label = gtk_label_new (_("ICON"));
  gtk_label_set_xalign (GTK_LABEL (icon_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (icon_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (icon_field), icon_label, FALSE, FALSE, 0);
  GtkWidget *icon_combo = terminal_sidebar_icon_combo_new (icon);
  gtk_box_pack_start (GTK_BOX (icon_field), icon_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row_box), icon_field, FALSE, FALSE, 0);
  g_object_set_data (G_OBJECT (row), "icon-combo", icon_combo);

  /* Color picker: the per-node recolor token (a palette name; "" = none).
   * Plain text combo — the fancy swatch picker is the context-menu dialog;
   * here the names are enough and stay compact. Read back directly from the
   * combo in startup_save_entries, like the icon picker. */
  GtkWidget *color_field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *color_label = gtk_label_new (_("COLOR"));
  gtk_label_set_xalign (GTK_LABEL (color_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (color_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (color_field), color_label, FALSE, FALSE, 0);
  GtkWidget *color_combo = gtk_combo_box_text_new ();
  for (guint i = 0; i < G_N_ELEMENTS (kStartupColors); i++)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (color_combo),
                               kStartupColors[i].id, _(kStartupColors[i].label));
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (color_combo),
                               (color != nullptr && *color != '\0') ? color : "");
  gtk_box_pack_start (GTK_BOX (color_field), color_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row_box), color_field, FALSE, FALSE, 0);
  g_object_set_data (G_OBJECT (row), "color-combo", color_combo);
  g_signal_connect (color_combo, "changed", G_CALLBACK (startup_entry_changed_cb), nullptr);

  /* Apply-to picker: which surfaces the recolor paints. Defaults to "both". */
  GtkWidget *target_field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *target_label = gtk_label_new (_("APPLY"));
  gtk_label_set_xalign (GTK_LABEL (target_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (target_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (target_field), target_label, FALSE, FALSE, 0);
  GtkWidget *target_combo = gtk_combo_box_text_new ();
  for (guint i = 0; i < G_N_ELEMENTS (kStartupColorTargets); i++)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (target_combo),
                               kStartupColorTargets[i].id, _(kStartupColorTargets[i].label));
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (target_combo),
                               (color_target != nullptr && *color_target != '\0') ? color_target : "both");
  gtk_box_pack_start (GTK_BOX (target_field), target_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row_box), target_field, FALSE, FALSE, 0);
  g_object_set_data (G_OBJECT (row), "target-combo", target_combo);
  g_signal_connect (target_combo, "changed", G_CALLBACK (startup_entry_changed_cb), nullptr);

  /* Remove button — × matching the wireframe action column. */
  GtkWidget *remove_btn = gtk_button_new_from_icon_name ("edit-delete-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_valign (remove_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (remove_btn, _("Remove this startup shell"));
  gtk_style_context_add_class (gtk_widget_get_style_context (remove_btn), "flat");
  gtk_style_context_add_class (gtk_widget_get_style_context (remove_btn), "axan-startup-remove");
  gtk_box_pack_start (GTK_BOX (row_box), remove_btn, FALSE, FALSE, 0);

  g_object_set_data (G_OBJECT (row), "name-entry", name_entry);
  g_object_set_data (G_OBJECT (row), "dir-entry", dir_entry);
  g_object_set_data (G_OBJECT (row), "cmd-entry", cmd_entry);
  g_object_set_data (G_OBJECT (row), "indent-btn", indent_btn);
  g_object_set_data (G_OBJECT (row), "outdent-btn", outdent_btn);
  g_object_set_data (G_OBJECT (row), "up-btn", up_btn);
  g_object_set_data (G_OBJECT (row), "down-btn", down_btn);

  g_signal_connect (name_entry, "changed", G_CALLBACK (startup_entry_changed_cb), nullptr);
  g_signal_connect (dir_entry, "changed", G_CALLBACK (startup_entry_changed_cb), nullptr);
  g_signal_connect (cmd_entry, "changed", G_CALLBACK (startup_entry_changed_cb), nullptr);
  g_signal_connect (icon_combo, "changed", G_CALLBACK (startup_icon_changed_cb), nullptr);
  g_signal_connect (remove_btn, "clicked", G_CALLBACK (startup_remove_clicked_cb), nullptr);
  g_signal_connect (indent_btn, "clicked", G_CALLBACK (startup_indent_clicked_cb), nullptr);
  g_signal_connect (outdent_btn, "clicked", G_CALLBACK (startup_outdent_clicked_cb), nullptr);
  g_signal_connect (up_btn, "clicked", G_CALLBACK (startup_move_up_clicked_cb), nullptr);
  g_signal_connect (down_btn, "clicked", G_CALLBACK (startup_move_down_clicked_cb), nullptr);

  gtk_widget_show_all (row);
  return row;
}

/* Walk the listbox once and (a) update each row's indent margin to reflect
 * its current depth and (b) update its indent/outdent button sensitivity.
 * Called after any structural change — add, remove, indent/outdent click,
 * and on initial load. */
static void
startup_refresh_layout (GtkBuilder *builder)
{
  GtkListBox *listbox = GTK_LIST_BOX (gtk_builder_get_object (builder, "startup-entries-listbox"));
  if (listbox == nullptr)
    return;

  GHashTable *id_map = startup_build_id_map (listbox);
  GList *children = gtk_container_get_children (GTK_CONTAINER (listbox));
  int idx = 0;
  for (GList *l = children; l != nullptr; l = l->next, idx++) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW (l->data);
    if (g_object_get_data (G_OBJECT (row), STARTUP_ROW_TAG) == nullptr)
      continue;

    int depth = startup_row_depth (row, id_map);
    GtkWidget *row_box = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "axan-row-box"));
    if (row_box != nullptr)
      gtk_widget_set_margin_start (row_box, 8 + depth * STARTUP_INDENT_STEP_PX);

    /* Indent disabled if this is the first row (nothing to nest under).
     * Outdent disabled if already at root depth (no parent to escape). */
    GtkWidget *indent_btn = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "indent-btn"));
    GtkWidget *outdent_btn = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "outdent-btn"));
    if (indent_btn != nullptr)
      gtk_widget_set_sensitive (indent_btn, idx > 0);
    if (outdent_btn != nullptr)
      gtk_widget_set_sensitive (outdent_btn, depth > 0);

    /* Up disabled if no preceding sibling under the same parent; Down
     * disabled if no following sibling. Computed by scanning the listbox
     * past this row's subtree extent for a matching parent_id. */
    GtkWidget *up_btn = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "up-btn"));
    GtkWidget *down_btn = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "down-btn"));
    if (up_btn != nullptr || down_btn != nullptr) {
      const char *pid = startup_row_get_parent_id (row);
      if (up_btn != nullptr) {
        gboolean has_prev = startup_find_sibling_index (listbox, idx, -1, pid) >= 0;
        gtk_widget_set_sensitive (up_btn, has_prev);
      }
      if (down_btn != nullptr) {
        int subtree_end = startup_subtree_end (listbox, idx, id_map);
        gboolean has_next = startup_find_sibling_index (listbox, subtree_end - 1, 1, pid) >= 0;
        gtk_widget_set_sensitive (down_btn, has_next);
      }
    }
  }
  g_list_free (children);
  g_hash_table_destroy (id_map);
}

/* Iterate the listbox, marshal each tagged row into an (id, parent_id, dir,
 * cmd) tuple, write the full a(ssss) GVariant back to the profile. Drops any
 * row whose parent_id doesn't resolve (orphaned) or whose own id is empty
 * (shouldn't happen via the editor but defensive). */
static void
startup_save_entries (GtkBuilder *builder, GSettings *profile)
{
  GtkListBox *listbox = GTK_LIST_BOX (gtk_builder_get_object (builder, "startup-entries-listbox"));
  if (listbox == nullptr || profile == nullptr)
    return;

  /* First pass: collect known ids so the second pass can drop orphans
   * whose parent_id no longer references a present row. */
  GHashTable *known_ids = g_hash_table_new (g_str_hash, g_str_equal);
  GList *children = gtk_container_get_children (GTK_CONTAINER (listbox));
  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW (l->data);
    if (g_object_get_data (G_OBJECT (row), STARTUP_ROW_TAG) == nullptr)
      continue;
    const char *id = startup_row_get_id (row);
    if (*id != '\0')
      g_hash_table_add (known_ids, (gpointer) id);
  }

  GVariantBuilder vb;
  g_variant_builder_init (&vb, G_VARIANT_TYPE ("a(ssssssss)"));

  guint count = 0;
  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW (l->data);
    if (g_object_get_data (G_OBJECT (row), STARTUP_ROW_TAG) == nullptr)
      continue;
    const char *id = startup_row_get_id (row);
    if (*id == '\0')
      continue;
    const char *pid = startup_row_get_parent_id (row);
    /* Drop dangling parent references — promote the row to root rather than
     * persisting a broken pointer. */
    if (*pid != '\0' && !g_hash_table_contains (known_ids, pid))
      pid = "";

    GtkEntry *name_entry = nullptr, *dir_entry = nullptr, *cmd_entry = nullptr;
    startup_row_get_fields (row, &name_entry, &dir_entry, &cmd_entry);
    const char *name = name_entry ? gtk_entry_get_text (name_entry) : "";
    const char *dir = dir_entry ? gtk_entry_get_text (dir_entry) : "";
    const char *cmd = cmd_entry ? gtk_entry_get_text (cmd_entry) : "";
    /* Icon read-back from the row: stashed on the GtkListBoxRow via
     * startup_row_get_icon. The picker combobox (built in startup_create_row)
     * keeps this stash in sync; rows the user hasn't touched preserve the
     * original gsettings value through the load → row → save round trip. */
    const char *icon = startup_row_get_icon (row);
    /* Color + apply-to read straight off the row's combos. Empty color id =
     * "no color"; target defaults to "both". */
    GtkComboBox *color_combo = GTK_COMBO_BOX (g_object_get_data (G_OBJECT (row), "color-combo"));
    GtkComboBox *target_combo = GTK_COMBO_BOX (g_object_get_data (G_OBJECT (row), "target-combo"));
    const char *color = color_combo ? gtk_combo_box_get_active_id (color_combo) : "";
    const char *color_target = target_combo ? gtk_combo_box_get_active_id (target_combo) : "";
    if (color == nullptr) color = "";
    if (color_target == nullptr) color_target = "";
    g_variant_builder_add (&vb, "(ssssssss)", id, pid, name, dir, cmd, icon, color, color_target);
    count++;
  }
  g_list_free (children);
  g_hash_table_destroy (known_ids);

  GVariant *value = g_variant_builder_end (&vb);
  g_settings_set_value (profile, "default-launch-entries", value);
  /* set_value adopts a floating reference; no unref needed. */

  startup_refresh_layout (builder);
  startup_update_count (builder, count);
  startup_update_empty_state (builder, count);
}

/* Populate the listbox from the profile's current value. Clears any prior
 * rows first so this is safe to call on every profile-load. */
static void
startup_load_entries (GtkBuilder *builder, GSettings *profile)
{
  GtkListBox *listbox = GTK_LIST_BOX (gtk_builder_get_object (builder, "startup-entries-listbox"));
  if (listbox == nullptr || profile == nullptr)
    return;

  /* Clear existing rows. */
  GList *children = gtk_container_get_children (GTK_CONTAINER (listbox));
  for (GList *l = children; l != nullptr; l = l->next)
    gtk_container_remove (GTK_CONTAINER (listbox), GTK_WIDGET (l->data));
  g_list_free (children);

  gs_unref_variant GVariant *entries = g_settings_get_value (profile, "default-launch-entries");
  guint count = 0;
  if (entries != nullptr) {
    GVariantIter iter;
    g_variant_iter_init (&iter, entries);
    const char *id = nullptr;
    const char *parent_id = nullptr;
    const char *name = nullptr;
    const char *dir = nullptr;
    const char *cmd = nullptr;
    const char *icon = nullptr;
    const char *color = nullptr;
    const char *color_target = nullptr;
    while (g_variant_iter_loop (&iter, "(&s&s&s&s&s&s&s&s)", &id, &parent_id, &name, &dir, &cmd, &icon, &color, &color_target)) {
      /* Backfill a UUID for any entry that arrives with an empty id (e.g.
       * hand-edited gsettings, or a schema downgrade survivor). The row's
       * stash takes a strdup so the gs_free here is fine. */
      gs_free char *backfilled_id = nullptr;
      const char *effective_id = id;
      if (effective_id == nullptr || *effective_id == '\0') {
        backfilled_id = g_uuid_string_random ();
        effective_id = backfilled_id;
      }
      GtkWidget *row = startup_create_row (effective_id, parent_id, name, dir, cmd, icon, color, color_target);
      gtk_container_add (GTK_CONTAINER (listbox), row);
      count++;
    }
  }

  /* Heal any pre-existing pre-order violations in the loaded data — earlier
   * editor versions could leave forward references or out-of-place subtrees
   * in gsettings; this resorts them before the user can interact. Persist
   * the heal back to gsettings so the broken state doesn't survive across
   * sessions waiting for the user to make some other change. */
  if (startup_reorder_to_pre_order (listbox))
    startup_save_entries (builder, profile);
  startup_refresh_layout (builder);
  startup_update_count (builder, count);
  startup_update_empty_state (builder, count);
}

/* Footer count label: "N entries" or "no entries". */
static void
startup_update_count (GtkBuilder *builder, guint count)
{
  GtkLabel *label = GTK_LABEL (gtk_builder_get_object (builder, "startup-count-label"));
  if (label == nullptr)
    return;
  gs_free char *text = nullptr;
  if (count == 0)
    /* Just "no entries" — the empty-state body above already explains that
     * axan opens one default shell, and the longer string overflowed the
     * footer row's right edge next to the action buttons. */
    text = g_strdup (_("no entries"));
  else
    text = g_strdup_printf (ngettext ("%u entry", "%u entries", count), count);
  gtk_label_set_text (label, text);
}

/* Toggle the empty-state placeholder visibility. We hide the listbox (and its
 * scrolled container) when empty so the placeholder gets the vertical space;
 * the v3 wireframe explicitly chose the explanatory empty state. */
static void
startup_update_empty_state (GtkBuilder *builder, guint count)
{
  GtkWidget *scrolled = GTK_WIDGET (gtk_builder_get_object (builder, "startup-scrolled"));
  GtkWidget *empty = GTK_WIDGET (gtk_builder_get_object (builder, "startup-empty-state"));
  if (scrolled == nullptr || empty == nullptr)
    return;
  gboolean is_empty = (count == 0);
  gtk_widget_set_visible (scrolled, !is_empty);
  gtk_widget_set_visible (empty, is_empty);
}

/* Add button: append an empty row at root depth, focus the directory field.
 * Saving happens via the entry's "changed" signal once the user types
 * anything; we also save now to make the row real (so it survives a dialog
 * close+reopen even before the user fills in the fields). The id is a fresh
 * UUID generated here — it stays stable for the lifetime of the entry even
 * across reorders and renames, so children referencing it never break. */
static void
startup_add_clicked_cb (GtkButton *button G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;
  GtkBuilder *builder = the_pref_data->builder;
  GtkListBox *listbox = GTK_LIST_BOX (gtk_builder_get_object (builder, "startup-entries-listbox"));
  if (listbox == nullptr)
    return;
  gs_free char *new_id = g_uuid_string_random ();
  GtkWidget *row = startup_create_row (new_id, "", "", "", "", "", "", "");
  gtk_container_add (GTK_CONTAINER (listbox), row);
  GtkEntry *name_entry = nullptr;
  startup_row_get_fields (GTK_LIST_BOX_ROW (row), &name_entry, nullptr, nullptr);
  if (name_entry)
    gtk_widget_grab_focus (GTK_WIDGET (name_entry));
  startup_save_entries (builder, the_pref_data->selected_profile);
}

/* Copy-snippet handler for the help dialog rows. user_data carries the
 * snippet string (an interned const char* — no free). Copies to both
 * the CLIPBOARD selection (Ctrl-V destination) and PRIMARY (middle-click
 * paste) so the user can paste with either gesture. */
static void
startup_help_copy_clicked_cb (GtkButton *button, gpointer user_data)
{
  const char *snippet = (const char *) user_data;
  if (snippet == nullptr)
    return;
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (button));
  GtkClipboard *cb = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
  GtkClipboard *primary = gtk_clipboard_get_for_display (display, GDK_SELECTION_PRIMARY);
  gtk_clipboard_set_text (cb, snippet, -1);
  gtk_clipboard_set_text (primary, snippet, -1);
}

/* Build one row of the help dialog: a copyable snippet in monospace on
 * the left, dim description on the right, and a copy button. The snippet
 * pointer is captured by reference — pass string literals (which live
 * forever) or g_intern_string'd values. */
static GtkWidget *
startup_help_make_row (const char *snippet, const char *description)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *snip = gtk_label_new (nullptr);
  /* Pango span: monospace + slightly larger weight bias via <b>. */
  gs_free char *snip_markup = g_markup_printf_escaped (
    "<tt><b>%s</b></tt>", snippet);
  gtk_label_set_markup (GTK_LABEL (snip), snip_markup);
  gtk_label_set_xalign (GTK_LABEL (snip), 0.0);
  gtk_label_set_selectable (GTK_LABEL (snip), FALSE);
  gtk_widget_set_size_request (snip, 220, -1);
  gtk_box_pack_start (GTK_BOX (row), snip, FALSE, FALSE, 0);

  GtkWidget *desc = gtk_label_new (description);
  gtk_label_set_xalign (GTK_LABEL (desc), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (desc), TRUE);
  gtk_widget_set_hexpand (desc, TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (desc), "dim-label");
  gtk_box_pack_start (GTK_BOX (row), desc, TRUE, TRUE, 0);

  GtkWidget *copy = gtk_button_new_from_icon_name ("edit-copy-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (copy, _("Copy snippet to clipboard"));
  gtk_widget_set_valign (copy, GTK_ALIGN_CENTER);
  gtk_style_context_add_class (gtk_widget_get_style_context (copy), "flat");
  /* g_intern_string keeps the snippet alive forever — fine for these
   * static template strings. The button user_data is non-owning. */
  g_signal_connect (copy, "clicked",
                    G_CALLBACK (startup_help_copy_clicked_cb),
                    (gpointer) g_intern_string (snippet));
  gtk_box_pack_start (GTK_BOX (row), copy, FALSE, FALSE, 0);

  return row;
}

/* Add a section header label to the help dialog vbox. */
static void
startup_help_add_header (GtkBox *vbox, const char *text)
{
  GtkWidget *label = gtk_label_new (nullptr);
  gs_free char *markup = g_markup_printf_escaped ("<big><b>%s</b></big>", text);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_top (label, 8);
  gtk_box_pack_start (vbox, label, FALSE, FALSE, 0);
}

/* Add a wrap-line of plain prose to the help dialog vbox. */
static void
startup_help_add_prose (GtkBox *vbox, const char *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_box_pack_start (vbox, label, FALSE, FALSE, 0);
}

/* Modal help dialog using gtk_dialog_run — the proven pattern in the
 * rest of this codebase. Several non-modal approaches (raw GtkWindow,
 * GtkDialog with response signal) failed under this build's WM/GTK
 * combination — clicks never reached widgets inside the window. Trading
 * non-modality for reliability is worth it; the user dismisses the
 * dialog when they need to edit. */

/* Open a modal explaining how the Startup-tab Name templates work and what
 * variables they accept. Triggered by the `?` button next to the tab's
 * intro line. Keeping the reference out of placeholder text — placeholder
 * has to compete with the field width for legibility, and any non-trivial
 * template list loses immediately. */
static void
startup_help_clicked_cb (GtkButton *button G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr)
    return;
  GtkWindow *parent = GTK_WINDOW (gtk_builder_get_object (the_pref_data->builder, "preferences-dialog"));

  /* Modal dialog with gtk_dialog_run at the bottom. Same pattern used
   * elsewhere in this codebase (error dialogs, profile-rename popovers).
   * Earlier attempts at a non-modal GtkWindow had widgets that received
   * no mouse events — give up and ship the proven shape. */
  GtkWidget *dialog = gtk_dialog_new_with_buttons (
    _("Startup entry reference"),
    parent,
    GtkDialogFlags (GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
    _("_Close"), GTK_RESPONSE_CLOSE,
    nullptr);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 560, 520);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CLOSE);

  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *scrolled = gtk_scrolled_window_new (nullptr, nullptr);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_box_pack_start (GTK_BOX (content), scrolled, TRUE, TRUE, 0);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (vbox, 16);
  gtk_widget_set_margin_end (vbox, 16);
  gtk_widget_set_margin_top (vbox, 12);
  gtk_widget_set_margin_bottom (vbox, 12);
  gtk_container_add (GTK_CONTAINER (scrolled), vbox);

  /* Per-entry fields section (no copyable snippets — these are field
   * descriptions, not templates). */
  startup_help_add_header (GTK_BOX (vbox), _("Per-entry fields"));
  startup_help_add_prose (GTK_BOX (vbox),
    _("Name — optional sidebar label. Empty falls back to the shell's OSC title, "
      "then the current directory name, then a literal “shell”. Non-empty templates "
      "are rendered verbatim with the variables below substituted live."));
  startup_help_add_prose (GTK_BOX (vbox),
    _("Directory — the working directory the shell starts in. ~ expands to $HOME; "
      "absolute paths pass through."));
  startup_help_add_prose (GTK_BOX (vbox),
    _("Command — what to run in the shell. Runs inside $SHELL -c, so when the "
      "command exits the tab drops back to your shell rather than closing. Empty "
      "command opens your shell directly."));

  /* Variables. Each has a copy button. Snippet strings are literals so
   * g_intern_string in the helper just returns a stable pointer. */
  startup_help_add_header (GTK_BOX (vbox), _("Name template variables"));
  startup_help_add_prose (GTK_BOX (vbox),
    _("Case-insensitive. Underscores and hyphens are interchangeable. Unknown names "
      "emit literally so typos are visible."));
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$pwd", _("full current directory path (also $cwd)")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$~", _("current directory with $HOME collapsed to ~")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$dir", _("basename of the current directory")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$user", _("your username")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$hostname", _("short hostname (also $host)")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$shell", _("basename of $SHELL")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$title", _("current VTE window title (also $cmd)")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$branch", _("current git branch (empty if not in a repo)")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$repo", _("basename of the git repository's top-level dir")),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("${file:~/.cache/axan/status}",
      _("first line of the named file (max 128 bytes, trimmed). Useful for piping "
        "external state into the label via a sidecar file.")),
    FALSE, FALSE, 0);

  /* Examples — copyable composite templates. */
  startup_help_add_header (GTK_BOX (vbox), _("Examples"));
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$repo · $branch", "→ axan · main"),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("$~", "→ ~/Documents/GitHub/axan"),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("top in $dir", "→ top in axan"),
    FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox),
    startup_help_make_row ("${file:~/.cache/axan/status}",
      _("→ whatever's on the first line of that file")),
    FALSE, FALSE, 0);

  startup_help_add_prose (GTK_BOX (vbox),
    _("Refreshes happen on the shell's next OSC event (prompt redraw on cd, git "
      "checkout, etc.). Sidecar files therefore update on the next prompt rather "
      "than instantly."));

  gtk_widget_show_all (dialog);
  /* Modal blocking loop. Returns when the user clicks Close, hits the X,
   * or presses Escape. Destroy unconditionally afterwards. */
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
}

/* Call into the axan-server over DBus to retrieve a snapshot of the most-
 * recently-active window's sidebar tree as default-launch-entries tuples.
 * Returns a floating a(ssssss) GVariant on success, nullptr on failure
 * (DBus connection failure, server not running, etc — the error is shown
 * to the user via the supplied parent window). */
static GVariant *
startup_fetch_window_snapshot (GtkWindow *parent)
{
  gs_free_error GError *err = nullptr;
  gs_unref_object TerminalFactory *factory =
    terminal_factory_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                             GDBusProxyFlags (G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                                              G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
                                             TERMINAL_APPLICATION_ID,
                                             TERMINAL_FACTORY_OBJECT_PATH,
                                             nullptr, &err);
  if (factory == nullptr) {
    terminal_util_show_error_dialog (parent, nullptr, err,
                                     _("Could not contact axan to capture window state."));
    return nullptr;
  }

  GVariant *entries = nullptr;
  if (!terminal_factory_call_capture_window_state_sync (factory, &entries, nullptr, &err)) {
    terminal_util_show_error_dialog (parent, nullptr, err,
                                     _("Could not capture window state."));
    return nullptr;
  }
  return entries;  /* caller owns */
}

/* Write a captured a(ssssss) snapshot to `profile`'s default-launch-entries
 * key, then reload the editor so the new rows show up immediately. Shared
 * by both capture flavors (overwrite and create-new). */
static void
startup_apply_snapshot (GtkBuilder *builder, GSettings *profile, GVariant *entries)
{
  if (entries == nullptr || profile == nullptr)
    return;
  g_settings_set_value (profile, "default-launch-entries", entries);
  /* set_value adopts a floating reference; no unref needed here. */
  startup_load_entries (builder, profile);
}

/* "Capture current window" — overwrite the currently-edited profile's
 * Startup entries with a snapshot of the live window. Confirms first
 * since the operation is destructive. Empty snapshot (no axan windows
 * running) short-circuits with a notice rather than silently wiping
 * the user's existing entries. */
static void
startup_capture_overwrite_clicked_cb (GtkButton *button G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr || the_pref_data->selected_profile == nullptr)
    return;

  GtkWindow *parent = GTK_WINDOW (gtk_builder_get_object (the_pref_data->builder, "preferences-dialog"));
  gs_unref_variant GVariant *entries = startup_fetch_window_snapshot (parent);
  if (entries == nullptr)
    return;
  if (g_variant_n_children (entries) == 0) {
    terminal_util_show_error_dialog (parent, nullptr, nullptr, "%s",
                                     _("No axan window is open to capture from."));
    return;
  }

  gs_free char *msg = g_strdup_printf (
    _("Replace this profile's startup shells with %u captured from the current window?\n\nExisting entries will be lost. Commands won't be carried over — you'll need to re-add any."),
    (guint) g_variant_n_children (entries));
  GtkWidget *dialog = gtk_message_dialog_new (parent,
                                              GtkDialogFlags (GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                              GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                              "%s", msg);
  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                          _("_Cancel"), GTK_RESPONSE_CANCEL,
                          _("_Replace"), GTK_RESPONSE_ACCEPT,
                          nullptr);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
  gint resp = gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
  if (resp != GTK_RESPONSE_ACCEPT)
    return;

  startup_apply_snapshot (the_pref_data->builder, the_pref_data->selected_profile, entries);
}

/* "Capture as a new profile…" — fetches the snapshot, prompts for a profile
 * name with a self-contained GtkDialog (the existing profile_popup_dialog
 * helper in terminal-prefs.cc is static, so we roll our own here rather
 * than refactor cross-module visibility for one new caller). Creates the
 * profile cloned from the currently-edited one so visual settings carry
 * over, then writes the captured entries. The user clicks the new profile
 * in the profile listbox to switch to it — auto-switch would require
 * exposing listbox_select_profile, which is also static. */
static void
startup_capture_new_clicked_cb (GtkButton *button G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr)
    return;

  GtkWindow *parent = GTK_WINDOW (gtk_builder_get_object (the_pref_data->builder, "preferences-dialog"));

  /* Fetch the snapshot first so we don't bother the user for a name when
   * there's nothing to capture. */
  gs_unref_variant GVariant *entries = startup_fetch_window_snapshot (parent);
  if (entries == nullptr)
    return;
  if (g_variant_n_children (entries) == 0) {
    terminal_util_show_error_dialog (parent, nullptr, nullptr, "%s",
                                     _("No axan window is open to capture from."));
    return;
  }

  /* Name-entry dialog. Reuses the standard GtkMessageDialog chrome so it
   * matches the rest of preferences without depending on terminal-prefs's
   * popover internals. */
  GtkWidget *dialog = gtk_dialog_new_with_buttons (
    _("New profile from window"),
    parent,
    GtkDialogFlags (GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
    _("_Cancel"), GTK_RESPONSE_CANCEL,
    _("_Create"), GTK_RESPONSE_ACCEPT,
    nullptr);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (vbox, 16);
  gtk_widget_set_margin_end (vbox, 16);
  gtk_widget_set_margin_top (vbox, 12);
  gtk_widget_set_margin_bottom (vbox, 12);
  gtk_box_pack_start (GTK_BOX (content), vbox, TRUE, TRUE, 0);

  GtkWidget *label = gtk_label_new (_("Enter a name for the new profile:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

  GtkWidget *entry = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  gtk_box_pack_start (GTK_BOX (vbox), entry, FALSE, FALSE, 0);

  gtk_widget_show_all (dialog);
  gint resp = gtk_dialog_run (GTK_DIALOG (dialog));
  gs_free char *name = (resp == GTK_RESPONSE_ACCEPT)
    ? g_strdup (gtk_entry_get_text (GTK_ENTRY (entry))) : nullptr;
  gtk_widget_destroy (dialog);

  if (name == nullptr || *name == '\0')
    return;

  /* Clone from the currently-edited profile if any so visual settings carry
   * over. terminal_app_new_profile takes the BASE profile as a GSettings*
   * (nullptr → default-settings clone). */
  gs_free char *new_uuid = terminal_app_new_profile (terminal_app_get (),
                                                     the_pref_data->selected_profile,
                                                     name);
  if (new_uuid == nullptr) {
    terminal_util_show_error_dialog (parent, nullptr, nullptr, "%s",
                                     _("Failed to create the new profile."));
    return;
  }

  TerminalSettingsList *profiles_list = terminal_app_get_profiles_list (terminal_app_get ());
  gs_unref_object GSettings *new_profile =
    terminal_settings_list_ref_child (profiles_list, new_uuid);
  if (new_profile == nullptr)
    return;
  g_settings_set_value (new_profile, "default-launch-entries", entries);
}

/* Called each time the user switches away from a profile, so it's no longer being edited */
void
profile_prefs_unload (void)
{
  profile_prefs_signal_handlers_disconnect_all ();
  profile_prefs_settings_unbind_all ();
}

/* Called each time the user selects a new profile to edit */
void
profile_prefs_load (const char *uuid, GSettings *profile)
{
  GtkWidget *w;
  GtkBuilder *builder = the_pref_data->builder;
  guint i;

  profile_prefs_unload ();

  gtk_label_set_text (GTK_LABEL (gtk_builder_get_object (builder, "profile-uuid")),
                      uuid);

  profile_prefs_signal_connect (gtk_builder_get_object (builder, "default-size-reset-button"),
                                "clicked",
                                G_CALLBACK (default_size_reset_cb),
                                profile);
  profile_prefs_signal_connect (gtk_builder_get_object (builder, "cell-scale-reset-button"),
                                "clicked",
                                G_CALLBACK (cell_scale_reset_cb),
                                profile);

  /* Hook up the palette colorpickers and combo box */

  for (i = 0; i < TERMINAL_PALETTE_SIZE; ++i)
    {
      char name[32];
      char *text;

      g_snprintf (name, sizeof (name), "palette-colorpicker-%u", i);
      w = (GtkWidget *) gtk_builder_get_object (builder, name);

      g_object_set_data (G_OBJECT (w), "palette-entry-index", GUINT_TO_POINTER (i));

      text = g_strdup_printf (_("Choose Palette Color %u"), i);
      gtk_color_button_set_title (GTK_COLOR_BUTTON (w), text);
      g_free (text);

      text = g_strdup_printf (_("Palette entry %u"), i);
      gtk_widget_set_tooltip_text (w, text);
      g_free (text);

      profile_prefs_signal_connect (w, "notify::rgba",
                                    G_CALLBACK (palette_color_notify_cb),
                                    profile);
    }

  profile_palette_notify_colorpickers_cb (profile, TERMINAL_PROFILE_PALETTE_KEY, nullptr);
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_PALETTE_KEY,
                                G_CALLBACK (profile_palette_notify_colorpickers_cb),
                                nullptr);

  w = (GtkWidget *) gtk_builder_get_object (builder, "palette-combobox");
  profile_prefs_signal_connect (w, "notify::active",
                                G_CALLBACK (palette_scheme_combo_changed_cb),
                                profile);

  profile_palette_notify_scheme_combo_cb (profile, TERMINAL_PROFILE_PALETTE_KEY, GTK_COMBO_BOX (w));
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_PALETTE_KEY,
                                G_CALLBACK (profile_palette_notify_scheme_combo_cb),
                                w);

  /* Hook up the color scheme pickers and combo box */
  w = (GtkWidget *) gtk_builder_get_object (builder, "color-scheme-combobox");
  profile_prefs_signal_connect (w, "notify::active",
                                G_CALLBACK (color_scheme_combo_changed_cb),
                                profile);

  profile_colors_notify_scheme_combo_cb (profile, nullptr, GTK_COMBO_BOX (w));
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_FOREGROUND_COLOR_KEY,
                                G_CALLBACK (profile_colors_notify_scheme_combo_cb),
                                w);
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_BACKGROUND_COLOR_KEY,
                                G_CALLBACK (profile_colors_notify_scheme_combo_cb),
                                w);

  w = GTK_WIDGET (gtk_builder_get_object (builder, "custom-command-entry"));
  custom_command_entry_changed_cb (GTK_ENTRY (w));
  profile_prefs_signal_connect (w, "changed",
                                G_CALLBACK (custom_command_entry_changed_cb), nullptr);

  /* Startup tab — populate the list from this profile, wire the Add button.
   * Per-row entry-changed and remove-clicked signals are connected directly
   * inside startup_create_row; those widgets live for the duration of one
   * profile load and are torn down on profile_prefs_unload via the listbox
   * clear in startup_load_entries when a new profile loads. */
  startup_load_entries (builder, profile);
  profile_prefs_signal_connect (gtk_builder_get_object (builder, "startup-add-button"),
                                "clicked",
                                G_CALLBACK (startup_add_clicked_cb),
                                nullptr);
  profile_prefs_signal_connect (gtk_builder_get_object (builder, "startup-capture-button"),
                                "clicked",
                                G_CALLBACK (startup_capture_overwrite_clicked_cb),
                                nullptr);
  profile_prefs_signal_connect (gtk_builder_get_object (builder, "startup-capture-new-button"),
                                "clicked",
                                G_CALLBACK (startup_capture_new_clicked_cb),
                                nullptr);
  profile_prefs_signal_connect (gtk_builder_get_object (builder, "startup-help-button"),
                                "clicked",
                                G_CALLBACK (startup_help_clicked_cb),
                                nullptr);

  profile_prefs_signal_connect (gtk_builder_get_object (builder, "reset-compat-defaults-button"),
                                "clicked",
                                G_CALLBACK (reset_compat_defaults_cb),
                                profile);

  profile_prefs_settings_bind_with_mapping (profile,
                                            TERMINAL_PROFILE_BACKGROUND_COLOR_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "background-colorpicker"),
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  profile_prefs_settings_bind_with_mapping (profile,
                                            TERMINAL_PROFILE_BACKSPACE_BINDING_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "backspace-binding-combobox"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            vte_erase_binding_get_type, nullptr);
  profile_prefs_settings_bind (profile,
                               TERMINAL_PROFILE_BOLD_IS_BRIGHT_KEY,
                               gtk_builder_get_object (builder, "bold-is-bright-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_BOLD_COLOR_SAME_AS_FG_KEY,
                               gtk_builder_get_object (builder,
                                                       "bold-color-checkbutton"),
                               "active",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "bold-colorpicker"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_BOLD_COLOR_SAME_AS_FG_KEY,
                               w,
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_BOLD_COLOR_KEY,
                                            w,
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET |
							       G_SETTINGS_BIND_NO_SENSITIVITY),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  w = GTK_WIDGET (gtk_builder_get_object (builder, "cell-height-scale-spinbutton"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CELL_HEIGHT_SCALE_KEY,
                               gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (w)),
                               "value",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "cell-width-scale-spinbutton"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CELL_WIDTH_SCALE_KEY,
                               gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (w)),
                               "value",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY,
                               gtk_builder_get_object (builder,
                                                       "cursor-colors-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "cursor-foreground-colorpicker"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY,
                               w,
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_CURSOR_FOREGROUND_COLOR_KEY,
                                            w,
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET |
							       G_SETTINGS_BIND_NO_SENSITIVITY),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  w = GTK_WIDGET (gtk_builder_get_object (builder, "cursor-background-colorpicker"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY,
                               w,
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_CURSOR_BACKGROUND_COLOR_KEY,
                                            w,
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET |
							       G_SETTINGS_BIND_NO_SENSITIVITY),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY,
                               gtk_builder_get_object (builder,
                                                       "highlight-colors-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "highlight-foreground-colorpicker"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY,
                               w,
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_HIGHLIGHT_FOREGROUND_COLOR_KEY,
                                            w,
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET |
							       G_SETTINGS_BIND_NO_SENSITIVITY),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  w = GTK_WIDGET (gtk_builder_get_object (builder, "highlight-background-colorpicker"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY,
                               w,
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_HIGHLIGHT_BACKGROUND_COLOR_KEY,
                                            w,
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET |
							       G_SETTINGS_BIND_NO_SENSITIVITY),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_CURSOR_SHAPE_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "cursor-shape-combobox"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            vte_cursor_shape_get_type, nullptr);
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_CURSOR_BLINK_MODE_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "cursor-blink-mode-combobox"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            vte_cursor_blink_mode_get_type, nullptr);
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_TEXT_BLINK_MODE_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "text-blink-mode-combobox"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            vte_text_blink_mode_get_type, nullptr);

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CUSTOM_COMMAND_KEY,
                               gtk_builder_get_object (builder,
                                                       "custom-command-entry"),
                               "text",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "default-size-columns-spinbutton"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_DEFAULT_SIZE_COLUMNS_KEY,
                               gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (w)),
                               "value",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "default-size-rows-spinbutton"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_DEFAULT_SIZE_ROWS_KEY,
                               gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (w)),
                               "value",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_DELETE_BINDING_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "delete-binding-combobox"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            vte_erase_binding_get_type, nullptr);
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_EXIT_ACTION_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "exit-action-combobox"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            terminal_exit_action_get_type, nullptr);
  w = (GtkWidget*) gtk_builder_get_object (builder, "font-selector");
  gtk_font_chooser_set_filter_func (GTK_FONT_CHOOSER (w), monospace_filter, nullptr, nullptr);
#if GTK_CHECK_VERSION (3, 24, 0)
  gtk_font_chooser_set_level (GTK_FONT_CHOOSER (w),
			      GtkFontChooserLevel(GTK_FONT_CHOOSER_LEVEL_FAMILY |
						  GTK_FONT_CHOOSER_LEVEL_SIZE));
#endif

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_FONT_KEY,
                               w,
                               "font-name",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  profile_prefs_settings_bind_with_mapping (profile,
                                            TERMINAL_PROFILE_FOREGROUND_COLOR_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "foreground-colorpicker"),
                                            "rgba",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) s_to_rgba,
                                            (GSettingsBindSetMapping) rgba_to_s,
                                            nullptr, nullptr);

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_LOGIN_SHELL_KEY,
                               gtk_builder_get_object (builder,
                                                       "login-shell-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = GTK_WIDGET (gtk_builder_get_object (builder, "scrollback-lines-spinbutton"));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_SCROLLBACK_LINES_KEY,
                               gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (w)),
                               "value",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY,
                               gtk_builder_get_object (builder,
                                                       "scrollback-limited-checkbutton"),
                               "active",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY,
                               gtk_builder_get_object (builder,
                                                       "scrollback-box"),
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_with_mapping (profile,
                                            TERMINAL_PROFILE_SCROLLBAR_POLICY_KEY,
                                            gtk_builder_get_object (builder,
                                                                    "scrollbar-checkbutton"),
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) scrollbar_policy_to_bool,
                                            (GSettingsBindSetMapping) bool_to_scrollbar_policy,
                                            nullptr, nullptr);
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_SCROLL_ON_KEYSTROKE_KEY,
                               gtk_builder_get_object (builder,
                                                       "scroll-on-keystroke-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_SCROLL_ON_OUTPUT_KEY,
                               gtk_builder_get_object (builder,
                                                       "scroll-on-output-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_SCROLL_ON_INSERT_KEY,
                               gtk_builder_get_object (builder,
                                                       "scroll-on-insert-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY,
                               gtk_builder_get_object (builder,
                                                       "custom-font-checkbutton"),
                               "active",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN));

  w = (GtkWidget *) gtk_builder_get_object (builder, "preserve-working-directory-combobox");
  profile_prefs_settings_bind_with_mapping (profile, TERMINAL_PROFILE_PRESERVE_WORKING_DIRECTORY_KEY, w,
                                            "active",
                                            GSettingsBindFlags(G_SETTINGS_BIND_GET |
							       G_SETTINGS_BIND_SET),
                                            (GSettingsBindGetMapping) string_to_enum,
                                            (GSettingsBindSetMapping) enum_to_string,
                                            terminal_preserve_working_directory_get_type, nullptr);

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_USE_CUSTOM_COMMAND_KEY,
                               gtk_builder_get_object (builder,
                                                       "use-custom-command-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_USE_THEME_COLORS_KEY,
                               gtk_builder_get_object (builder,
                                                       "use-theme-colors-checkbutton"),
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_AUDIBLE_BELL_KEY,
                               gtk_builder_get_object (builder, "bell-checkbutton"),
                               "active",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  profile_prefs_settings_bind (profile,
                               TERMINAL_PROFILE_USE_CUSTOM_COMMAND_KEY,
                               gtk_builder_get_object (builder, "custom-command-entry-label"),
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind (profile,
                               TERMINAL_PROFILE_USE_CUSTOM_COMMAND_KEY,
                               gtk_builder_get_object (builder, "custom-command-entry"),
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind (profile,
                               TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY,
                               gtk_builder_get_object (builder, "font-selector"),
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind (profile,
                               TERMINAL_PROFILE_USE_THEME_COLORS_KEY,
                               gtk_builder_get_object (builder, "colors-box"),
                               "sensitive",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_INVERT_BOOLEAN |
						  G_SETTINGS_BIND_NO_SENSITIVITY));
  profile_prefs_settings_bind_writable (profile,
                                        TERMINAL_PROFILE_PALETTE_KEY,
                                        gtk_builder_get_object (builder, "palette-box"),
                                        "sensitive",
                                        FALSE);

  /* Scrolling options */
  w = (GtkWidget *) gtk_builder_get_object (builder, "scrollback-warning");
  profile_scrollback_warning_update_cb (profile, nullptr, w);
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY,
                                G_CALLBACK (profile_scrollback_warning_update_cb),
                                w);
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_SCROLLBACK_LINES_KEY,
                                G_CALLBACK (profile_scrollback_warning_update_cb),
                                w);

  /* Compatibility options */
  w = (GtkWidget *) gtk_builder_get_object (builder, "encoding-combobox");
  profile_prefs_signal_connect (w, "changed",
                                G_CALLBACK (profile_encoding_combo_changed_cb),
                                profile);

  profile_notify_encoding_combo_cb (profile, TERMINAL_PROFILE_ENCODING_KEY, GTK_COMBO_BOX (w));
  profile_prefs_signal_connect (profile, "changed::" TERMINAL_PROFILE_ENCODING_KEY,
                                G_CALLBACK (profile_notify_encoding_combo_cb),
                                w);

  w = (GtkWidget *) gtk_builder_get_object (builder, "cjk-ambiguous-width-combobox");
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_CJK_UTF8_AMBIGUOUS_WIDTH_KEY,
                               w,
                               "active-id",
                               GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));

  w = (GtkWidget *) gtk_builder_get_object (builder, "enable-sixel-checkbutton");
  profile_prefs_settings_bind (profile, TERMINAL_PROFILE_ENABLE_SIXEL_KEY, w,
                               "active",
			       GSettingsBindFlags(G_SETTINGS_BIND_GET |
						  G_SETTINGS_BIND_SET));
  gtk_widget_set_visible (w, (vte_get_feature_flags() & VTE_FEATURE_FLAG_SIXEL) != 0);
}

/* Called once per Preferences window, to destroy stuff that doesn't depend on the profile being edited */
void
profile_prefs_destroy (void)
{
  profile_prefs_unload ();

  g_array_free (the_pref_data->profile_signals, TRUE);
  g_array_free (the_pref_data->profile_bindings, TRUE);
}

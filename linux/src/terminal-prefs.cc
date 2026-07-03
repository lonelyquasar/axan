/*
 * Copyright © 2001, 2002 Havoc Pennington, Red Hat Inc.
 * Copyright © 2008, 2011, 2012, 2013 Christian Persch
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

#include "config.h"

#include <string.h>
#include <string>

#include <uuid.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "axan-icons-toml.hh"
#include "keybindings-toml.hh"
#include "profile-editor.hh"
#include "profile-toml.hh"
#include "settings-toml.hh"
#include "terminal-prefs.hh"
#include "terminal-accels.hh"
#include "terminal-app.hh"
#include "terminal-debug.hh"
#include "terminal-schemas.hh"
#include "terminal-util.hh"
#include "terminal-profiles-list.hh"
#include "terminal-libgsystem.hh"

PrefData *the_pref_data = nullptr;  /* global */

/* Bottom */

static void
prefs_dialog_help_button_clicked_cb (GtkWidget *button,
                                     PrefData *data)
{
  terminal_util_show_help ("pref");
}

static void
prefs_dialog_close_button_clicked_cb (GtkWidget *button,
                                      PrefData *data)
{
  gtk_widget_destroy (data->dialog);
}

/* Sidebar */

static inline GSimpleAction *
lookup_action (GtkWindow *window,
               const char *name)
{
  GAction *action;

  action = g_action_map_lookup_action (G_ACTION_MAP (window), name);
  g_return_val_if_fail (action != nullptr, nullptr);

  return G_SIMPLE_ACTION (action);
}

/* Update the sidebar (visibility of icons, sensitivity of menu entries) to reflect the default and the selected profiles. */
static void
listbox_update (GtkListBox *box)
{
  int i;
  GtkListBoxRow *row;
  GSettings *profile;
  gs_unref_object GSettings *default_profile;
  GtkStack *stack;
  GtkMenuButton *button;

  default_profile = terminal_settings_list_ref_default_child (the_pref_data->profiles_list);

  /* GTK+ doesn't seem to like if a popover is assigned to multiple buttons at once
   * (not even temporarily), so make sure to remove it from the previous button first. */
  for (i = 0; (row = gtk_list_box_get_row_at_index (box, i)) != nullptr; i++) {
    button = (GtkMenuButton*)g_object_get_data (G_OBJECT (row), "popover-button");
    gtk_menu_button_set_popover (button, nullptr);
  }

  for (i = 0; (row = gtk_list_box_get_row_at_index (box, i)) != nullptr; i++) {
    profile = (GSettings*)g_object_get_data (G_OBJECT (row), "gsettings");

    gboolean is_selected_profile = (profile != nullptr && profile == the_pref_data->selected_profile);
    gboolean is_default_profile = (profile != nullptr && profile == default_profile);

    stack = (GtkStack*)g_object_get_data (G_OBJECT (row), "home-stack");
    gtk_stack_set_visible_child_name (stack, is_default_profile ? "home" : "placeholder");

    stack = (GtkStack*)g_object_get_data (G_OBJECT (row), "popover-stack");
    gtk_stack_set_visible_child_name (stack, is_selected_profile ? "button" : "placeholder");
    if (is_selected_profile) {
      g_simple_action_set_enabled (lookup_action (GTK_WINDOW (the_pref_data->dialog), "delete"), !is_default_profile);
      g_simple_action_set_enabled (lookup_action (GTK_WINDOW (the_pref_data->dialog), "set-as-default"), !is_default_profile);

      GtkPopover *popover_menu = GTK_POPOVER (gtk_builder_get_object (the_pref_data->builder, "popover-menu"));
      button = (GtkMenuButton*)g_object_get_data (G_OBJECT (row), "popover-button");
      gtk_menu_button_set_popover (button, GTK_WIDGET (popover_menu));
      gtk_popover_set_relative_to (popover_menu, GTK_WIDGET (button));
    }
  }
}

static void
update_window_title (void)
{
  GtkListBoxRow *row = the_pref_data->selected_list_box_row;
  if (row == nullptr)
    return;

  GSettings *profile = (GSettings*)g_object_get_data (G_OBJECT (row), "gsettings");
  GtkLabel *label = (GtkLabel*)g_object_get_data (G_OBJECT (row), "label");
  const char *text = gtk_label_get_text (label);
  gs_free char *subtitle;
  gs_free char *title;

  if (profile == nullptr) {
    subtitle = g_strdup (text);
  } else {
    subtitle = g_strdup_printf (_("Profile “%s”"), text);
  }

  title = g_strdup_printf (_("Preferences – %s"), subtitle);
  gtk_window_set_title (GTK_WINDOW (the_pref_data->dialog), title);
}

/* A new entry is selected in the sidebar */
static void
listbox_row_selected_cb (GtkListBox *box,
                         GtkListBoxRow *row,
                         GtkStack *stack)
{
  profile_prefs_unload ();

  /* row can be nullptr intermittently during a profile meta operations */
  g_free (the_pref_data->selected_profile_uuid);
  if (row != nullptr) {
    the_pref_data->selected_profile = (GSettings*)g_object_get_data (G_OBJECT (row), "gsettings");
    the_pref_data->selected_profile_uuid = g_strdup ((char const*)g_object_get_data (G_OBJECT (row), "uuid"));
  } else {
    the_pref_data->selected_profile = nullptr;
    the_pref_data->selected_profile_uuid = nullptr;
  }
  the_pref_data->selected_list_box_row = row;

  listbox_update (box);

  if (row != nullptr) {
    if (the_pref_data->selected_profile != nullptr) {
      profile_prefs_load (the_pref_data->selected_profile_uuid, the_pref_data->selected_profile);
    }

    char const* stack_child_name = (char const*)g_object_get_data (G_OBJECT (row), "stack_child_name");
    gtk_stack_set_visible_child_name (stack, stack_child_name);
  }

  update_window_title ();
}

/* A profile's name changed, perhaps externally */
static void
profile_name_changed_cb (GtkLabel      *label,
                         GParamSpec    *pspec,
                         GtkListBoxRow *row)
{
  gtk_list_box_row_changed (row);  /* trigger re-sorting */

  if (row == the_pref_data->selected_list_box_row)
    update_window_title ();
}

/* Select a profile in the sidebar by UUID */
static gboolean
listbox_select_profile (const char *uuid)
{
  GtkListBoxRow *row;
  for (int i = 0; (row = gtk_list_box_get_row_at_index (the_pref_data->listbox, i)) != nullptr; i++) {
    const char *rowuuid = (char const*) g_object_get_data (G_OBJECT (row), "uuid");
    if (g_strcmp0 (rowuuid, uuid) == 0) {
      g_signal_emit_by_name (row, "activate");
      return TRUE;
    }
  }
  return FALSE;
}

/* Create a new profile now, select it, update the UI. */
static void
profile_new_now (const char *name)
{
  gs_free char *uuid = terminal_app_new_profile (terminal_app_get (), nullptr, name);

  listbox_select_profile (uuid);
}

/* Clone the selected profile now, select it, update the UI. */
static void
profile_clone_now (const char *name)
{
  if (the_pref_data->selected_profile == nullptr)
    return;

  gs_free char *uuid = terminal_app_new_profile (terminal_app_get (), the_pref_data->selected_profile, name);

  listbox_select_profile (uuid);
}

/* Rename the selected profile now, update the UI. */
static void
profile_rename_now (const char *name)
{
  if (the_pref_data->selected_profile == nullptr)
    return;

  /* This will automatically trigger a call to profile_name_changed_cb(). */
  g_settings_set_string (the_pref_data->selected_profile, TERMINAL_PROFILE_VISIBLE_NAME_KEY, name);
}

/* Delete the selected profile now, update the UI. */
static void
profile_delete_now (const char *dummy)
{
  if (the_pref_data->selected_profile == nullptr)
    return;

  /* Prepare to select the next one, or if there's no such then the previous one. */
  int index = gtk_list_box_row_get_index (the_pref_data->selected_list_box_row);
  GtkListBoxRow *new_selected_row = gtk_list_box_get_row_at_index (the_pref_data->listbox, index + 1);
  if (new_selected_row == nullptr)
    new_selected_row = gtk_list_box_get_row_at_index (the_pref_data->listbox, index - 1);
  GSettings *new_selected_profile = (GSettings*)g_object_get_data (G_OBJECT (new_selected_row), "gsettings");
  gs_free char *uuid = nullptr;
  if (new_selected_profile != nullptr)
    uuid = terminal_settings_list_dup_uuid_from_child (the_pref_data->profiles_list, new_selected_profile);

  terminal_app_remove_profile (terminal_app_get (), the_pref_data->selected_profile);

  listbox_select_profile (uuid);
}

/* "Set as default" selected. Do it now without asking for confirmation. */
static void
profile_set_as_default_cb (GSimpleAction *simple,
                           GVariant      *parameter,
                           gpointer       user_data)
{
  if (the_pref_data->selected_profile_uuid == nullptr)
    return;

  /* This will automatically trigger a call to listbox_update() via "default-changed". */
  terminal_settings_list_set_default_child (the_pref_data->profiles_list, the_pref_data->selected_profile_uuid);
}

/* Show an info bar inside the prefs dialog. Light-touch acknowledgement
 * for export/import operations that don't otherwise produce visible
 * feedback (unlike Rename which immediately re-labels a row). */
static void
prefs_show_brief (const char *message_format, ...) G_GNUC_PRINTF (1, 2);
static void
prefs_show_brief (const char *message_format, ...)
{
  va_list args;
  va_start (args, message_format);
  gs_free char *msg = g_strdup_vprintf (message_format, args);
  va_end (args);

  GtkWindow *parent = GTK_WINDOW (the_pref_data->dialog);
  GtkWidget *dialog = gtk_message_dialog_new (parent,
                                              GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_INFO,
                                              GTK_BUTTONS_OK,
                                              "%s", msg);
  g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), nullptr);
  gtk_widget_show (dialog);
}

static void
prefs_show_error (GError *error)
{
  GtkWindow *parent = GTK_WINDOW (the_pref_data->dialog);
  GtkWidget *dialog = gtk_message_dialog_new (parent,
                                              GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_OK,
                                              "%s",
                                              error ? error->message : "(unknown error)");
  g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), nullptr);
  gtk_widget_show (dialog);
}

/* Per-file accumulator: label (free with g_free) + g_strdup'd key names. */
struct ImportFileSummary {
  char *label;
  GPtrArray *keys;
};

static void
import_file_summary_clear (ImportFileSummary *fs)
{
  g_free (fs->label);
  if (fs->keys != nullptr)
    g_ptr_array_free (fs->keys, TRUE);
}

/* Look up a profile's visible-name via its UUID. Falls back to a short UUID
 * stem when the profile can't be opened (shouldn't happen post-import). */
static char *
profile_display_label (const char *uuid)
{
  gs_free char *path = g_strconcat (TERMINAL_PROFILES_PATH_PREFIX,
                                    ":", uuid, "/", nullptr);
  gs_unref_object GSettings *profile =
    g_settings_new_with_path (TERMINAL_PROFILE_SCHEMA, path);
  gs_free char *name = g_settings_get_string (profile, "visible-name");
  if (name != nullptr && *name != '\0')
    return g_strdup_printf ("profile \"%s\"", name);
  return g_strdup_printf ("profile %.8s…", uuid);
}

/* "Import Settings from TOML" — re-reads settings.toml, keybindings.toml,
 * and every profile .toml from ~/.config/axan/ into dconf. No file chooser:
 * design is that auto-export keeps these files in sync with dconf, so this
 * button exists only for the "I hand-edited the TOMLs and want them applied"
 * workflow. Mirrors --import-all on the CLI with the default directory.
 *
 * The dialog shown afterwards reports how many TOMLs were read AND which
 * settings actually changed. ≤10 changes total: each key listed inline.
 * >10 changes: per-file count summary so the dialog stays scannable. */
static void
import_all_cb (GSimpleAction *simple,
               GVariant      *parameter,
               gpointer       user_data)
{
  gs_free char *dir = g_build_filename (g_get_user_config_dir (), "axan", nullptr);
  gs_free_error GError *error = nullptr;
  guint files_imported = 0;
  guint total_changes = 0;
  GArray *per_file = g_array_new (FALSE, FALSE, sizeof (ImportFileSummary));
  g_array_set_clear_func (per_file, (GDestroyNotify) import_file_summary_clear);

  /* settings.toml */
  gs_free char *settings_path = g_build_filename (dir, "settings.toml", nullptr);
  if (g_file_test (settings_path, G_FILE_TEST_EXISTS)) {
    GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
    if (!axan_settings_toml_import (settings_path, keys, &error)) {
      g_ptr_array_free (keys, TRUE);
      g_array_unref (per_file);
      prefs_show_error (error);
      return;
    }
    files_imported++;
    total_changes += keys->len;
    ImportFileSummary fs = { g_strdup ("settings.toml"), keys };
    g_array_append_val (per_file, fs);
  }

  /* keybindings.toml */
  gs_free char *kb_path = g_build_filename (dir, "keybindings.toml", nullptr);
  if (g_file_test (kb_path, G_FILE_TEST_EXISTS)) {
    GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
    if (!axan_keybindings_toml_import (kb_path, keys, &error)) {
      g_ptr_array_free (keys, TRUE);
      g_array_unref (per_file);
      prefs_show_error (error);
      return;
    }
    files_imported++;
    total_changes += keys->len;
    ImportFileSummary fs = { g_strdup ("keybindings.toml"), keys };
    g_array_append_val (per_file, fs);
  }

  /* per-profile .toml files */
  gs_free char *prof_dir = g_build_filename (dir, "profiles", nullptr);
  GDir *pd = g_dir_open (prof_dir, 0, nullptr);
  if (pd != nullptr) {
    const char *name;
    while ((name = g_dir_read_name (pd)) != nullptr) {
      if (!g_str_has_suffix (name, ".toml"))
        continue;
      gs_free char *p = g_build_filename (prof_dir, name, nullptr);
      gs_free char *out_uuid = nullptr;
      GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
      if (!axan_profile_toml_import (p, &out_uuid, keys, &error)) {
        g_ptr_array_free (keys, TRUE);
        g_array_unref (per_file);
        g_dir_close (pd);
        prefs_show_error (error);
        return;
      }
      files_imported++;
      total_changes += keys->len;
      ImportFileSummary fs = { profile_display_label (out_uuid), keys };
      g_array_append_val (per_file, fs);
    }
    g_dir_close (pd);
  }

  /* Format the feedback. */
  if (files_imported == 0) {
    prefs_show_brief (_("No TOML files found under %s"), dir);
    g_array_unref (per_file);
    return;
  }

  GString *msg = g_string_new (nullptr);
  g_string_append_printf (msg,
                          ngettext ("Imported %u file from %s",
                                    "Imported %u files from %s",
                                    files_imported),
                          files_imported, dir);

  if (total_changes == 0) {
    g_string_append (msg, "\n\n");
    g_string_append (msg, _("No settings changed — TOMLs already match dconf."));
  } else if (total_changes <= 10) {
    g_string_append (msg, "\n\n");
    g_string_append_printf (msg,
                            ngettext ("Changed %u setting:",
                                      "Changed %u settings:",
                                      total_changes),
                            total_changes);
    for (guint i = 0; i < per_file->len; i++) {
      ImportFileSummary *fs = &g_array_index (per_file, ImportFileSummary, i);
      if (fs->keys->len == 0)
        continue;
      g_string_append_printf (msg, "\n  • %s: ", fs->label);
      for (guint k = 0; k < fs->keys->len; k++) {
        if (k > 0) g_string_append (msg, ", ");
        g_string_append (msg, (const char *) g_ptr_array_index (fs->keys, k));
      }
    }
  } else {
    g_string_append (msg, "\n\n");
    g_string_append_printf (msg,
                            ngettext ("Changed %u setting:",
                                      "Changed %u settings:",
                                      total_changes),
                            total_changes);
    for (guint i = 0; i < per_file->len; i++) {
      ImportFileSummary *fs = &g_array_index (per_file, ImportFileSummary, i);
      if (fs->keys->len == 0)
        continue;
      g_string_append_printf (msg,
                              ngettext ("\n  • %u in %s",
                                        "\n  • %u in %s",
                                        fs->keys->len),
                              fs->keys->len, fs->label);
    }
  }

  prefs_show_brief ("%s", msg->str);
  g_string_free (msg, TRUE);
  g_array_unref (per_file);
}

/* Add a section header to the icons help dialog. Bold + slightly larger
 * via Pango markup. */
static void
icons_help_add_header (GtkBox *vbox, const char *text)
{
  GtkWidget *label = gtk_label_new (nullptr);
  gs_free char *markup = g_markup_printf_escaped ("<big><b>%s</b></big>", text);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_top (label, 8);
  gtk_box_pack_start (vbox, label, FALSE, FALSE, 0);
}

/* Wrap-line of prose. */
static void
icons_help_add_prose (GtkBox *vbox, const char *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_box_pack_start (vbox, label, FALSE, FALSE, 0);
}

/* "?" button next to the icon importer — explains the picker contract,
 * the validation rules, and the advanced escape hatch for non-SVG
 * formats. Mirrors the Startup tab's help-dialog shape. */
static void
icons_help_clicked_cb (GtkButton *button G_GNUC_UNUSED,
                       gpointer   user_data G_GNUC_UNUSED)
{
  if (the_pref_data == nullptr)
    return;
  GtkWindow *parent = GTK_WINDOW (the_pref_data->dialog);

  GtkWidget *dialog = gtk_dialog_new_with_buttons (
    _("Icon registry reference"),
    parent,
    GtkDialogFlags (GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
    _("_Close"), GTK_RESPONSE_CLOSE,
    nullptr);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 580, 540);
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

  icons_help_add_header (GTK_BOX (vbox), _("Workflow"));
  icons_help_add_prose (GTK_BOX (vbox),
    _("Drop .svg files into ~/.config/axan/icons/, then click Import Icons "
      "from Folder (or run `axan --import-icons`). Each file is validated "
      "and passing names appear in the icon picker on the Startup tab."));

  icons_help_add_header (GTK_BOX (vbox), _("Validation rules"));
  icons_help_add_prose (GTK_BOX (vbox),
    _("• Filename must match ^[a-z0-9][a-z0-9_-]*\\.svg$ — lowercase ASCII, "
      "starts alnum, dashes/underscores allowed, .svg extension. Rejects "
      "“My Cool Icon.svg” and the like so the registered name is safe in "
      "TOML and dconf."));
  icons_help_add_prose (GTK_BOX (vbox),
    _("• File size must be under 100 KiB. A reasonable icon SVG is a few "
      "KB; anything larger is usually embedded raster data or a misnamed "
      "asset."));
  icons_help_add_prose (GTK_BOX (vbox),
    _("• File must render via gdk-pixbuf at 24×24 — the same loader the "
      "sidebar uses. If validation passes, picker rendering will too."));
  icons_help_add_prose (GTK_BOX (vbox),
    _("Rejected files stay in the staging folder; the import dialog lists "
      "the reason so you can fix and re-import."));

  icons_help_add_header (GTK_BOX (vbox), _("Picker contract"));
  icons_help_add_prose (GTK_BOX (vbox),
    _("The picker reads ~/.config/axan/icons.toml. Files in the staging "
      "folder that haven’t been imported do NOT appear in the dropdown — "
      "the registry is the validation gate. Built-in icons (terminal, ssh, "
      "git, …) come from the GTK icon theme and are always available."));

  icons_help_add_header (GTK_BOX (vbox), _("Non-SVG formats"));
  icons_help_add_prose (GTK_BOX (vbox),
    _("The importer is SVG-only because vectors scale cleanly to any "
      "sidebar size. If you’d rather use a PNG, a JPG, a WebP, an animated "
      "GIF, or for some reason a frame from an MP4 as your icon, the "
      "resolver isn’t going to stop you:"));
  icons_help_add_prose (GTK_BOX (vbox),
    _("  1. Drop the file into ~/.config/axan/icons/ (any filename, any "
      "extension).\n"
      "  2. Edit ~/.config/axan/profiles/<uuid>.toml directly and set the "
      "icon field on a launch entry to the bare filename, e.g. "
      "icon = \"my-thing.png\".\n"
      "  3. Click Import Settings from TOML (or run `axan --import-all`)."));
  icons_help_add_prose (GTK_BOX (vbox),
    _("Whatever gdk-pixbuf can load, the sidebar will render. The icon "
      "won’t appear in the picker dropdown — that’s reserved for icons "
      "that passed validation. The path is deliberately rough, and "
      "support for more icon formats isn’t on the roadmap."));

  gtk_widget_show_all (dialog);
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
}

/* "Import Icons from Folder" — validation gate for SVGs dropped in
 * ~/.config/axan/icons/. Mirrors `axan --import-icons` on the CLI. */
static void
import_icons_cb (GSimpleAction *simple,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GPtrArray *imported = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *rejected = g_ptr_array_new_with_free_func (axan_icon_rejection_free);
  gs_free_error GError *error = nullptr;

  if (!axan_icons_toml_import (nullptr, imported, rejected, &error)) {
    g_ptr_array_free (imported, TRUE);
    g_ptr_array_free (rejected, TRUE);
    prefs_show_error (error);
    return;
  }

  GString *msg = g_string_new (nullptr);
  g_string_append_printf (msg,
                          ngettext ("Registered %u icon.",
                                    "Registered %u icons.",
                                    imported->len),
                          imported->len);
  if (imported->len > 0 && imported->len <= 10) {
    g_string_append (msg, "\n  ");
    for (guint i = 0; i < imported->len; i++) {
      if (i > 0) g_string_append (msg, ", ");
      g_string_append (msg, (const char *) g_ptr_array_index (imported, i));
    }
  }
  if (rejected->len > 0) {
    g_string_append (msg, "\n\n");
    g_string_append_printf (msg,
                            ngettext ("Rejected %u file:",
                                      "Rejected %u files:",
                                      rejected->len),
                            rejected->len);
    for (guint i = 0; i < rejected->len; i++) {
      AxanIconRejection *r =
        (AxanIconRejection *) g_ptr_array_index (rejected, i);
      g_string_append_printf (msg, "\n  • %s: %s", r->name, r->reason);
    }
  }

  prefs_show_brief ("%s", msg->str);
  g_string_free (msg, TRUE);
  g_ptr_array_free (imported, TRUE);
  g_ptr_array_free (rejected, TRUE);
}


static void
popover_dialog_cancel_clicked_cb (GtkButton *button,
                                  gpointer user_data)
{
  GtkPopover *popover_dialog = GTK_POPOVER (gtk_builder_get_object (the_pref_data->builder, "popover-dialog"));

  gtk_popover_popdown (popover_dialog);
}

static void
popover_dialog_ok_clicked_cb (GtkButton *button,
                              void (*fn) (const char *))
{
  GtkEntry *entry = GTK_ENTRY (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-entry"));
  const char *name = gtk_entry_get_text (entry);

  /* Perform what we came for */
  (*fn) (name);

  /* Hide/popdown the popover */
  popover_dialog_cancel_clicked_cb (button, nullptr);
}

static void
popover_dialog_closed_cb (GtkPopover *popover,
                          gpointer   user_data)
{

  GtkEntry *entry = GTK_ENTRY (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-entry"));
  gtk_entry_set_text (entry, "");

  GtkButton *ok = GTK_BUTTON (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-ok"));
  GtkButton *cancel = GTK_BUTTON (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-cancel"));

  g_signal_handlers_disconnect_matched (ok, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr,
                                        (void*)popover_dialog_ok_clicked_cb, nullptr);
  g_signal_handlers_disconnect_matched (cancel, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr,
                                        (void*)popover_dialog_cancel_clicked_cb, nullptr);
  g_signal_handlers_disconnect_matched (popover, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr,
                                        (void*)popover_dialog_closed_cb, nullptr);
}


/* Updates the OK button's sensitivity (insensitive if entry field is empty or whitespace only).
 * The entry's initial value and OK's initial sensitivity have to match in the .ui file. */
static void
popover_dialog_notify_text_cb (GtkEntry   *entry,
                               GParamSpec *pspec,
                               GtkWidget  *ok)
{
  gs_free char *text = g_strchomp (g_strdup (gtk_entry_get_text (entry)));
  gtk_widget_set_sensitive (ok, text[0] != '\0');
}


/* Common dialog for entering new profile name, or confirming deletion */
static void
profile_popup_dialog (GtkWidget *relative_to,
                      const char *header,
                      const char *body,
                      const char *entry_text,
                      const char *ok_text,
                      void (*fn) (const char *))
{
  GtkLabel *label1 = GTK_LABEL (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-label1"));
  gtk_label_set_text (label1, header);

  GtkLabel *label2 = GTK_LABEL (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-label2"));
  gtk_label_set_text (label2, body);

  GtkEntry *entry = GTK_ENTRY (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-entry"));
  if (entry_text != nullptr) {
    gtk_entry_set_text (entry, entry_text);
    gtk_widget_show (GTK_WIDGET (entry));
  } else {
    gtk_entry_set_text (entry, ".");  /* to make the OK button sensitive */
    gtk_widget_hide (GTK_WIDGET (entry));
  }

  GtkButton *ok = GTK_BUTTON (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-ok"));
  gtk_button_set_label (ok, ok_text);
  GtkButton *cancel = GTK_BUTTON (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-cancel"));
  GtkPopover *popover_dialog = GTK_POPOVER (gtk_builder_get_object (the_pref_data->builder, "popover-dialog"));

  g_signal_connect (ok, "clicked", G_CALLBACK (popover_dialog_ok_clicked_cb), (void*)fn);
  g_signal_connect (cancel, "clicked", G_CALLBACK (popover_dialog_cancel_clicked_cb), nullptr);
  g_signal_connect (popover_dialog, "closed", G_CALLBACK (popover_dialog_closed_cb), nullptr);

  gtk_popover_set_relative_to (popover_dialog, relative_to);
  gtk_popover_set_position (popover_dialog, GTK_POS_BOTTOM);
  gtk_popover_set_default_widget (popover_dialog, GTK_WIDGET (ok));

  gtk_popover_popup (popover_dialog);

  gtk_widget_grab_focus (entry_text != nullptr ? GTK_WIDGET (entry) : GTK_WIDGET (cancel));
}

/* "New" selected, ask for profile name */
static void
profile_new_cb (GtkButton *button,
                gpointer   user_data)
{
  profile_popup_dialog (GTK_WIDGET (the_pref_data->new_profile_button),
                        _("New Profile"),
                        _("Enter name for new profile with default settings:"),
                        "",
                        _("Create"),
                        profile_new_now);
}

/* "Clone" selected, ask for profile name */
static void
profile_clone_cb (GSimpleAction *simple,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  gs_free char *name = g_settings_get_string (the_pref_data->selected_profile, TERMINAL_PROFILE_VISIBLE_NAME_KEY);

  gs_free char *label = g_strdup_printf (_("Enter name for new profile based on “%s”:"), name);
  gs_free char *clone_name = g_strdup_printf (_("%s (Copy)"), name);

  profile_popup_dialog (GTK_WIDGET (the_pref_data->selected_list_box_row),
                        _("Clone Profile"),
                        label,
                        clone_name,
                        _("Clone"),
                        profile_clone_now);
}

/* "Rename" selected, ask for new name */
static void
profile_rename_cb (GSimpleAction *simple,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  if (the_pref_data->selected_profile == nullptr)
    return;

  gs_free char *name = g_settings_get_string (the_pref_data->selected_profile, TERMINAL_PROFILE_VISIBLE_NAME_KEY);

  gs_free char *label = g_strdup_printf (_("Enter new name for profile “%s”:"), name);

  profile_popup_dialog (GTK_WIDGET (the_pref_data->selected_list_box_row),
                        _("Rename Profile"),
                        label,
                        name,
                        _("Rename"),
                        profile_rename_now);
}

/* "Delete" selected, ask for confirmation */
static void
profile_delete_cb (GSimpleAction *simple,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  if (the_pref_data->selected_profile == nullptr)
    return;

  gs_free char *name = g_settings_get_string (the_pref_data->selected_profile, TERMINAL_PROFILE_VISIBLE_NAME_KEY);

  gs_free char *label = g_strdup_printf (_("Really delete profile “%s”?"), name);

  profile_popup_dialog (GTK_WIDGET (the_pref_data->selected_list_box_row),
                        _("Delete Profile"),
                        label,
                        nullptr,
                        _("Delete"),
                        profile_delete_now);
}

/* Create a (non-header) row of the sidebar, either a global or a profile entry. */
static GtkListBoxRow *
listbox_create_row (const char *name,
                    const char *stack_child_name,
                    const char *uuid,
                    GSettings  *gsettings /* adopted */,
                    gpointer    sort_order)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (gtk_list_box_row_new ());

  g_object_set_data_full (G_OBJECT (row), "stack_child_name", g_strdup (stack_child_name), g_free);
  g_object_set_data_full (G_OBJECT (row), "uuid", g_strdup (uuid), g_free);
  if (gsettings != nullptr)
    g_object_set_data_full (G_OBJECT (row), "gsettings", gsettings, (GDestroyNotify)g_object_unref);
  g_object_set_data (G_OBJECT (row), "sort_order", sort_order);

  GtkBox *hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_margin_start (GTK_WIDGET (hbox), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (hbox), 6);
  gtk_widget_set_margin_top (GTK_WIDGET (hbox), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (hbox), 6);

  GtkLabel *label = GTK_LABEL (gtk_label_new (name));
  if (gsettings != nullptr) {
    g_signal_connect (label, "notify::label", G_CALLBACK (profile_name_changed_cb), row);
    g_settings_bind (gsettings,
                     TERMINAL_PROFILE_VISIBLE_NAME_KEY,
                     label,
                     "label",
                     G_SETTINGS_BIND_GET);
  }
  gtk_label_set_xalign (label, 0);
  /* Bound the sidebar width regardless of profile name length (axan#406).
   * The name field accepts arbitrary input; without ellipsize, a paragraph-length
   * name would widen the listbox and break the prefs dialog layout. */
  gtk_label_set_ellipsize (label, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (label, 22);
  gtk_box_pack_start (hbox, GTK_WIDGET (label), TRUE, TRUE, 0);
  g_object_set_data (G_OBJECT (row), "label", label);

  /* Always add the "default" symbol and the "menu" button, even on rows of global prefs.
   * Use GtkStack to possible achieve visibility:hidden on it.
   * This is so that all listbox rows have the same dimensions, and the width doesn't change
   * as you switch the default profile. */

  GtkStack *popover_stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_margin_start (GTK_WIDGET (popover_stack), 6);
  GtkMenuButton *popover_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  gtk_button_set_relief (GTK_BUTTON (popover_button), GTK_RELIEF_NONE);
  gtk_stack_add_named (popover_stack, GTK_WIDGET (popover_button), "button");
  GtkLabel *popover_label = GTK_LABEL (gtk_label_new (""));
  gtk_stack_add_named (popover_stack, GTK_WIDGET (popover_label), "placeholder");
  g_object_set_data (G_OBJECT (row), "popover-stack", popover_stack);
  g_object_set_data (G_OBJECT (row), "popover-button", popover_button);

  gtk_box_pack_end (hbox, GTK_WIDGET (popover_stack), FALSE, FALSE, 0);

  GtkStack *home_stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_margin_start (GTK_WIDGET (home_stack), 12);
  GtkImage *home_image = GTK_IMAGE (gtk_image_new_from_icon_name ("emblem-default-symbolic", GTK_ICON_SIZE_BUTTON));
  gtk_widget_set_tooltip_text (GTK_WIDGET (home_image), _("This is the default profile"));
  gtk_stack_add_named (home_stack, GTK_WIDGET (home_image), "home");
  GtkLabel *home_label = GTK_LABEL (gtk_label_new (""));
  gtk_stack_add_named (home_stack, GTK_WIDGET (home_label), "placeholder");
  g_object_set_data (G_OBJECT (row), "home-stack", home_stack);

  gtk_box_pack_end (hbox, GTK_WIDGET (home_stack), FALSE, FALSE, 0);

  gtk_container_add (GTK_CONTAINER (row), GTK_WIDGET (hbox));

  gtk_widget_show_all (GTK_WIDGET (row));

  gtk_stack_set_visible_child_name (popover_stack, "placeholder");
  gtk_stack_set_visible_child_name (home_stack, "placeholder");

  return row;
}

/* Add all the non-profile rows to the sidebar */
static void
listbox_add_all_globals (PrefData *data)
{
  GtkListBoxRow *row;

  row = listbox_create_row (_("General"),
                            "general-prefs",
                            nullptr, nullptr, (gpointer) 0);
  gtk_list_box_insert (data->listbox, GTK_WIDGET (row), -1);

  row = listbox_create_row (_("Shortcuts"),
                            "shortcut-prefs",
                            nullptr, nullptr, (gpointer) 1);
  gtk_list_box_insert (data->listbox, GTK_WIDGET (row), -1);
}

/* Remove all the profile rows from the sidebar */
static void
listbox_remove_all_profiles (PrefData *data)
{
  int i = 0;

  data->selected_profile = nullptr;
  g_free (data->selected_profile_uuid);
  data->selected_profile_uuid = nullptr;
  profile_prefs_unload ();

  GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (the_pref_data->listbox), 0);
  g_signal_emit_by_name (row, "activate");

  while ((row = gtk_list_box_get_row_at_index (data->listbox, i)) != nullptr) {
    if (g_object_get_data (G_OBJECT (row), "gsettings") != nullptr) {
      gtk_widget_destroy (GTK_WIDGET (row));
    } else {
      i++;
    }
  }
}

/* Add all the profiles to the sidebar */
static void
listbox_add_all_profiles (PrefData *data)
{
  GList *list, *l;
  GtkListBoxRow *row;

  list = terminal_settings_list_ref_children (data->profiles_list);

  for (l = list; l != nullptr; l = l->next) {
    GSettings *profile = (GSettings *) l->data;
    gs_free gchar *uuid = terminal_settings_list_dup_uuid_from_child (data->profiles_list, profile);

    row = listbox_create_row (nullptr,
                              "profile-prefs",
                              uuid,
                              profile /* adopts */,
                              (gpointer) 42);
    gtk_list_box_insert (data->listbox, GTK_WIDGET (row), -1);
  }

  g_list_free(list); /* the items themselves were adopted into the model above */

  listbox_update (data->listbox);  /* FIXME: This is not needed but I don't know why :-) */
}

/* Re-add all the profiles to the sidebar.
 * This is called when a profile is added or removed, and also when the list of profiles is
 * modified externally.
 * Try to keep the selected profile, whenever possible.
 * When the list is modified externally, the terminal_settings_list_*() methods seem to preserve
 * the GSettings object for every profile that remains in the list. There's no guarantee however
 * that a newly created GSettings can't receive the same address that a ceased one used to have.
 * So don't rely on GSettings* to keep track of the selected profile, use the UUID instead. */
static void
listbox_readd_profiles (PrefData *data)
{
  gs_free char *uuid = g_strdup (data->selected_profile_uuid);

  listbox_remove_all_profiles (data);
  listbox_add_all_profiles (data);

  if (uuid != nullptr)
    listbox_select_profile (uuid);
}

/* Create a header row ("Global" or "Profiles +") */
static GtkWidget *
listboxrow_create_header (const char *text,
                          gboolean visible_button)
{
  GtkBox *hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_margin_start (GTK_WIDGET (hbox), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (hbox), 6);
  gtk_widget_set_margin_top (GTK_WIDGET (hbox), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (hbox), 6);

  GtkLabel *label = GTK_LABEL (gtk_label_new (nullptr));
  gs_free char *markup = g_markup_printf_escaped ("<b>%s</b>", text);
  gtk_label_set_markup (label, markup);
  gtk_label_set_xalign (label, 0);
  gtk_box_pack_start (hbox, GTK_WIDGET (label), TRUE, TRUE, 0);

  /* Always add a "new profile" button. Use GtkStack to possible achieve visibility:hidden on it.
   * This is so that both header rows have the same dimensions. */

  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  GtkButton *button = GTK_BUTTON (gtk_button_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
  gtk_button_set_relief (button, GTK_RELIEF_NONE);
  gtk_stack_add_named (stack, GTK_WIDGET (button), "button");
  GtkLabel *labelx = GTK_LABEL (gtk_label_new (""));
  gtk_stack_add_named (stack, GTK_WIDGET (labelx), "placeholder");

  gtk_box_pack_end (hbox, GTK_WIDGET (stack), FALSE, FALSE, 0);

  gtk_widget_show_all (GTK_WIDGET (hbox));

  if (visible_button) {
    gtk_stack_set_visible_child_name (stack, "button");
    g_signal_connect (button, "clicked", G_CALLBACK (profile_new_cb), nullptr);
    the_pref_data->new_profile_button = GTK_WIDGET (button);
  } else {
    gtk_stack_set_visible_child_name (stack, "placeholder");
  }

  return GTK_WIDGET (hbox);
}

/* Manage the creation or removal of the header row ("Global" or "Profiles +") */
static void
listboxrow_update_header (GtkListBoxRow *row,
                          GtkListBoxRow *before,
                          gpointer       user_data)
{
  if (before == nullptr) {
    if (gtk_list_box_row_get_header (row) == nullptr) {
      gtk_list_box_row_set_header (row, listboxrow_create_header (_("Global"), FALSE));
    }
    return;
  }

  GSettings *profile = (GSettings*)g_object_get_data (G_OBJECT (row), "gsettings");
  if (profile != nullptr) {
    GSettings *profile_before = (GSettings*)g_object_get_data (G_OBJECT (before), "gsettings");
    if (profile_before != nullptr) {
      gtk_list_box_row_set_header (row, nullptr);
    } else {
      if (gtk_list_box_row_get_header (row) == nullptr) {
        gtk_list_box_row_set_header (row, listboxrow_create_header (_("Profiles"), TRUE));
      }
    }
  }
}

/* Sort callback for rows of the sidebar (global and profile ones).
 * Global ones are kept at the top in fixed order. This is implemented via sort_order
 * which is an integer disguised as a pointer for ease of implementation.
 * Profile ones are sorted lexicographically. */
static gint
listboxrow_compare_cb (GtkListBoxRow *row1,
                       GtkListBoxRow *row2,
                       gpointer       user_data)
{
  gpointer sort_order_1 = g_object_get_data (G_OBJECT (row1), "sort_order");
  gpointer sort_order_2 = g_object_get_data (G_OBJECT (row2), "sort_order");

  if (sort_order_1 != sort_order_2)
    return sort_order_1 < sort_order_2 ? -1 : 1;

  GtkLabel *label1 = (GtkLabel*)g_object_get_data (G_OBJECT (row1), "label");
  const char *text1 = gtk_label_get_text (label1);
  GtkLabel *label2 = (GtkLabel*)g_object_get_data (G_OBJECT (row2), "label");
  const char *text2 = gtk_label_get_text (label2);

  return g_utf8_collate (text1, text2);
}

/* Keybindings tab */

/* Make sure the treeview is repainted with the correct text color, see bug 792139. */
static void
shortcuts_button_toggled_cb (GtkWidget *widget,
                             GtkTreeView *tree_view)
{
  gtk_widget_queue_draw (GTK_WIDGET (tree_view));
}

/* misc */

static void
prefs_dialog_destroy_cb (GtkWidget *widget,
                         PrefData *data)
{
  /* Don't run this handler again */
  g_signal_handlers_disconnect_by_func (widget, (void*)prefs_dialog_destroy_cb, data);

  g_signal_handlers_disconnect_by_func (data->profiles_list,
                                        (void*)listbox_readd_profiles, data);
  g_signal_handlers_disconnect_by_func (data->profiles_list,
                                        (void*)listbox_update, data->listbox);

  profile_prefs_destroy ();

  g_object_unref (data->builder);
  g_free (data->selected_profile_uuid);
  g_free (data);
}

static void
make_default_button_clicked_cb(GtkWidget* button,
                               PrefData* data)
{
  terminal_app_make_default_terminal(terminal_app_get());
}

void
terminal_prefs_show_preferences(GSettings* profile,
                                char const* widget_name,
                                unsigned timestamp)
{
  TerminalApp *app = terminal_app_get ();
  PrefData *data;
  GtkWidget *dialog, *tree_view;
  GtkWidget *show_menubar_button, *disable_mnemonics_button, *disable_menu_accel_button;
  GtkWidget *disable_shortcuts_button;
  GtkWidget *theme_variant_label, *theme_variant_combo;
  GtkWidget *new_terminal_mode_label, *new_terminal_mode_combo;
  GtkWidget *new_tab_position_combo;
  GtkWidget *startup_sidebar_state_combo, *collapsed_size_combo;
  GtkWidget *auto_assign_icon_button, *show_icons_in_expanded_button;
  GtkWidget *recolor_icons_button;
  GtkWidget *close_button, *help_button;
  GtkWidget *content_box, *general_frame, *keybindings_frame;
  GtkWidget *always_check_default_button, *make_default_button;
  GSettings *settings;

  const GActionEntry action_entries[] = {
    { "clone",           profile_clone_cb,          nullptr, nullptr, nullptr },
    { "rename",          profile_rename_cb,         nullptr, nullptr, nullptr },
    { "delete",          profile_delete_cb,         nullptr, nullptr, nullptr },
    { "set-as-default",  profile_set_as_default_cb, nullptr, nullptr, nullptr },
    { "import-all",      import_all_cb,             nullptr, nullptr, nullptr },
    { "import-icons",    import_icons_cb,           nullptr, nullptr, nullptr },
  };

  if (the_pref_data != nullptr)
    goto done;

  {
  the_pref_data = g_new0 (PrefData, 1);
  data = the_pref_data;
  data->profiles_list = terminal_app_get_profiles_list (app);

  /* FIXME this method is only used from here. Inline it here instead. */
  data->builder = terminal_util_load_widgets_resource ("/org/gnome/terminal/ui/preferences.ui",
                                       "preferences-dialog",
                                       "preferences-dialog", &dialog,
                                       "dialogue-content-box", &content_box,
                                       "general-frame", &general_frame,
                                       "keybindings-frame", &keybindings_frame,
                                       "close-button", &close_button,
                                       "help-button", &help_button,
                                       "default-show-menubar-checkbutton", &show_menubar_button,
                                       "theme-variant-label", &theme_variant_label,
                                       "theme-variant-combobox", &theme_variant_combo,
                                       "new-terminal-mode-label", &new_terminal_mode_label,
                                       "new-terminal-mode-combobox", &new_terminal_mode_combo,
                                       "disable-mnemonics-checkbutton", &disable_mnemonics_button,
                                       "disable-shortcuts-checkbutton", &disable_shortcuts_button,
                                       "disable-menu-accel-checkbutton", &disable_menu_accel_button,
                                       "new-tab-position-combobox", &new_tab_position_combo,
                                       "startup-sidebar-state-combobox", &startup_sidebar_state_combo,
                                       "collapsed-size-combobox", &collapsed_size_combo,
                                       "auto-assign-icon-checkbutton", &auto_assign_icon_button,
                                       "show-icons-in-expanded-checkbutton", &show_icons_in_expanded_button,
                                       "recolor-icons-checkbutton", &recolor_icons_button,
                                       "always-check-default-checkbutton", &always_check_default_button,
                                       "make-default-button", &make_default_button,
                                       "accelerators-treeview", &tree_view,
                                       "the-stack", &data->stack,
                                       "the-listbox", &data->listbox,
                                       nullptr);

  data->dialog = dialog;

  gtk_window_set_application (GTK_WINDOW (data->dialog), GTK_APPLICATION (terminal_app_get ()));

  terminal_util_bind_mnemonic_label_sensitivity (dialog);

  settings = terminal_app_get_global_settings (app);

  g_action_map_add_action_entries (G_ACTION_MAP (dialog),
                                   action_entries, G_N_ELEMENTS (action_entries),
                                   data);

  /* Sidebar */

  gtk_list_box_set_header_func (GTK_LIST_BOX (data->listbox),
                                listboxrow_update_header,
                                nullptr,
                                nullptr);
  g_signal_connect (data->listbox, "row-selected", G_CALLBACK (listbox_row_selected_cb), data->stack);
  gtk_list_box_set_sort_func (data->listbox, listboxrow_compare_cb, nullptr, nullptr);

  listbox_add_all_globals (data);
  listbox_add_all_profiles (data);
  g_signal_connect_swapped (data->profiles_list, "children-changed",
                            G_CALLBACK (listbox_readd_profiles), data);
  g_signal_connect_swapped (data->profiles_list, "default-changed",
                            G_CALLBACK (listbox_update), data->listbox);

  GtkEntry *entry = GTK_ENTRY (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-entry"));
  GtkButton *ok = GTK_BUTTON (gtk_builder_get_object (the_pref_data->builder, "popover-dialog-ok"));
  g_signal_connect (entry, "notify::text", G_CALLBACK (popover_dialog_notify_text_cb), ok);

  GtkButton *icons_help_btn =
    GTK_BUTTON (gtk_builder_get_object (the_pref_data->builder, "icons-help-button"));
  if (icons_help_btn != nullptr)
    g_signal_connect (icons_help_btn, "clicked",
                      G_CALLBACK (icons_help_clicked_cb), nullptr);

  /* General page */

  gboolean shell_shows_menubar;
  g_object_get (gtk_settings_get_default (),
                "gtk-shell-shows-menubar", &shell_shows_menubar,
                nullptr);
  if (shell_shows_menubar || terminal_app_get_use_headerbar (app)) {
    gtk_widget_set_visible (show_menubar_button, FALSE);
  } else {
    g_settings_bind (settings,
                     TERMINAL_SETTING_DEFAULT_SHOW_MENUBAR_KEY,
                     show_menubar_button,
                     "active",
                     GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  }

  g_settings_bind (settings,
                   TERMINAL_SETTING_THEME_VARIANT_KEY,
                   theme_variant_combo,
                   "active-id",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  if (terminal_app_get_menu_unified (app) ||
      terminal_app_get_use_headerbar (app)) {
    g_settings_bind (settings,
                     TERMINAL_SETTING_NEW_TERMINAL_MODE_KEY,
                     new_terminal_mode_combo,
                     "active-id",
                     GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  } else {
    gtk_widget_set_visible (new_terminal_mode_label, FALSE);
    gtk_widget_set_visible (new_terminal_mode_combo, FALSE);
  }

  g_settings_bind (settings,
                   TERMINAL_SETTING_NEW_TAB_POSITION_KEY,
                   new_tab_position_combo,
                   "active-id",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_settings_bind (settings,
                   TERMINAL_SETTING_STARTUP_SIDEBAR_STATE_KEY,
                   startup_sidebar_state_combo,
                   "active-id",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_settings_bind (settings,
                   TERMINAL_SETTING_COLLAPSED_SIZE_KEY,
                   collapsed_size_combo,
                   "active-id",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_settings_bind (settings,
                   TERMINAL_SETTING_AUTO_ASSIGN_ICON_KEY,
                   auto_assign_icon_button,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_settings_bind (settings,
                   TERMINAL_SETTING_SHOW_ICONS_IN_EXPANDED_KEY,
                   show_icons_in_expanded_button,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_settings_bind (settings,
                   TERMINAL_SETTING_RECOLOR_ICONS_KEY,
                   recolor_icons_button,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  if (shell_shows_menubar) {
    gtk_widget_set_visible (disable_mnemonics_button, FALSE);
  } else {
    g_settings_bind (settings,
                     TERMINAL_SETTING_ENABLE_MNEMONICS_KEY,
                     disable_mnemonics_button,
                     "active",
                     GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  }
  g_settings_bind (settings,
                   TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY,
                   disable_menu_accel_button,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_settings_bind(settings,
                  TERMINAL_SETTING_ALWAYS_CHECK_DEFAULT_KEY,
                  always_check_default_button,
                  "active",
                  GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_signal_connect(make_default_button, "clicked",
                   G_CALLBACK(make_default_button_clicked_cb), data);

  g_object_bind_property(app, "is-default-terminal",
                         make_default_button, "sensitive",
                         GBindingFlags(G_BINDING_DEFAULT |
                                       G_BINDING_SYNC_CREATE |
                                       G_BINDING_INVERT_BOOLEAN));

  /* Shortcuts page */

  g_settings_bind (settings,
                   TERMINAL_SETTING_ENABLE_SHORTCUTS_KEY,
                   disable_shortcuts_button,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_signal_connect (disable_shortcuts_button, "toggled",
                    G_CALLBACK (shortcuts_button_toggled_cb), tree_view);

  terminal_accels_fill_treeview (tree_view, disable_shortcuts_button);

  /* Profile page */

  profile_prefs_init ();

  /* Move action widgets to titlebar when headerbar is used */
  if (terminal_app_get_dialog_use_headerbar (app)) {
    GtkWidget *headerbar;
    GtkWidget *bbox;

    headerbar = (GtkWidget*)g_object_new (GTK_TYPE_HEADER_BAR,
					  "show-close-button", TRUE,
					  nullptr);
    bbox = gtk_widget_get_parent (help_button);

    gtk_container_remove (GTK_CONTAINER (bbox), (GtkWidget*)g_object_ref (help_button));
    gtk_header_bar_pack_start (GTK_HEADER_BAR (headerbar), help_button);
    g_object_unref (help_button);

    gtk_style_context_add_class (gtk_widget_get_style_context (help_button),
                                 "text-button");

    gtk_widget_show (headerbar);
    gtk_widget_hide (bbox);

    gtk_window_set_titlebar (GTK_WINDOW (dialog), headerbar);

    /* Remove extra spacing around the content, and extra frames */
    g_object_set (G_OBJECT (content_box), "margin", 0, nullptr);
    gtk_frame_set_shadow_type (GTK_FRAME (general_frame), GTK_SHADOW_NONE);
    gtk_frame_set_shadow_type (GTK_FRAME (keybindings_frame), GTK_SHADOW_NONE);
  }

  /* misc */

  g_signal_connect (close_button, "clicked", G_CALLBACK (prefs_dialog_close_button_clicked_cb), data);
  g_signal_connect (help_button, "clicked", G_CALLBACK (prefs_dialog_help_button_clicked_cb), data);
  g_signal_connect (dialog, "destroy", G_CALLBACK (prefs_dialog_destroy_cb), data);

  g_object_add_weak_pointer (G_OBJECT (dialog), (gpointer *) &the_pref_data);
  }

done:
  if (profile != nullptr) {
    gs_free char *uuid = terminal_settings_list_dup_uuid_from_child (the_pref_data->profiles_list, profile);
    listbox_select_profile (uuid);
  } else {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (the_pref_data->listbox), 0);
    g_signal_emit_by_name (row, "activate");
  }

  terminal_util_dialog_focus_widget (the_pref_data->builder, widget_name);

  gtk_window_present_with_time(GTK_WINDOW(the_pref_data->dialog), timestamp);
}

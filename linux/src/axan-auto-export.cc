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
 * Auto-export TOML mirrors — see axan-auto-export.hh.
 *
 * State is file-static: one TerminalApp lives per process and the watchers
 * survive for the lifetime of that app. The shutdown path doesn't bother
 * tearing handlers down — they go away with the GObjects they're attached to.
 */

#include "config.h"
#include "axan-auto-export.hh"

#include "axan-log.h"
#include "axan-toml.hh"
#include "keybindings-toml.hh"
#include "profile-toml.hh"
#include "settings-toml.hh"
#include "terminal-app.hh"
#include "terminal-debug.hh"
#include "terminal-libgsystem.hh"
#include "terminal-profiles-list.hh"
#include "terminal-schemas.hh"
#include "terminal-settings-utils.hh"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

namespace {

constexpr guint kDebounceMs = 500;

struct AutoExportState {
  TerminalApp *app;             /* not owned */
  GSettings *keybindings;       /* owned */
  guint pending_id;             /* 0 if no timer scheduled */
};

AutoExportState g_state = { nullptr, nullptr, 0 };

void schedule_resync ();
void resync_profile_watchers ();

void
delete_orphan_profile_tomls (GHashTable *live_uuids)
{
  gs_free char *dir =
    g_build_filename (g_get_user_config_dir (), "axan", "profiles", nullptr);
  gs_unref_object GFile *gdir = g_file_new_for_path (dir);
  gs_unref_object GFileEnumerator *en =
    g_file_enumerate_children (gdir,
                               G_FILE_ATTRIBUTE_STANDARD_NAME,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               nullptr, nullptr);
  if (en == nullptr)
    return; /* dir not present yet — nothing to clean */

  for (;;) {
    GFileInfo *info = nullptr;
    if (!g_file_enumerator_iterate (en, &info, nullptr, nullptr, nullptr) ||
        info == nullptr)
      break;
    const char *name = g_file_info_get_name (info);
    if (!g_str_has_suffix (name, ".toml"))
      continue;
    gs_free char *uuid = g_strndup (name, strlen (name) - 5);
    if (g_hash_table_contains (live_uuids, uuid))
      continue;
    gs_free char *full = g_build_filename (dir, name, nullptr);
    if (g_unlink (full) != 0) {
      axan_log_warn ("auto_export", "could not remove orphan %s", full);
    }
  }
}

gboolean
fire_resync (gpointer /*unused*/)
{
  g_state.pending_id = 0;
  _terminal_debug_print (TERMINAL_DEBUG_SERVER, "auto-export: resync\n");

  gboolean prev_quiet = axan_toml_set_quiet (TRUE);
  gs_free_error GError *error = nullptr;

  if (!axan_settings_toml_export (nullptr, &error)) {
    axan_log_warn ("auto_export", "settings.toml export failed: %s",
                   error ? error->message : "?");
    g_clear_error (&error);
  }

  if (!axan_keybindings_toml_export (nullptr, &error)) {
    axan_log_warn ("auto_export", "keybindings.toml export failed: %s",
                   error ? error->message : "?");
    g_clear_error (&error);
  }

  TerminalSettingsList *list = terminal_app_get_profiles_list (g_state.app);
  gs_strfreev char **uuids = terminal_settings_list_dupv_children (list);
  GHashTable *live = g_hash_table_new (g_str_hash, g_str_equal);
  if (uuids != nullptr) {
    for (guint i = 0; uuids[i] != nullptr; i++) {
      if (!axan_profile_toml_export (uuids[i], nullptr, &error)) {
        axan_log_warn ("auto_export", "profile %s export failed: %s",
                       uuids[i], error ? error->message : "?");
        g_clear_error (&error);
      }
      g_hash_table_add (live, uuids[i]);
    }
  }
  delete_orphan_profile_tomls (live);
  g_hash_table_destroy (live);

  axan_toml_set_quiet (prev_quiet);
  return G_SOURCE_REMOVE;
}

void
schedule_resync ()
{
  if (g_state.pending_id != 0)
    return;
  g_state.pending_id = g_timeout_add (kDebounceMs, fire_resync, nullptr);
}

void
on_any_changed (GSettings * /*settings*/, const char * /*key*/, gpointer /*user_data*/)
{
  schedule_resync ();
}

void
on_profile_children_changed (TerminalSettingsList * /*list*/,
                             const char * /*uuid*/,
                             gpointer /*user_data*/)
{
  resync_profile_watchers ();
  schedule_resync ();
}

void
resync_profile_watchers ()
{
  TerminalSettingsList *list = terminal_app_get_profiles_list (g_state.app);
  gs_strfreev char **uuids = terminal_settings_list_dupv_children (list);
  if (uuids == nullptr)
    return;

  for (guint i = 0; uuids[i] != nullptr; i++) {
    gs_unref_object GSettings *child =
      terminal_settings_list_ref_child (list, uuids[i]);
    if (child == nullptr)
      continue;
    /* Idempotent: disconnect any prior handler for this callback, then attach.
     * Profiles created since last call get fresh handlers; profiles that
     * already had one effectively re-attach (no leak, no duplicate). */
    g_signal_handlers_disconnect_by_func (child, (void*)on_any_changed, nullptr);
    g_signal_connect (child, "changed",
                      G_CALLBACK (on_any_changed), nullptr);
  }
}

} // namespace

void
axan_auto_export_init (TerminalApp *app)
{
  g_return_if_fail (app != nullptr);
  g_return_if_fail (g_state.app == nullptr);
  g_state.app = app;

  GSettings *global = terminal_app_get_global_settings (app);
  g_signal_connect (global, "changed",
                    G_CALLBACK (on_any_changed), nullptr);

  /* Keybindings is a relocatable schema; we hold our own ref for the
   * lifetime of the process. */
  g_state.keybindings =
    g_settings_new_with_path (TERMINAL_KEYBINDINGS_SCHEMA,
                              TERMINAL_KEYBINDINGS_SCHEMA_PATH);
  g_signal_connect (g_state.keybindings, "changed",
                    G_CALLBACK (on_any_changed), nullptr);

  TerminalSettingsList *list = terminal_app_get_profiles_list (app);
  g_signal_connect (list, "changed",
                    G_CALLBACK (on_any_changed), nullptr);
  g_signal_connect (list, "children-changed",
                    G_CALLBACK (on_profile_children_changed), nullptr);

  resync_profile_watchers ();

  /* First-run bootstrap: if no TOMLs exist yet, force one resync so users
   * see a populated ~/.config/axan/ on initial install without waiting for
   * a settings change. */
  gs_free char *settings_path = axan_settings_toml_default_path ();
  if (!g_file_test (settings_path, G_FILE_TEST_EXISTS))
    schedule_resync ();
}

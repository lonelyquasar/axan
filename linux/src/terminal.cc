/*
 * Copyright © 2001, 2002 Havoc Pennington
 * Copyright © 2002 Red Hat, Inc.
 * Copyright © 2002 Sun Microsystems
 * Copyright © 2003 Mariano Suarez-Alvarez
 * Copyright © 2008, 2010, 2011 Christian Persch
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

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include <gtk/gtk.h>

#include "axan-log.h"
#include "axan-icons-toml.hh"
#include "keybindings-toml.hh"
#include "profile-toml.hh"
#include "settings-toml.hh"
#include "terminal-debug.hh"
#include "terminal-defines.hh"
#include "terminal-schemas.hh"
#include "terminal-i18n.hh"
#include "terminal-options.hh"
#include "terminal-gdbus-generated.h"
#include "terminal-defines.hh"
#include "terminal-client-utils.hh"
#include "terminal-libgsystem.hh"

GS_DEFINE_CLEANUP_FUNCTION0(TerminalOptions*, gs_local_options_free, terminal_options_free)
#define gs_free_options __attribute__ ((cleanup(gs_local_options_free)))

/* Wait-for-exit helper */

typedef struct {
  GMainLoop *loop;
  int status;
} RunData;

static void
receiver_child_exited_cb (TerminalReceiver *receiver,
                          int status,
                          RunData *data)
{
  data->status = status;

  if (g_main_loop_is_running (data->loop))
    g_main_loop_quit (data->loop);
}

static void
factory_name_owner_notify_cb (TerminalFactory *factory,
                              GParamSpec *pspec,
                              RunData *data)
{
  /* Name owner change to nullptr can only mean that the server
   * went away before it could send out our child-exited signal.
   * Assume the server was killed and thus our child process
   * too, and return with the corresponding exit code.
   */
  if (g_dbus_proxy_get_name_owner(G_DBUS_PROXY (factory)) != nullptr)
    return;

  data->status = W_EXITCODE(0, SIGKILL);

  if (g_main_loop_is_running (data->loop))
    g_main_loop_quit (data->loop);
}

static int
run_receiver (TerminalFactory *factory,
              TerminalReceiver *receiver)
{
  RunData data = { g_main_loop_new (nullptr, FALSE), 0 };
  gulong receiver_exited_id = g_signal_connect (receiver, "child-exited",
                                                G_CALLBACK (receiver_child_exited_cb), &data);
  gulong factory_notify_id = g_signal_connect (factory, "notify::g-name-owner",
                                               G_CALLBACK (factory_name_owner_notify_cb), &data);
  g_main_loop_run (data.loop);
  g_signal_handler_disconnect (receiver, receiver_exited_id);
  g_signal_handler_disconnect (factory, factory_notify_id);
  g_main_loop_unref (data.loop);

  /* Mangle the exit status */
  int exit_code;
  if (WIFEXITED (data.status))
    exit_code = WEXITSTATUS (data.status);
  else if (WIFSIGNALED (data.status))
    exit_code = 128 + (int) WTERMSIG (data.status);
  else if (WCOREDUMP (data.status))
    exit_code = 127;
  else
    exit_code = 127;

  return exit_code;
}

/* Factory helpers */

static gboolean
get_factory_exit_status (const char *service_name,
                         const char *message,
                         int *exit_status)
{
  gs_free char *pattern = nullptr, *number = nullptr;
  gs_unref_regex GRegex *regex = nullptr;
  gs_free_match_info GMatchInfo *match_info = nullptr;
  gint64 v;
  char *end;
  GError *err = nullptr;

  pattern = g_strdup_printf ("org.freedesktop.DBus.Error.Spawn.ChildExited: Process %s exited with status (\\d+)$",
                             service_name);
  regex = g_regex_new (pattern, GRegexCompileFlags(0), GRegexMatchFlags(0), &err);
  g_assert_no_error (err);

  if (!g_regex_match (regex, message, GRegexMatchFlags(0), &match_info))
    return FALSE;

  number = g_match_info_fetch (match_info, 1);
  g_assert_nonnull (number);

  errno = 0;
  v = g_ascii_strtoll (number, &end, 10);
  if (errno || end == number || *end != '\0' || v < 0 || v > G_MAXINT)
    return FALSE;

  *exit_status = (int)v;
  return TRUE;
}

static gboolean
handle_factory_error (const char *service_name,
                      GError *error)
{
  int exit_status;

  if (!g_dbus_error_is_remote_error (error) ||
      !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_CHILD_EXITED) ||
      !get_factory_exit_status (service_name, error->message, &exit_status))
    return FALSE;

  g_dbus_error_strip_remote_error (error);
  terminal_printerr ("%s\n\n", error->message);

  switch (exit_status) {
  case _EXIT_FAILURE_WRONG_ID:
    terminal_printerr ("You tried to run axan-server with elevated privileged. This is not supported.\n");
    break;
  case _EXIT_FAILURE_NO_UTF8:
    terminal_printerr ("The environment that axan-server was launched with specified a non-UTF-8 locale. This is not supported.\n");
    break;
  case _EXIT_FAILURE_UNSUPPORTED_LOCALE:
    terminal_printerr ("The environment that axan-server was launched with specified an unsupported locale.\n");
    break;
  case _EXIT_FAILURE_GTK_INIT:
    terminal_printerr ("The environment that axan-server was launched with most likely contained an incorrect or unset \"DISPLAY\" variable.\n");
    break;
  default:
    break;
  }
  terminal_printerr ("See https://wiki.gnome.org/Apps/Terminal/FAQ#Exit_status_%d for more information.\n", exit_status);

  return TRUE;
}

static gboolean
handle_create_instance_error (const char *service_name,
                              GError *error)
{
  if (handle_factory_error (service_name, error))
    return TRUE;

  g_dbus_error_strip_remote_error (error);
  terminal_printerr ("Error creating terminal: %s\n", error->message);
  return FALSE; /* don't abort */
}

static gboolean
handle_create_receiver_proxy_error (const char *service_name,
                                    GError *error)
{
  if (handle_factory_error (service_name, error))
    return TRUE;

  g_dbus_error_strip_remote_error (error);
  terminal_printerr ("Failed to create proxy for terminal: %s\n", error->message);
  return FALSE; /* don't abort */
}

static gboolean
handle_exec_error (const char *service_name,
                   GError *error)
{
  if (handle_factory_error (service_name, error))
    return TRUE;

  g_dbus_error_strip_remote_error (error);
  terminal_printerr ("Error: %s\n", error->message);
  return FALSE; /* don't abort */
}

static gboolean
factory_proxy_new_for_service_name (const char *service_name,
                                    gboolean ping_server,
                                    gboolean connect_signals,
                                    TerminalFactory **factory_ptr,
                                    char **service_name_ptr,
                                    GError **error)
{
  if (service_name == nullptr)
    service_name = TERMINAL_APPLICATION_ID;

  gs_free_error GError *err = nullptr;
  gs_unref_object TerminalFactory *factory =
    terminal_factory_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                             GDBusProxyFlags(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                             connect_signals ? 0 : G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
                                             service_name,
                                             TERMINAL_FACTORY_OBJECT_PATH,
                                             nullptr /* cancellable */,
                                             &err);
  if (factory == nullptr) {
    if (!handle_factory_error (service_name, err))
      terminal_printerr ("Error constructing proxy for %s:%s: %s\n",
                         service_name, TERMINAL_FACTORY_OBJECT_PATH, err->message);
    g_propagate_error (error, err);
    err = nullptr;
    return FALSE;
  }

  if (ping_server) {
    /* If we try to use the environment specified server, we need to make
     * sure it actually exists so we can later fall back to the default name.
     * There doesn't appear to a way to fail proxy creation above if the
     * unique name doesn't exist; so we do it this way.
     */
    gs_unref_variant GVariant *v = g_dbus_proxy_call_sync (G_DBUS_PROXY (factory),
                                                           "org.freedesktop.DBus.Peer.Ping",
                                                           g_variant_new ("()"),
                                                           G_DBUS_CALL_FLAGS_NONE,
                                                           1000 /* 1s */,
                                                           nullptr /* cancelleable */,
                                                           &err);
    if (v == nullptr) {
      g_propagate_error (error, err);
      err = nullptr;
      return FALSE;
    }
  }

  gs_transfer_out_value (factory_ptr, &factory);
  *service_name_ptr = g_strdup (service_name);
  return TRUE;
}

static gboolean
factory_proxy_new (TerminalOptions *options,
                   TerminalFactory **factory_ptr,
                   char **service_name_ptr,
                   char **parent_screen_object_path_ptr,
                   GError **error)
{
  const char *service_name = options->server_app_id;

  /* If --app-id was specified, or the environment does not specify
   * the server to use, create the factory proxy from the given (or default)
   * name, with no fallback.
   *
   * If the server specified by the environment doesn't exist, fall back to the
   * default server, and ignore the environment-specified parent screen.
   */
  if (options->server_app_id == nullptr &&
      options->server_unique_name != nullptr) {
    gs_free_error GError *err = nullptr;
    if (factory_proxy_new_for_service_name (options->server_unique_name,
                                            TRUE,
                                            options->wait,
                                            factory_ptr,
                                            service_name_ptr,
                                            &err)) {
      *parent_screen_object_path_ptr = g_strdup (options->parent_screen_object_path);
      return TRUE;
    }

    terminal_printerr ("Failed to use specified server: %s\n",
                       err->message);
    terminal_printerr ("Falling back to default server.\n");

    /* Fall back to the default */
    service_name = nullptr;
  }

  *parent_screen_object_path_ptr = nullptr;

  return factory_proxy_new_for_service_name (service_name,
                                             FALSE,
                                             options->wait,
                                             factory_ptr,
                                             service_name_ptr,
                                             error);
}

static bool
handle_show_preferences_remote (TerminalOptions *options,
                                const char *service_name)
{
  gs_free_error GError *error = nullptr;
  gs_unref_object GDBusConnection *bus = nullptr;
  gs_free char *object_path = nullptr;
  GVariantBuilder builder;

  /* For reasons (!?), the org.gtk.Actions interface's object path
   * is derived from the service name, i.e. for service name
   * "foo.bar.baz" the object path is "/foo/bar/baz".
   * This means that without the name (like when given only the unique name),
   * we cannot activate the action.
   */
  if (!service_name ||
      g_dbus_is_unique_name(service_name)) {
    return false;
  }

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, nullptr, &error);
  if (bus == nullptr) {
    terminal_printerr ("Failed to get session bus: %s\n", error->message);
    return true;
  }

  object_path = g_strdelimit (g_strdup_printf (".%s", service_name), ".", '/');

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("(sava{sv})"));
  g_variant_builder_add (&builder, "s", "preferences");
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("av"));
  g_variant_builder_close (&builder);
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
  if (options->startup_id)
    g_variant_builder_add (&builder, "{sv}",
                           "desktop-startup-id", g_variant_new_string (options->startup_id));
  if (options->activation_token)
    g_variant_builder_add (&builder, "{sv}",
                           "activation-token", g_variant_new_string (options->activation_token));
  g_variant_builder_close (&builder);

  if (!g_dbus_connection_call_sync (bus,
                                    service_name,
                                    object_path,
                                    "org.gtk.Actions",
                                    "Activate",
                                    g_variant_builder_end (&builder),
                                    G_VARIANT_TYPE ("()"),
                                    G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                    30 * 1000 /* ms timeout */,
                                    nullptr /* cancelleable */,
                                    &error)) {
    terminal_printerr ("Activate call failed: %s\n", error->message);
    return true;
  }

  return true;
}

static void
handle_show_preferences(TerminalOptions *options,
                        const char *service_name)
{
  // First try remoting to the specified server
  if (handle_show_preferences_remote(options, service_name))
    return;

  // If that isn't possible, launch the prefs binary directly
  auto launcher = g_subprocess_launcher_new(GSubprocessFlags(0));
  gs_free auto exe = terminal_client_get_file_uninstalled(TERM_BINDIR,
                                                          TERM_LIBEXECDIR,
                                                          TERMINAL_PREFERENCES_BINARY_NAME,
                                                          G_FILE_TEST_IS_EXECUTABLE);
  char *argv[2] = {exe, nullptr};

  gs_free_error GError* error = nullptr;
  if (!g_subprocess_launcher_spawnv(launcher, argv, &error)) {
    terminal_printerr ("Failed to launch preferences: %s\n", error->message);
  }
}

/**
 * handle_options:
 * @app:
 * @options: a #TerminalOptions
 * @allow_resume: whether to merge the terminal configuration from the
 *   saved session on resume
 * @wait_for_receiver: location to store the #TerminalReceiver to wait for
 *
 * Processes @options. It loads or saves the terminal configuration, or
 * opens the specified windows and tabs.
 *
 * Returns: %TRUE if @options could be successfully handled, or %FALSE on
 *   error
 */
static gboolean
handle_options (TerminalOptions *options,
                TerminalFactory *factory,
                const char *service_name,
                const char *parent_screen_object_path,
                TerminalReceiver **wait_for_receiver)
{
  /* We need to forward the locale encoding to the server, see bug #732128 */
  const char *encoding;
  g_get_charset (&encoding);

  if (options->show_preferences) {
    handle_show_preferences (options, service_name);
  } else {
    /* Make sure we open at least one window */
    terminal_options_ensure_window (options);
    /* If the user gave no explicit shells but the active profile defines
     * a default-launch-entries list, expand the single implicit tab into
     * one tab per entry. See terminal-options.cc for the trigger gates. */
    terminal_options_expand_profile_launch_entries (options);
  }

  const char *factory_unique_name = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (factory));

  for (GList *lw = options->initial_windows;  lw != nullptr; lw = lw->next)
    {
      InitialWindow *iw = (InitialWindow*)lw->data;

      g_assert_nonnull (iw);

      guint window_id = 0;

      gs_free char *previous_screen_object_path = nullptr;
      if (iw->implicit_first_window)
        previous_screen_object_path = g_strdup (parent_screen_object_path);

      /* Track the screen object path produced by each tab create_instance call,
       * indexed by tab position within this window. When a tab declares a
       * parent_tab_index (only possible via the profile default-launch-entries
       * expansion path — see terminal_options_expand_profile_launch_entries),
       * we look up that index here and pass the parent's path as
       * "axan-sidebar-parent-screen" so the server can wire the sidebar
       * hierarchy at creation time. Forward-reference invariant guarantees
       * the parent appears at a strictly lower index, so it has already been
       * created and pushed when we reach the child. */
      GPtrArray *tab_object_paths = g_ptr_array_new_with_free_func (g_free);

      /* Now add the tabs */
      for (GList *lt = iw->tabs; lt != nullptr; lt = lt->next)
        {
          InitialTab *it = (InitialTab*)lt->data;
          g_assert_nonnull (it);

          GVariantBuilder builder;
          g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

          terminal_client_append_create_instance_options (&builder,
                                                          options->display_name,
                                                          options->startup_id,
                                                          options->activation_token,
                                                          iw->geometry,
                                                          iw->role,
                                                          it->profile ? it->profile : options->default_profile,
                                                          encoding,
                                                          it->title ? it->title : options->default_title,
                                                          it->active,
                                                          iw->start_maximized,
                                                          iw->start_fullscreen);

          /* This will be used to apply missing defaults */
          if (parent_screen_object_path != nullptr)
            g_variant_builder_add (&builder, "{sv}",
                                   "parent-screen", g_variant_new_object_path (parent_screen_object_path));

          /* This will be used to get the parent window */
          if (previous_screen_object_path)
            g_variant_builder_add (&builder, "{sv}",
                                   "window-from-screen", g_variant_new_object_path (previous_screen_object_path));

          /* Sidebar-tree parent linkage. Distinct from "parent-screen" above,
           * which is about profile/zoom inheritance for new-window-from-screen
           * semantics. This option asks the server to re-parent the new
           * screen's sidebar row under the named existing screen's row. */
          if (it->parent_tab_index >= 0 &&
              (guint) it->parent_tab_index < tab_object_paths->len) {
            const char *parent_path =
              (const char *) g_ptr_array_index (tab_object_paths, it->parent_tab_index);
            if (parent_path != nullptr)
              g_variant_builder_add (&builder, "{sv}",
                                     "axan-sidebar-parent-screen",
                                     g_variant_new_object_path (parent_path));
          }

          /* Optional display-name template — non-empty means the sidebar
           * should expand variables in this string and use the result for
           * the row label instead of the OSC-title fallback chain. */
          if (it->name_template != nullptr && it->name_template[0] != '\0')
            g_variant_builder_add (&builder, "{sv}",
                                   "axan-display-name-template",
                                   g_variant_new_string (it->name_template));
          /* Optional sidebar icon identifier — non-empty means the cell
           * should render this icon instead of the fallback status glyph.
           * Forms: builtin:NAME, abs path, or bare filename (see schema). */
          if (it->icon != nullptr && it->icon[0] != '\0')
            g_variant_builder_add (&builder, "{sv}",
                                   "axan-icon",
                                   g_variant_new_string (it->icon));
          /* Optional per-node recolor — `color` is a token (palette name or
           * "#RRGGBB"), `color_target` is icon/text/both. Resolved to a hex
           * per active theme at render time on the server. */
          if (it->color != nullptr && it->color[0] != '\0')
            g_variant_builder_add (&builder, "{sv}",
                                   "axan-color",
                                   g_variant_new_string (it->color));
          if (it->color_target != nullptr && it->color_target[0] != '\0')
            g_variant_builder_add (&builder, "{sv}",
                                   "axan-color-target",
                                   g_variant_new_string (it->color_target));
          if (window_id)
            g_variant_builder_add (&builder, "{sv}",
                                   "window-id", g_variant_new_uint32 (window_id));
          /* Restored windows shouldn't demand attention; see bug #586308. */
          if (iw->source_tag == SOURCE_SESSION)
            g_variant_builder_add (&builder, "{sv}",
                                   "present-window", g_variant_new_boolean (FALSE));
          if (options->zoom_set || it->zoom_set)
            g_variant_builder_add (&builder, "{sv}",
                                   "zoom", g_variant_new_double (it->zoom_set ? it->zoom : options->zoom));
          if (iw->force_menubar_state)
            g_variant_builder_add (&builder, "{sv}",
                                   "show-menubar", g_variant_new_boolean (iw->menubar_state));

          gs_free_error GError *err = nullptr;
          gs_free char *object_path = nullptr;
          if (!terminal_factory_call_create_instance_sync
                 (factory,
                  g_variant_builder_end (&builder),
                  &object_path,
                  nullptr /* cancellable */,
                  &err)) {
            if (handle_create_instance_error (service_name, err))
              return FALSE;
            else {
              /* Keep tab_object_paths aligned with iw->tabs: push NULL so a
               * later tab referencing this index via parent_tab_index lands
               * on a sentinel rather than on an unrelated successor's path. */
              g_ptr_array_add (tab_object_paths, nullptr);
              continue; /* Continue processing the remaining options! */
            }
          }

          /* Deprecated and not working on new server anymore */
          char *p = strstr (object_path, "/window/");
          if (p) {
            char *end = nullptr;
            guint64 value;

            errno = 0;
            p += strlen ("/window/");
            value = g_ascii_strtoull (p, &end, 10);
            if (errno == 0 && end != p && *end == '/')
              window_id = (guint) value;
          }

          g_free (previous_screen_object_path);
          previous_screen_object_path = g_strdup (object_path);
          /* Record this tab's path so subsequent tabs in the same window
           * can reference it via parent_tab_index. The g_strdup gives the
           * array its own copy; the array's free_func is g_free. */
          g_ptr_array_add (tab_object_paths, g_strdup (object_path));

          gs_unref_object TerminalReceiver *receiver =
            terminal_receiver_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                      GDBusProxyFlags(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
								      (it->wait ? 0 : G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS)),
                                                      factory_unique_name,
                                                      object_path,
                                                      nullptr /* cancellable */,
                                                      &err);
          if (receiver == nullptr) {
            if (handle_create_receiver_proxy_error (service_name, err))
              return FALSE;
            else
              continue; /* Continue processing the remaining options! */
          }

          g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

          char **argv = it->exec_argv ? it->exec_argv : options->exec_argv;
          int argc = argv ? g_strv_length (argv) : 0;

          PassFdElement *fd_array = it->fd_array ? (PassFdElement*)it->fd_array->data : nullptr;
          gsize fd_array_len = it->fd_array ? it->fd_array->len : 0;

          terminal_client_append_exec_options (&builder,
                                               !options->no_environment,
                                               it->working_dir ? it->working_dir
                                                               : options->default_working_dir,
                                               fd_array, fd_array_len,
                                               argc == 0);

          if (!terminal_receiver_call_exec_sync (receiver,
                                                 g_variant_builder_end (&builder),
                                                 g_variant_new_bytestring_array ((const char * const *) argv, argc),
                                                 it->fd_list, nullptr /* outfdlist */,
                                                 nullptr /* cancellable */,
                                                 &err)) {
            if (handle_exec_error (service_name, err))
              return FALSE;
            else
              continue; /* Continue processing the remaining options! */
          }

          if (it->wait)
            gs_transfer_out_value (wait_for_receiver, &receiver);

          if (options->print_environment)
            g_print ("%s=%s\n", TERMINAL_ENV_SCREEN, object_path);
        }

      /* Free the per-window object-path tracker. A return FALSE inside the
       * inner loop bypasses this, but that path exits the process anyway. */
      g_ptr_array_unref (tab_object_paths);
    }

  return TRUE;
}

int
main (int argc, char **argv)
{
  int exit_code = EXIT_FAILURE;

  /* axan: replace gnome-terminal's stderr-routing log writer with our
   * file-routing one so client logs land in ~/.cache/axan/axan.log next to
   * the server logs. */
  axan_log_init ();

  g_set_prgname ("axan");

  setlocale (LC_ALL, "");

  terminal_i18n_init (TRUE);

  _terminal_debug_init ();

  gs_free_error GError *error = nullptr;
  gs_free_options TerminalOptions *options = terminal_options_parse (&argc, &argv, &error);
  if (options == nullptr) {
    terminal_printerr (_("Failed to parse arguments: %s\n"), error->message);
    return exit_code;
  }

  /* axan: profile/settings/keybindings import/export takes an early-exit
   * path before D-Bus activation. The operations run against dconf
   * directly so axan-server doesn't need to be reachable. See axan#401. */
  if (options->export_profile_uuid != nullptr) {
    if (!axan_profile_toml_export (options->export_profile_uuid,
                                   options->export_output_path,
                                   &error)) {
      terminal_printerr ("axan: export failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  if (options->import_profile_path != nullptr) {
    gs_free char *imported_uuid = nullptr;
    if (!axan_profile_toml_import (options->import_profile_path,
                                   &imported_uuid,
                                   nullptr,
                                   &error)) {
      terminal_printerr ("axan: import failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    g_print ("%s\n", imported_uuid);
    return EXIT_SUCCESS;
  }
  if (options->export_settings) {
    if (!axan_settings_toml_export (options->export_output_path, &error)) {
      terminal_printerr ("axan: settings export failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  if (options->import_settings_path != nullptr) {
    if (!axan_settings_toml_import (options->import_settings_path, nullptr, &error)) {
      terminal_printerr ("axan: settings import failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  if (options->export_keybindings) {
    if (!axan_keybindings_toml_export (options->export_output_path, &error)) {
      terminal_printerr ("axan: keybindings export failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  if (options->import_keybindings_path != nullptr) {
    if (!axan_keybindings_toml_import (options->import_keybindings_path, nullptr, &error)) {
      terminal_printerr ("axan: keybindings import failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  if (options->export_all) {
    /* Settings + keybindings + every registered profile to default paths
     * under $XDG_CONFIG_HOME/axan/. Stops on the first failure rather
     * than continuing partway through — easier to reason about. */
    if (!axan_settings_toml_export (nullptr, &error) ||
        !axan_keybindings_toml_export (nullptr, &error)) {
      terminal_printerr ("axan: export-all failed: %s\n", error->message);
      return EXIT_FAILURE;
    }
    gs_unref_object GSettings *plist =
      g_settings_new (TERMINAL_PROFILES_LIST_SCHEMA);
    gs_strfreev gchar **uuids =
      g_settings_get_strv (plist, TERMINAL_SETTINGS_LIST_LIST_KEY);
    for (gsize i = 0; uuids[i] != nullptr; ++i) {
      if (!axan_profile_toml_export (uuids[i], nullptr, &error)) {
        terminal_printerr ("axan: export-all failed on profile %s: %s\n",
                           uuids[i], error->message);
        return EXIT_FAILURE;
      }
    }
    return EXIT_SUCCESS;
  }
  if (options->import_all_path != nullptr) {
    /* Read settings.toml, keybindings.toml, and every *.toml under
     * <dir>/profiles/. Each piece uses sparse semantics individually —
     * a missing file just means "don't touch that schema". */
    gs_free char *settings_path = g_build_filename (options->import_all_path,
                                                    "settings.toml", nullptr);
    if (g_file_test (settings_path, G_FILE_TEST_EXISTS)) {
      if (!axan_settings_toml_import (settings_path, nullptr, &error)) {
        terminal_printerr ("axan: import-all failed (settings): %s\n",
                           error->message);
        return EXIT_FAILURE;
      }
    }
    gs_free char *kb_path = g_build_filename (options->import_all_path,
                                              "keybindings.toml", nullptr);
    if (g_file_test (kb_path, G_FILE_TEST_EXISTS)) {
      if (!axan_keybindings_toml_import (kb_path, nullptr, &error)) {
        terminal_printerr ("axan: import-all failed (keybindings): %s\n",
                           error->message);
        return EXIT_FAILURE;
      }
    }
    gs_free char *prof_dir = g_build_filename (options->import_all_path,
                                               "profiles", nullptr);
    GError *dir_err = nullptr;
    GDir *dir = g_dir_open (prof_dir, 0, &dir_err);
    if (dir != nullptr) {
      const char *name;
      while ((name = g_dir_read_name (dir)) != nullptr) {
        if (!g_str_has_suffix (name, ".toml"))
          continue;
        gs_free char *p = g_build_filename (prof_dir, name, nullptr);
        gs_free char *imported_uuid = nullptr;
        if (!axan_profile_toml_import (p, &imported_uuid, nullptr, &error)) {
          terminal_printerr ("axan: import-all failed on %s: %s\n",
                             p, error->message);
          g_dir_close (dir);
          return EXIT_FAILURE;
        }
      }
      g_dir_close (dir);
    } else {
      /* Missing profiles/ directory is fine — just means no profiles to
       * import. Anything else is a real error. */
      if (!g_error_matches (dir_err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        terminal_printerr ("axan: warning: %s\n", dir_err->message);
      g_error_free (dir_err);
    }
    return EXIT_SUCCESS;
  }
  if (options->import_icons) {
    GPtrArray *imported = g_ptr_array_new_with_free_func (g_free);
    GPtrArray *rejected = g_ptr_array_new_with_free_func (axan_icon_rejection_free);
    if (!axan_icons_toml_import (nullptr, imported, rejected, &error)) {
      terminal_printerr ("axan: import-icons failed: %s\n", error->message);
      g_ptr_array_free (imported, TRUE);
      g_ptr_array_free (rejected, TRUE);
      return EXIT_FAILURE;
    }
    g_printerr ("axan: registered %u icon%s\n",
                imported->len, imported->len == 1 ? "" : "s");
    for (guint i = 0; i < imported->len; i++)
      g_printerr ("  + %s\n", (const char *) g_ptr_array_index (imported, i));
    if (rejected->len > 0) {
      g_printerr ("axan: rejected %u file%s\n",
                  rejected->len, rejected->len == 1 ? "" : "s");
      for (guint i = 0; i < rejected->len; i++) {
        AxanIconRejection *r =
          (AxanIconRejection *) g_ptr_array_index (rejected, i);
        g_printerr ("  - %s: %s\n", r->name, r->reason);
      }
    }
    g_ptr_array_free (imported, TRUE);
    g_ptr_array_free (rejected, TRUE);
    return EXIT_SUCCESS;
  }

  g_set_application_name (_("Axan"));

  gs_unref_object TerminalFactory *factory = nullptr;
  gs_free char *service_name = nullptr;
  gs_free char *parent_screen_object_path = nullptr;
  if (!factory_proxy_new (options,
                          &factory,
                          &service_name,
                          &parent_screen_object_path,
                          &error))
    return exit_code;

  if (options->print_environment) {
    const char *name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (factory));
    if (name_owner != nullptr)
      g_print ("%s=%s\n", TERMINAL_ENV_SERVICE_NAME, name_owner);
    else
      return exit_code;
  }

  TerminalReceiver *receiver = nullptr;
  if (!handle_options (options, factory, service_name, parent_screen_object_path, &receiver))
    return exit_code;

  if (receiver != nullptr) {
    exit_code = run_receiver (factory, receiver);
    g_object_unref (receiver);
  } else
    exit_code = EXIT_SUCCESS;

  return exit_code;
}

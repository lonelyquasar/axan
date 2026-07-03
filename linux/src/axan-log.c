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

/* axan-log.c — file-only structured logging.
 *
 * Architecture: we install a single GLogWriterFunc via
 * g_log_set_writer_func() at process start. From that point on every
 * GLib log call — ours, GTK's, libadwaita's, vte's — routes through
 * axan_log_writer() and lands in the log file. Nothing goes to
 * stderr / stdout. This is intentional: per the project workflow
 * preferences, axan should never spam the launching shell.
 *
 * Format chosen for grep-ability:
 *   ISO8601 LEVEL [domain] func: event=name message
 * Example:
 *   2026-05-20T08:52:13.456Z DEBUG [axan] ptyxis_window_init: event=sidebar_bind bound n_pages=0
 *
 * Levels are 5-char left-padded so columns align in the file:
 *   ERROR CRIT  WARN  MSG   INFO  DEBUG
 */

#include "axan-log.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <unistd.h>

static FILE *log_fp = NULL;
static char  log_path[1024] = { 0 };

static const char *
axan_log_level_str (GLogLevelFlags level)
{
  if (level & G_LOG_LEVEL_ERROR)    return "ERROR";
  if (level & G_LOG_LEVEL_CRITICAL) return "CRIT ";
  if (level & G_LOG_LEVEL_WARNING)  return "WARN ";
  if (level & G_LOG_LEVEL_MESSAGE)  return "MSG  ";
  if (level & G_LOG_LEVEL_INFO)     return "INFO ";
  if (level & G_LOG_LEVEL_DEBUG)    return "DEBUG";
  return "?    ";
}

/* Lookup a string field by key. GLib gives us GLogField structs whose
 * .length is -1 for null-terminated string values, >0 for binary
 * blobs. We only handle string fields here. */
static const char *
axan_log_field (const GLogField *fields,
                gsize            n_fields,
                const char      *key)
{
  for (gsize i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, key) == 0 && fields[i].length < 0)
        return (const char *) fields[i].value;
    }
  return NULL;
}

static GLogWriterOutput
axan_log_writer (GLogLevelFlags    level,
                 const GLogField  *fields,
                 gsize             n_fields,
                 gpointer          user_data)
{
  GDateTime *now;
  g_autofree char *ts = NULL;
  const char *domain;
  const char *msg;
  const char *event;
  const char *func;

  (void) user_data;

  if (log_fp == NULL)
    return G_LOG_WRITER_HANDLED;

  now = g_date_time_new_now_utc ();
  ts  = g_date_time_format (now, "%Y-%m-%dT%H:%M:%S.%fZ");
  g_date_time_unref (now);

  domain = axan_log_field (fields, n_fields, "GLIB_DOMAIN");
  msg    = axan_log_field (fields, n_fields, "MESSAGE");
  event  = axan_log_field (fields, n_fields, "EVENT");
  func   = axan_log_field (fields, n_fields, "CODE_FUNC");

  if (domain == NULL) domain = "?";
  if (msg    == NULL) msg    = "(no message)";

  /* "unknown-time" instead of an all-question-mark placeholder so we
   * don't trip the compiler's trigraph warning on "?\?-". */
  fprintf (log_fp, "%s %s [%s]", ts ? ts : "unknown-time",
           axan_log_level_str (level), domain);
  if (func != NULL)
    fprintf (log_fp, " %s", func);
  fprintf (log_fp, ":");
  if (event != NULL)
    fprintf (log_fp, " event=%s", event);
  fprintf (log_fp, " %s\n", msg);
  fflush (log_fp);

  return G_LOG_WRITER_HANDLED;
}

void
axan_log_init (void)
{
  g_autofree char *dir = NULL;

  if (log_fp != NULL)
    return;

  dir = g_build_filename (g_get_user_cache_dir (), "axan", NULL);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      fprintf (stderr, "axan: could not create log dir %s\n", dir);
      return;
    }

  g_snprintf (log_path, sizeof log_path, "%s/axan.log", dir);
  log_fp = g_fopen (log_path, "a");
  if (log_fp == NULL)
    {
      fprintf (stderr, "axan: could not open log file %s\n", log_path);
      return;
    }

  /* Line buffering so we see partial output if axan crashes. */
  setvbuf (log_fp, NULL, _IOLBF, 4096);

  g_log_set_writer_func (axan_log_writer, NULL, NULL);

  axan_log_info ("log_init",
                 "axan log opened pid=%d path=%s",
                 (int) getpid (), log_path);
}

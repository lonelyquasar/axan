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

/* axan-log.h
 *
 * Structured file-only logging for axan.
 *
 * All messages go through GLib's logging system, so we automatically
 * capture every g_debug/g_info/g_message/g_warning/g_critical/g_error
 * emitted by GTK, libadwaita, vte, and Ptyxis-derived code in
 * addition to ours.
 *
 * Output: ~/.cache/axan/axan.log (XDG_CACHE_HOME-aware), append mode.
 * Format: ISO8601-timestamp LEVEL [domain] func: event=name message
 *
 * Call axan_log_init() once, early in main(), before any GTK init.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

void axan_log_init (void);

/* event is a short snake_case identifier ("row_right_pressed",
 * "popover_visible_changed", "sidebar_bind"). Grep the log file by
 * event name when troubleshooting.
 *
 * fmt + variadic args follow printf semantics and become the MESSAGE
 * field. Structured EVENT and CODE_FUNC fields are attached
 * automatically.
 */
#define axan_log_debug(event_name, fmt, ...) \
  g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, \
                    "EVENT", event_name, \
                    "CODE_FUNC", G_STRFUNC, \
                    "MESSAGE", fmt, ##__VA_ARGS__)

#define axan_log_info(event_name, fmt, ...) \
  g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, \
                    "EVENT", event_name, \
                    "CODE_FUNC", G_STRFUNC, \
                    "MESSAGE", fmt, ##__VA_ARGS__)

#define axan_log_warn(event_name, fmt, ...) \
  g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, \
                    "EVENT", event_name, \
                    "CODE_FUNC", G_STRFUNC, \
                    "MESSAGE", fmt, ##__VA_ARGS__)

#define axan_log_error(event_name, fmt, ...) \
  g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, \
                    "EVENT", event_name, \
                    "CODE_FUNC", G_STRFUNC, \
                    "MESSAGE", fmt, ##__VA_ARGS__)

G_END_DECLS

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
 * Auto-export TOML mirrors.
 *
 * Watches GSettings changes (global settings, keybindings, profile list, each
 * profile) and re-emits the canonical TOML files in ~/.config/axan/ after a
 * short debounce. Designed for the server process — wire up at TerminalApp
 * construction time, after profiles_list has been built.
 *
 * The TOML files mirror dconf state. They are not a source of truth; editing
 * them only takes effect when the user (or `axan --import-all`) re-imports.
 *
 * Direct CLI writes (`gsettings set …`) while no axan process is running will
 * not auto-export; in that case, run `axan --export-all` after the change.
 */

#pragma once

#include "terminal-app.hh"

void axan_auto_export_init (TerminalApp *app);

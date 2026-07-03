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
 * axan: vertical-sidebar MDI container for terminal screens.
 *
 * Implements TerminalMdiContainer using a GtkPaned with a GtkListBox
 * sidebar on the left and a GtkStack of screen containers on the right.
 * Replaces the upstream gnome-terminal GtkNotebook-based MDI.
 */

#ifndef TERMINAL_SIDEBAR_H
#define TERMINAL_SIDEBAR_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TERMINAL_TYPE_SIDEBAR         (terminal_sidebar_get_type ())
#define TERMINAL_SIDEBAR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), TERMINAL_TYPE_SIDEBAR, TerminalSidebar))
#define TERMINAL_SIDEBAR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), TERMINAL_TYPE_SIDEBAR, TerminalSidebarClass))
#define TERMINAL_IS_SIDEBAR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), TERMINAL_TYPE_SIDEBAR))
#define TERMINAL_IS_SIDEBAR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), TERMINAL_TYPE_SIDEBAR))
#define TERMINAL_SIDEBAR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), TERMINAL_TYPE_SIDEBAR, TerminalSidebarClass))

typedef struct _TerminalSidebar        TerminalSidebar;
typedef struct _TerminalSidebarClass   TerminalSidebarClass;
typedef struct _TerminalSidebarPrivate TerminalSidebarPrivate;

struct _TerminalSidebar
{
  GtkPaned parent_instance;
  TerminalSidebarPrivate *priv;
};

struct _TerminalSidebarClass
{
  GtkPanedClass parent_class;
};

GType      terminal_sidebar_get_type (void);
GtkWidget *terminal_sidebar_new      (void);

/* Sidebar collapse state. Matches the SidebarState enum in the schema —
 * stored as int via GSettings so the C side does not need to plumb the
 * GEnumValue dance. Values are intentionally numeric-stable: any rewire
 * must keep these ordinals so persisted state continues to load.
 *
 *   EXPANDED  — full row list with names (~320px)
 *   COLLAPSED — narrow column of initial-tab squares
 *   MINIMIZED — thin strip showing only the expand chevron (no cells)
 *
 * Cycle order matches the enum order: expanded → collapsed → minimized
 * → expanded. */
typedef enum {
  TERMINAL_SIDEBAR_STATE_EXPANDED  = 0,
  TERMINAL_SIDEBAR_STATE_COLLAPSED = 1,
  TERMINAL_SIDEBAR_STATE_MINIMIZED = 2,
} TerminalSidebarState;

TerminalSidebarState terminal_sidebar_get_state    (TerminalSidebar *self);
void                 terminal_sidebar_set_state    (TerminalSidebar *self,
                                                    TerminalSidebarState state);
/* Cycle expanded → minimized → collapsed → expanded. Wired to the toolbar
 * chevron and the cycle-sidebar accelerator. */
void                 terminal_sidebar_cycle_state  (TerminalSidebar *self);

/* Set child's parent in the sidebar hierarchy to parent. Both screens must
 * already be present in the sidebar (added via the MDI add_screen path).
 * Pass parent = nullptr to make child a root row.
 *
 * Returns TRUE on success, FALSE if either screen is not in this sidebar
 * or the relationship would form a cycle (i.e., parent is a descendant
 * of child). Callers using this for startup hierarchy via DBus must accept
 * that some failures are silent — the row stays a root, which is the
 * graceful-degrade behavior. */
struct _TerminalScreen;
gboolean   terminal_sidebar_set_screen_parent (TerminalSidebar *self,
                                               struct _TerminalScreen *child,
                                               struct _TerminalScreen *parent);

/* Walk the sidebar in pre-order and emit each row as a 6-tuple matching the
 * default-launch-entries schema: (id, parent_id, name, directory, command, icon).
 * Fresh UUIDs are generated for ids — these are new entries, not the
 * originals — and parent_id references the freshly-generated id of the
 * parent row. `name` is the screen's display-name template if any,
 * `directory` is its current cwd, `command` is always empty (the
 * wrapper-shell exec means we can't recover the original argv reliably),
 * and `icon` is always empty (icons are profile-declared metadata, not
 * recovered from live row state).
 *
 * Returns a floating GVariant of type `a(ssssss)`. */
struct _GVariant;
struct _GVariant *terminal_sidebar_capture_entries (TerminalSidebar *self);

G_END_DECLS

#endif /* TERMINAL_SIDEBAR_H */

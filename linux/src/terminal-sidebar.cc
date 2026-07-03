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
 * v0 MVP: flat GtkListBox of screens, no hierarchy, no drag-drop, no status
 * glyphs. Each row shows the screen's title bound via g_object_bind_property.
 * Selecting a row swaps the visible child in the right-hand GtkStack and
 * emits MDI screen-switched.
 */

#include "config.h"

#include "terminal-sidebar.hh"
#include "terminal-sidebar-icons.hh"
#include "terminal-mdi-container.hh"
#include "terminal-screen.hh"
#include "terminal-screen-container.hh"
#include "terminal-window.hh"
#include "terminal-libgsystem.hh"
#include "terminal-schemas.hh"
#include "terminal-app.hh"
#include "axan-log.h"

#include <glib/gi18n.h>

/* Three discrete sidebar widths matching the v3 wireframes (05/05b/05c).
 * EXPANDED is the user-resizable target via the paned splitter; MINIMIZED
 * and COLLAPSED are fixed-size affordances. The collapsed strip is wide
 * enough to be a comfortable click target (10–12px) but narrow enough that
 * the user reads it as "out of the way." */
#define SIDEBAR_DEFAULT_WIDTH   320
/* Minimized strip is just a thin click target — no cells, only the
 * expand chevron. Anything wider reads as a leftover UI element. */
#define SIDEBAR_MINIMIZED_WIDTH 14

/* Concrete pixel widths for the three CollapsedSize enum values. Kept
 * in C so they can move without a schema migration; the schema's
 * description warns the user these may change between releases.
 *
 *   small  — floor; just enough for the two-letter initials and a glyph
 *   medium — comfortable spacing; cells read as deliberate tiles
 *   large  — reserves space for a future icon slot above the initials
 *
 * Width of the surrounding column = these values; cell width = column - 6
 * (3px gutter on each side). */
#define SIDEBAR_COLLAPSED_W_SMALL  26
#define SIDEBAR_COLLAPSED_W_MEDIUM 44
#define SIDEBAR_COLLAPSED_W_LARGE  72

/* GSettings key used to persist sidebar state across sessions. The
 * sidebar grabs its GSettings instance from TerminalApp's global
 * settings — earlier the sidebar created its own g_settings_new
 * instance on the default backend, but TerminalApp wires its global
 * settings through a custom TerminalSettingsBridgeBackend; the two
 * instances didn't share changed signals, so prefs writes never
 * notified the sidebar. */
#define SIDEBAR_GSETTINGS_KEY  "sidebar-state"

/* Hierarchy indent: row_box margin-start = INDENT_BASE_PX + depth * INDENT_STEP_PX.
 * Step is intentionally small (16 px) — the brief expects dashed elbow guides to
 * carry visual weight, which the theme pass will add. The current MVP renders
 * indent as plain whitespace, which is enough to read structure but not yet
 * styled to spec. */
#define INDENT_BASE_PX 8
#define INDENT_STEP_PX 16

struct _TerminalSidebarPrivate
{
  GtkWidget      *list_box;        /* in the left pane */
  GtkWidget      *stack;           /* in the right pane */
  GtkWidget      *count_label;     /* sidebar toolbar: "N shells" pill */
  GHashTable     *screen_to_row;   /* TerminalScreen* -> GtkListBoxRow* (not owned) */
  TerminalScreen *active_screen;   /* mirror of the current selection, for old/new in screen-switched */
  GtkListBoxRow  *dnd_highlighted; /* most recently drag-highlighted row, for clean-up on motion/leave */
  gboolean        syncing_selection;

  /* Sidebar collapse state plumbing. The left pane swaps between three
   * presentations (expanded list / minimized initial-tabs / collapsed hint
   * strip) by setting the visible child on `view_stack` and adjusting the
   * paned position. Minimized cells mirror the listbox — they are rebuilt
   * lazily whenever the user switches into minimized state (or rows change
   * while already in minimized). */
  TerminalSidebarState  state;
  GSettings            *app_settings;       /* owned ref for sidebar-state persistence */
  GtkWidget            *view_stack;         /* swaps expanded/minimized/collapsed views */
  GtkWidget            *coll_box;           /* the collapsed view's outer vbox */
  GtkWidget            *coll_head;          /* the cycle chevron at the top of the collapsed view */
  GtkWidget            *coll_scrolled;      /* the scrolled window wrapping collapsed_list */
  GtkWidget            *collapsed_list;     /* vbox of cell widgets */
  GHashTable           *cell_to_screen;     /* GtkWidget* (cell) -> TerminalScreen* (not owned) */
  guint                 last_expanded_width;/* paned position remembered before minimize/collapse */
  int                   collapsed_size;     /* CollapsedSize enum value: 0=small, 1=medium, 2=large */
};

/* Custom GTK selection target for drag-reorder. SAME_APP scope is correct:
 * dragging a row out of axan into another application would have no meaning,
 * and accepting external drops would risk treating arbitrary bytes as a row
 * pointer. The info field is unused (only one target) but kept for clarity. */
enum {
  AXAN_TARGET_SIDEBAR_ROW = 0
};

static const GtkTargetEntry sidebar_dnd_targets[] = {
  { (char *)"AXAN_SIDEBAR_ROW", GTK_TARGET_SAME_APP, AXAN_TARGET_SIDEBAR_ROW }
};

static void terminal_sidebar_mdi_iface_init (TerminalMdiContainerInterface *iface);

/* Forward declarations crossing section boundaries. */
static void  sidebar_rebuild_collapsed_list (TerminalSidebar *self);
static char *sidebar_compute_row_label_text (TerminalScreen *screen);
static GtkWidget *sidebar_resolve_expanded_row_icon (TerminalSidebar *self,
                                                     TerminalScreen *screen);
static void  sidebar_settings_changed_cb    (GSettings *settings,
                                             const gchar *key,
                                             gpointer user_data);
static void  sidebar_collapsed_size_changed_cb (GSettings *settings,
                                                const gchar *key,
                                                gpointer user_data);
static void  sidebar_apply_state            (TerminalSidebar *self,
                                             TerminalSidebarState new_state);
static void  sidebar_apply_collapsed_size   (TerminalSidebar *self);
static int   sidebar_collapsed_size_to_width (int size);

G_DEFINE_TYPE_WITH_CODE (TerminalSidebar, terminal_sidebar, GTK_TYPE_PANED,
                         G_ADD_PRIVATE (TerminalSidebar)
                         G_IMPLEMENT_INTERFACE (TERMINAL_TYPE_MDI_CONTAINER, terminal_sidebar_mdi_iface_init))

/* ----- helpers ----------------------------------------------------------- */

static const char *
stack_name_for_screen (TerminalScreen *screen)
{
  /* Stable per-process identifier for the screen, used as GtkStack child name. */
  static char buf[32];
  g_snprintf (buf, sizeof buf, "s%p", (void *) screen);
  return buf;
}

static GtkListBoxRow *
sidebar_lookup_row (TerminalSidebar *self, TerminalScreen *screen)
{
  return GTK_LIST_BOX_ROW (g_hash_table_lookup (self->priv->screen_to_row, screen));
}

static TerminalScreen *
screen_from_row (GtkListBoxRow *row)
{
  if (!row)
    return nullptr;
  return TERMINAL_SCREEN (g_object_get_data (G_OBJECT (row), "axan-screen"));
}

/* ----- hierarchy -------------------------------------------------------- */
/*
 * Hierarchy is a logical layer over the flat GtkListBox: each row stores a
 * pointer to its parent row (NULL = root). Listbox order is maintained as a
 * pre-order traversal of the tree, which gives two useful invariants:
 *
 *   1. A node's subtree is always a *consecutive* range of listbox indices
 *      starting at the node itself, so we can move a subtree by moving a
 *      contiguous range rather than chasing pointers.
 *
 *   2. The end of a subtree is found by walking forward from the source
 *      index until we encounter a row whose depth is <= the source's depth
 *      (or until we run off the end of the listbox).
 *
 * Depth is not cached — every call walks the parent chain. With realistic
 * tree sizes (tens of shells, a few levels deep) this is negligible compared
 * to the GTK reflow work that follows. If profiling ever flags it, the right
 * fix is to cache depth via g_object_set_data and invalidate on reparent.
 */

static GtkListBoxRow *
row_get_parent (GtkListBoxRow *row)
{
  if (!row)
    return nullptr;
  return GTK_LIST_BOX_ROW (g_object_get_data (G_OBJECT (row), "axan-parent"));
}

static void
row_set_parent (GtkListBoxRow *row, GtkListBoxRow *parent)
{
  g_object_set_data (G_OBJECT (row), "axan-parent", parent);
}

static int
row_depth (GtkListBoxRow *row)
{
  int depth = 0;
  for (GtkListBoxRow *p = row_get_parent (row); p != nullptr; p = row_get_parent (p))
    depth++;
  return depth;
}

static gboolean
row_is_descendant_of (GtkListBoxRow *row, GtkListBoxRow *ancestor)
{
  for (GtkListBoxRow *p = row_get_parent (row); p != nullptr; p = row_get_parent (p)) {
    if (p == ancestor)
      return TRUE;
  }
  return FALSE;
}

static void
row_apply_indent (GtkListBoxRow *row)
{
  /* The row_box pointer is stashed at row creation so we can update margin
   * after reparenting without re-walking the widget tree. */
  GtkWidget *row_box = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "axan-row-box"));
  if (row_box == nullptr)
    return;
  int margin = INDENT_BASE_PX + row_depth (row) * INDENT_STEP_PX;
  gtk_widget_set_margin_start (row_box, margin);
}

/* End index (exclusive) of the subtree rooted at the row at start_index.
 * Walks forward over the contiguous range of rows whose depth is strictly
 * greater than the source's depth. */
static int
subtree_end_index (GtkListBox *list_box, int start_index)
{
  GtkListBoxRow *source = gtk_list_box_get_row_at_index (list_box, start_index);
  if (source == nullptr)
    return start_index;
  int source_depth = row_depth (source);
  int i = start_index + 1;
  for (;;) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    if (r == nullptr || row_depth (r) <= source_depth)
      return i;
    i++;
  }
}

/* Walk a row and all its descendants, re-applying indent. Used after a
 * subtree move when the root's parent (and hence every descendant's depth)
 * has changed. */
static void
subtree_apply_indent (GtkListBox *list_box, int start_index)
{
  int end = subtree_end_index (list_box, start_index);
  for (int i = start_index; i < end; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    if (r != nullptr)
      row_apply_indent (r);
  }
}

/* ----- context menus ---------------------------------------------------- */

static void
on_menu_new_shell (GtkMenuItem *item G_GNUC_UNUSED, gpointer user_data)
{
  GtkWidget *anchor = GTK_WIDGET (user_data);
  GtkWidget *toplevel = gtk_widget_get_toplevel (anchor);
  if (!toplevel || !G_IS_ACTION_GROUP (toplevel)) {
    axan_log_warn ("ctxmenu_new_shell", "no win action group reachable from anchor=%p", (void *) anchor);
    return;
  }
  axan_log_debug ("ctxmenu_new_shell", "invoking win.new-terminal");
  g_action_group_activate_action (G_ACTION_GROUP (toplevel), "new-terminal",
                                  g_variant_new ("(ss)", "tab", "current"));
}

/* Walk up the widget tree from a screen to find its containing sidebar.
 * The screen sits inside terminal-screen-container, which sits inside
 * the sidebar's stack, which sits inside the paned (TerminalSidebar
 * itself). Used by context-menu handlers that need to talk back to the
 * sidebar — close-request emission, icon resolution with sidebar
 * settings context, etc. Returns NULL if the walk fails (screen
 * detached from its container). */
static TerminalSidebar *
sidebar_from_screen (TerminalScreen *screen)
{
  GtkWidget *w = GTK_WIDGET (screen);
  while (w && !TERMINAL_IS_SIDEBAR (w))
    w = gtk_widget_get_parent (w);
  return w ? TERMINAL_SIDEBAR (w) : nullptr;
}

static void
on_menu_close_shell (GtkMenuItem *item G_GNUC_UNUSED, gpointer user_data)
{
  /* user_data is a TerminalScreen* captured at menu-build time. Walk up
   * to the sidebar and emit the same screen-close-request the notebook's
   * tab × button used, so window-level confirm-close logic runs
   * unchanged. */
  TerminalScreen *screen = TERMINAL_SCREEN (user_data);
  TerminalSidebar *sidebar = sidebar_from_screen (screen);
  if (sidebar == nullptr)
    return;

  axan_log_debug ("ctxmenu_close_shell", "screen=%p", (void *) screen);
  g_signal_emit_by_name (sidebar, "screen-close-request", screen);
}

/* Resolve a screen's current working directory to a local filesystem path.
 * VTE reports cwd as a file:// URI from OSC 7; this strips the scheme and
 * returns a newly-allocated path the caller frees. Returns NULL when the
 * shell hasn't emitted OSC 7 yet, the URI isn't a local file, or parsing
 * fails — callers should treat NULL as "no cwd known" and grey or skip
 * cwd-dependent actions. */
static char *
screen_resolve_cwd_path (TerminalScreen *screen)
{
  const char *uri = vte_terminal_get_current_directory_uri (VTE_TERMINAL (screen));
  if (uri == nullptr || *uri == '\0')
    return nullptr;
  return g_filename_from_uri (uri, nullptr, nullptr);
}

/* Synchronously fetch the current git branch name for `cwd` by running
 * `git rev-parse --abbrev-ref HEAD`. Reads only the HEAD file under the
 * hood so it's effectively free (sub-millisecond on healthy repos);
 * returning a newly-allocated string. NULL means either the cwd isn't a
 * git repo, git isn't installed, or the spawn failed — caller treats
 * NULL as "no branch to copy" rather than as an error needing user-
 * visible reporting. */
static char *
screen_git_branch (const char *cwd)
{
  if (cwd == nullptr || *cwd == '\0')
    return nullptr;
  const char *argv[] = { "git", "rev-parse", "--abbrev-ref", "HEAD", nullptr };
  gs_free char *stdout_buf = nullptr;
  int exit_status = 0;
  gs_free_error GError *err = nullptr;
  gboolean ok = g_spawn_sync (cwd, (char **) argv, nullptr,
                              GSpawnFlags (G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
                              nullptr, nullptr, &stdout_buf, nullptr,
                              &exit_status, &err);
  if (!ok || exit_status != 0 || stdout_buf == nullptr)
    return nullptr;
  /* git appends a newline; strip and copy out. */
  char *out = g_strdup (stdout_buf);
  g_strchomp (out);
  if (*out == '\0') {
    g_free (out);
    return nullptr;
  }
  return out;
}

/* Read-only toggle handler. Reads the new state off the check-menu-item
 * and pipes it to VTE directly — set_input_enabled flips both keyboard
 * and paste handling, the same way the window-level win.read-only action
 * does but scoped to the right-clicked screen rather than active_screen. */
static void
on_menu_read_only_toggled (GtkCheckMenuItem *item, gpointer user_data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (user_data);
  gboolean read_only = gtk_check_menu_item_get_active (item);
  axan_log_debug ("ctxmenu_read_only", "screen=%p read_only=%d",
                  (void *) screen, read_only);
  vte_terminal_set_input_enabled (VTE_TERMINAL (screen), !read_only);
}

/* Copy the screen's current working directory to the clipboard. The cwd
 * was resolved at menu-build time and stashed on the item (g_strdup so
 * it survives the menu's lifetime regardless of whether the shell cd's
 * between right-click and selection). */
static void
on_menu_copy_path (GtkMenuItem *item, gpointer user_data G_GNUC_UNUSED)
{
  const char *cwd = (const char *) g_object_get_data (G_OBJECT (item), "axan-cwd");
  if (cwd == nullptr || *cwd == '\0')
    return;
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (item));
  GtkClipboard *cb = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
  GtkClipboard *primary = gtk_clipboard_get_for_display (display, GDK_SELECTION_PRIMARY);
  gtk_clipboard_set_text (cb, cwd, -1);
  gtk_clipboard_set_text (primary, cwd, -1);
  axan_log_debug ("ctxmenu_copy_path", "cwd='%s'", cwd);
}

/* Copy the git branch name resolved at menu-build time. Same stash
 * pattern as Copy Path — the value is computed once when the menu opens
 * and reused on activate to avoid re-spawning git after the user has
 * already seen the live state in the menu. */
static void
on_menu_copy_branch (GtkMenuItem *item, gpointer user_data G_GNUC_UNUSED)
{
  const char *branch = (const char *) g_object_get_data (G_OBJECT (item), "axan-branch");
  if (branch == nullptr || *branch == '\0')
    return;
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (item));
  GtkClipboard *cb = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
  GtkClipboard *primary = gtk_clipboard_get_for_display (display, GDK_SELECTION_PRIMARY);
  gtk_clipboard_set_text (cb, branch, -1);
  gtk_clipboard_set_text (primary, branch, -1);
  axan_log_debug ("ctxmenu_copy_branch", "branch='%s'", branch);
}

/* Duplicate the right-clicked shell into a new tab inheriting profile +
 * spawn args via terminal_screen_reexec_from_screen. Distinct from the
 * window-level win.new-terminal action, which always uses active_screen
 * as parent — here the parent is whichever screen was right-clicked,
 * which may not be the currently active one. */
static void
on_menu_duplicate_shell (GtkMenuItem *item G_GNUC_UNUSED, gpointer user_data)
{
  TerminalScreen *parent = TERMINAL_SCREEN (user_data);
  GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (parent));
  if (!TERMINAL_IS_WINDOW (toplevel))
    return;
  TerminalWindow *window = TERMINAL_WINDOW (toplevel);

  gs_unref_object GSettings *profile = terminal_screen_ref_profile (parent);
  if (profile == nullptr)
    return;

  TerminalScreen *screen = terminal_screen_new (profile, nullptr /* title */, 1.0);
  terminal_window_add_screen (window, screen, -1);
  terminal_window_switch_screen (window, screen);
  gtk_widget_grab_focus (GTK_WIDGET (screen));

  /* reexec_from_screen carries the parent's spawn arguments forward —
   * cwd, env, argv. The new shell starts in the same directory and runs
   * the same program. Command isn't preserved if the original shell has
   * since exec'd into something else (e.g. user ran `vim`); cwd is. */
  terminal_screen_reexec_from_screen (screen, parent, nullptr, nullptr);
  axan_log_debug ("ctxmenu_duplicate", "parent=%p new=%p",
                  (void *) parent, (void *) screen);
}

/* ======================================================================
 * Edit session node dialog (M13 "Edit Session Node" design handoff).
 *
 * One consolidated modal mirroring the Windows frontend's in-tree editor:
 * [icon preview | Session name] with a "?" variables popover, a built-in
 * icon button row + Browse, a row of color swatches, and an Apply-color-to
 * (Icon/Text/Both) radio. Replaces the older separate Rename + Icon ▶
 * context-menu items. Session-only, like those were — Save pushes the name
 * template + the per-node icon/recolor onto the live screen; it does not
 * persist across a restart (that rides the launch-entries schema work).
 * ====================================================================== */

/* color_target paints the icon unless explicitly "text" — defined fully
 * near sidebar_make_collapsed_cell; forward-declared here for the dialog's
 * live preview, which sits earlier in the file. */
static gboolean sidebar_target_paints_icon (const char *target);

/* The swatch palette. What gets STORED on a node is the semantic `name`
 * ("red"…"purple"), not a hex — so the same stored value resolves to a
 * theme-appropriate hex at render time and stays legible across light, dark,
 * and future colorblind-safe themes (and stays portable in an exported TOML).
 * A literal "#RRGGBB" is also accepted on a node as an advanced escape hatch
 * (it does NOT adapt across themes); the swatch UI only offers the names.
 *
 * Two tuned six-hue sets from the "Axan Session Colors" design handoff: the
 * color lands on a symbolic glyph + label text directly on the sidebar
 * background, so it's a text-contrast problem. Windows auto-adapts a single
 * set; Adwaita doesn't, so dark and light each get their own, pushed for hue
 * separation + chroma rather than lightened toward the background (which
 * flattened an earlier attempt into indistinguishable pastels). Measured
 * against the worst-case surface per theme — #353535 dark, #f6f5f4 light.
 * Every hue clears AA (4.5:1); yellow/green clear AAA on dark for free. Red is
 * held at the ~4.5:1 floor so it reads as a true red rather than salmon
 * (pushing brighter forces it toward pink); blue leans cyan and purple leans
 * violet for cleaner separation. The first entry is "no color" (name "") →
 * paint with the theme foreground. */
typedef struct { const char *name; const char *hex; const char *label; } NodeEditSwatch;
static const NodeEditSwatch node_edit_swatches_light[] = {
  { "",       "",        N_("No color") },
  { "red",    "#df0025", N_("Red")      },
  { "orange", "#a35c00", N_("Orange")   },
  { "yellow", "#787101", N_("Yellow")   },
  { "green",  "#007f39", N_("Green")    },
  { "blue",   "#0277a4", N_("Blue")     },
  { "purple", "#7d04ff", N_("Purple")   },
};
static const NodeEditSwatch node_edit_swatches_dark[] = {
  { "",       "",        N_("No color") },
  { "red",    "#ff6f69", N_("Red")      },
  { "orange", "#ff9405", N_("Orange")   },
  { "yellow", "#f8ea09", N_("Yellow")   },
  { "green",  "#09ff79", N_("Green")    },
  { "blue",   "#12bbff", N_("Blue")     },
  { "purple", "#b5a1ff", N_("Purple")   },
};

/* Pick the palette that contrasts against the current sidebar background.
 * Both arrays are the same length; *n_out receives it. */
static const NodeEditSwatch *
node_edit_active_swatches (guint *n_out)
{
  *n_out = G_N_ELEMENTS (node_edit_swatches_dark);
  return terminal_sidebar_icon_theme_is_dark () ? node_edit_swatches_dark
                                                 : node_edit_swatches_light;
}

/* Resolve a stored color token to a concrete hex for the active theme:
 *   ""/NULL          → NULL (no recolor; paint with the theme foreground)
 *   "#RRGGBB"        → the literal hex (advanced escape hatch; theme-static)
 *   a palette name   → that hue's hex in the active-theme palette
 *   unrecognized     → NULL + a logged warning (mirrors an unknown builtin icon)
 * Returns a string valid for the duration of the call (a static palette hex,
 * or the caller-owned token pointer for the literal-hex case). */
static const char *
sidebar_resolve_color_token (const char *token)
{
  if (token == nullptr || *token == '\0')
    return nullptr;
  if (token[0] == '#')
    return token; /* literal hex — used verbatim, no theme adaptation */

  guint n = 0;
  const NodeEditSwatch *pal = node_edit_active_swatches (&n);
  for (guint i = 0; i < n; i++) {
    if (pal[i].name != nullptr && *pal[i].name != '\0' &&
        g_ascii_strcasecmp (pal[i].name, token) == 0)
      return (pal[i].hex != nullptr && *pal[i].hex != '\0') ? pal[i].hex : nullptr;
  }
  axan_log_warn ("sidebar.color",
                 "unknown color token '%s' — falling back to theme foreground", token);
  return nullptr;
}

/* Transient state for one open dialog. Lives on the on_menu_edit_node stack
 * frame for the duration of gtk_dialog_run, so handlers capture &state. The
 * three working strings are the in-flight edit; they're only pushed onto the
 * screen on Save. */
typedef struct {
  gchar     *icon_id;       /* "" / "builtin:NAME" / path */
  gchar     *icon_color;    /* "" or "#RRGGBB" */
  gchar     *color_target;  /* "icon" / "text" / "both" */
  GtkWidget *preview_box;   /* 40px frame; holds the live preview image */
  GPtrArray *icon_buttons;  /* GtkToggleButton*, each with "axan-icon-id" */
  GPtrArray *swatches;      /* GtkToggleButton*, each with "axan-hex" */
  gboolean   syncing;       /* re-entrancy guard for the select-* loops */
} NodeEditState;

/* Rebuild the 40px preview from the working icon + color. The icon is tinted
 * only when the target paints the icon — so the preview matches what the row
 * will show, exactly like the Windows _UpdateNodeEditPreview. */
static void
node_edit_update_preview (NodeEditState *st)
{
  GList *kids = gtk_container_get_children (GTK_CONTAINER (st->preview_box));
  for (GList *l = kids; l != nullptr; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (kids);

  const char *tok = (st->icon_color != nullptr && *st->icon_color != '\0' &&
                     sidebar_target_paints_icon (st->color_target))
                      ? st->icon_color : nullptr;
  const char *eff = tok != nullptr ? sidebar_resolve_color_token (tok) : nullptr;
  GtkWidget *img = (st->icon_id != nullptr && *st->icon_id != '\0')
    ? terminal_sidebar_icon_resolve_colored (st->icon_id, 24, eff)
    : nullptr;
  if (img != nullptr) {
    gtk_widget_set_halign (img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
    gtk_container_add (GTK_CONTAINER (st->preview_box), img);
    gtk_widget_show (img);
  }
}

/* Adopt @id as the working icon, sync the button row's active states, and
 * refresh the preview. Also called from Browse, where @id is a path matching
 * no button — every button then goes inactive, which is correct.
 *
 * gtk_toggle_button_set_active() routes through gtk_button_clicked(), which
 * re-emits "clicked" — so without the guard the loop below would re-enter
 * this handler for every button it flips, and the selection would collapse
 * toward the lowest index (you could only ever move the selection earlier in
 * the row). The guard lets the loop set each button's visual state while the
 * re-entrant calls return immediately. */
static void
node_edit_select_icon (NodeEditState *st, const char *id)
{
  if (st->syncing)
    return;
  st->syncing = TRUE;
  /* Dup before free: the initial-sync call passes st->icon_id as @id, so
   * freeing first would leave @id dangling and g_strdup would read freed
   * memory (the value came back as garbage → no button matched → the editor
   * showed "no icon" for a shell that had one). */
  char *new_icon_id = g_strdup (id ? id : "");
  g_free (st->icon_id);
  st->icon_id = new_icon_id;
  for (guint i = 0; i < st->icon_buttons->len; i++) {
    GtkWidget *b = GTK_WIDGET (g_ptr_array_index (st->icon_buttons, i));
    const char *bid = (const char *) g_object_get_data (G_OBJECT (b), "axan-icon-id");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b),
                                  g_strcmp0 (bid, st->icon_id) == 0);
  }
  st->syncing = FALSE;
  node_edit_update_preview (st);
}

static void
node_edit_icon_clicked (GtkButton *b, gpointer user_data)
{
  NodeEditState *st = (NodeEditState *) user_data;
  const char *id = (const char *) g_object_get_data (G_OBJECT (b), "axan-icon-id");
  node_edit_select_icon (st, id);
}

/* @name is the palette name ("red"…"purple") or "" for no-color — the token
 * we STORE on the node, not a hex. The swatch buttons carry the name; we match
 * on it. If the node's stored token is a literal hex (advanced), no swatch
 * matches and none shows selected, which is correct. */
static void
node_edit_select_color (NodeEditState *st, const char *name)
{
  if (st->syncing) /* same set_active → "clicked" re-entrancy as select_icon */
    return;
  st->syncing = TRUE;
  /* Dup before free — see node_edit_select_icon: the initial-sync call aliases
   * @name to st->icon_color, so a free-first would corrupt the value. */
  char *new_color = g_strdup (name ? name : "");
  g_free (st->icon_color);
  st->icon_color = new_color;
  for (guint i = 0; i < st->swatches->len; i++) {
    GtkWidget *s = GTK_WIDGET (g_ptr_array_index (st->swatches, i));
    const char *sname = (const char *) g_object_get_data (G_OBJECT (s), "axan-color-name");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (s),
                                  g_strcmp0 (sname, st->icon_color) == 0);
  }
  st->syncing = FALSE;
  node_edit_update_preview (st);
}

static void
node_edit_swatch_clicked (GtkButton *b, gpointer user_data)
{
  NodeEditState *st = (NodeEditState *) user_data;
  const char *hex = (const char *) g_object_get_data (G_OBJECT (b), "axan-color-name");
  node_edit_select_color (st, hex);
}

/* Apply-to radios fire "toggled" on both the newly-on and newly-off member;
 * act only on the one going active. */
static void
node_edit_apply_toggled (GtkToggleButton *r, gpointer user_data)
{
  if (!gtk_toggle_button_get_active (r))
    return;
  NodeEditState *st = (NodeEditState *) user_data;
  const char *v = (const char *) g_object_get_data (G_OBJECT (r), "axan-target");
  g_free (st->color_target);
  st->color_target = g_strdup (v ? v : "both");
  node_edit_update_preview (st);
}

static void
node_edit_help_clicked (GtkButton *b G_GNUC_UNUSED, gpointer user_data)
{
  gtk_popover_popup (GTK_POPOVER (user_data));
}

/* "Browse to icon file…" — pick any pixbuf-loadable image and set it as the
 * working icon (an absolute path). Mirrors the Windows browse stub. */
static void
node_edit_browse_clicked (GtkButton *b, gpointer user_data)
{
  NodeEditState *st = (NodeEditState *) user_data;
  GtkWidget *top = gtk_widget_get_toplevel (GTK_WIDGET (b));
  GtkFileChooserNative *fc = gtk_file_chooser_native_new (
    _("Choose an icon image"),
    GTK_IS_WINDOW (top) ? GTK_WINDOW (top) : nullptr,
    GTK_FILE_CHOOSER_ACTION_OPEN,
    _("_Open"), _("_Cancel"));
  GtkFileFilter *filt = gtk_file_filter_new ();
  gtk_file_filter_set_name (filt, _("Images"));
  gtk_file_filter_add_pixbuf_formats (filt);
  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (fc), filt);

  if (gtk_native_dialog_run (GTK_NATIVE_DIALOG (fc)) == GTK_RESPONSE_ACCEPT) {
    gs_free char *fn = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (fc));
    if (fn != nullptr)
      node_edit_select_icon (st, fn);
  }
  g_object_unref (fc);
}

/* Insert a variable token into the Session name entry at the cursor. Works
 * while the popover is open (the entry isn't focused, but its caret position
 * is preserved), so the user can click several variables in a row to build a
 * template. */
static void
node_edit_var_insert_clicked (GtkButton *b, gpointer user_data)
{
  GtkEditable *entry = GTK_EDITABLE (user_data);
  const char *tok = (const char *) g_object_get_data (G_OBJECT (b), "axan-var");
  if (tok == nullptr)
    return;
  /* Trailing space so consecutive inserts (and following typed text) don't
   * run together — the popover keeps focus, so the user can't fix it inline
   * between clicks. */
  gs_free char *insert = g_strconcat (tok, " ", nullptr);
  gint pos = gtk_editable_get_position (entry);
  gtk_editable_insert_text (entry, insert, -1, &pos); /* advances pos past the insert */
  gtk_editable_set_position (entry, pos);
}

/* One row in the "?" variables popover: a leading "insert" button (drops the
 * token into the Session name field), a monospace code cell, and a wrapped
 * description. @insert_tok is the canonical token to insert (e.g. "$pwd" for
 * the "$pwd  $cwd" alias row). */
static void
node_edit_var_row (GtkWidget *list, const char *code, const char *desc,
                   const char *insert_tok, GtkWidget *entry)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *ins = gtk_button_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_MENU);
  gtk_button_set_relief (GTK_BUTTON (ins), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (ins, FALSE);
  gtk_widget_set_valign (ins, GTK_ALIGN_CENTER);
  gs_free char *tip = g_strdup_printf (_("Insert %s into the Session name"), insert_tok);
  gtk_widget_set_tooltip_text (ins, tip);
  g_object_set_data_full (G_OBJECT (ins), "axan-var", g_strdup (insert_tok), g_free);
  g_signal_connect (ins, "clicked", G_CALLBACK (node_edit_var_insert_clicked), entry);

  GtkWidget *c = gtk_label_new (code);
  gtk_label_set_xalign (GTK_LABEL (c), 0.0);
  gtk_widget_set_size_request (c, 92, -1);
  gtk_style_context_add_class (gtk_widget_get_style_context (c), "monospace");
  GtkWidget *d = gtk_label_new (desc);
  gtk_label_set_xalign (GTK_LABEL (d), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (d), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (d), 32);

  gtk_box_pack_start (GTK_BOX (row), ins, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), c, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), d, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (list), row, FALSE, FALSE, 0);
}

static GtkWidget *
node_edit_build_help_popover (GtkWidget *relative_to, GtkWidget *entry)
{
  GtkWidget *pop = gtk_popover_new (relative_to);
  gtk_popover_set_position (GTK_POPOVER (pop), GTK_POS_BOTTOM);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);

  GtkWidget *head = gtk_label_new (
    _("Type any of these and axan substitutes the live value. Mix with plain text freely."));
  gtk_label_set_xalign (GTK_LABEL (head), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (head), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (head), 40);
  gtk_style_context_add_class (gtk_widget_get_style_context (head), "dim-label");
  gtk_box_pack_start (GTK_BOX (box), head, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

  /* The real axan grammar (see the root README's label-template section),
   * not the Windows list — so the help matches what the template engine
   * actually expands here. */
  node_edit_var_row (box, "$pwd  $cwd",   _("Full working-directory path"),        "$pwd",         entry);
  node_edit_var_row (box, "$~",           _("Working directory, $HOME collapsed to ~"), "$~",      entry);
  node_edit_var_row (box, "$dir",         _("Current folder name only"),           "$dir",         entry);
  node_edit_var_row (box, "$shell",       _("Shell name — bash, zsh, fish…"),      "$shell",       entry);
  node_edit_var_row (box, "$user",        _("Logged-in user"),                     "$user",        entry);
  node_edit_var_row (box, "$host",        _("Short hostname"),                     "$host",        entry);
  node_edit_var_row (box, "$branch",      _("Active git branch, if any"),          "$branch",      entry);
  node_edit_var_row (box, "$repo",        _("Git repository name"),                "$repo",        entry);
  node_edit_var_row (box, "$cmd  $title", _("Current window title"),               "$cmd",         entry);
  node_edit_var_row (box, "${file:path}", _("First line of the file at path"),     "${file:path}", entry);

  GtkWidget *eg = gtk_label_new ("$dir — $shell   →   axan — bash");
  gtk_label_set_xalign (GTK_LABEL (eg), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (eg), "monospace");
  gtk_box_pack_start (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  gtk_box_pack_start (GTK_BOX (box), eg, FALSE, FALSE, 0);

  gtk_widget_show_all (box);
  gtk_container_add (GTK_CONTAINER (pop), box);
  return pop;
}

/* Build one swatch toggle button. The colored fill + selected ring come from
 * a per-widget CSS provider (GTK3 has no inline style); the "no color" entry
 * shows a clear glyph instead of a fill. */
static GtkWidget *
node_edit_make_swatch (NodeEditState *st, const NodeEditSwatch *sw)
{
  GtkWidget *btn = gtk_toggle_button_new ();
  gtk_widget_set_size_request (btn, 30, 30);
  gtk_widget_set_focus_on_click (btn, FALSE);
  gtk_widget_set_tooltip_text (btn, _(sw->label));
  /* Store the semantic NAME (the node token); fill the swatch with the
   * active-theme hex so the picker is WYSIWYG for the current theme. */
  g_object_set_data_full (G_OBJECT (btn), "axan-color-name", g_strdup (sw->name), g_free);

  GtkCssProvider *prov = gtk_css_provider_new ();
  gs_free char *css = (sw->hex != nullptr && *sw->hex != '\0')
    ? g_strdup_printf (
        "button { background-image:none; background-color:%s;"
        " border-radius:5px; min-width:24px; min-height:24px; padding:0; }"
        "button:checked { box-shadow: inset 0 0 0 2px @theme_selected_bg_color; }",
        sw->hex)
    : g_strdup (
        "button { border-radius:5px; min-width:24px; min-height:24px; padding:0; }"
        "button:checked { box-shadow: inset 0 0 0 2px @theme_selected_bg_color; }");
  gtk_css_provider_load_from_data (prov, css, -1, nullptr);
  gtk_style_context_add_provider (gtk_widget_get_style_context (btn),
                                  GTK_STYLE_PROVIDER (prov),
                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (prov);

  if (sw->hex == nullptr || *sw->hex == '\0') {
    GtkWidget *x = gtk_image_new_from_icon_name ("edit-clear-symbolic", GTK_ICON_SIZE_MENU);
    gtk_container_add (GTK_CONTAINER (btn), x);
  }

  g_signal_connect (btn, "clicked", G_CALLBACK (node_edit_swatch_clicked), st);
  g_ptr_array_add (st->swatches, btn);
  return btn;
}

/* enumerate_options callback: append one icon toggle button per option. */
static void
node_edit_collect_icon (const char *id, const char *label, gpointer user_data)
{
  NodeEditState *st = (NodeEditState *) user_data;
  GtkWidget *btn = gtk_toggle_button_new ();
  gtk_widget_set_focus_on_click (btn, FALSE);
  gtk_widget_set_tooltip_text (btn, label);
  g_object_set_data_full (G_OBJECT (btn), "axan-icon-id", g_strdup (id ? id : ""), g_free);

  GtkWidget *img = (id != nullptr && *id != '\0')
    ? terminal_sidebar_icon_resolve (id, 18)
    : gtk_image_new_from_icon_name ("edit-clear-symbolic", GTK_ICON_SIZE_MENU);
  if (img == nullptr) /* resolve failed — keep a same-size placeholder */
    img = gtk_image_new_from_icon_name ("image-missing", GTK_ICON_SIZE_MENU);
  gtk_widget_set_size_request (img, 18, 18);
  gtk_container_add (GTK_CONTAINER (btn), img);

  g_signal_connect (btn, "clicked", G_CALLBACK (node_edit_icon_clicked), st);
  g_ptr_array_add (st->icon_buttons, btn);
}

/* A bold section heading, like the Windows editor's "Icon"/"Color" labels. */
static GtkWidget *
node_edit_section_label (const char *text)
{
  GtkWidget *l = gtk_label_new (nullptr);
  gs_free char *markup = g_markup_printf_escaped ("<b>%s</b>", text);
  gtk_label_set_markup (GTK_LABEL (l), markup);
  gtk_label_set_xalign (GTK_LABEL (l), 0.0);
  gtk_widget_set_margin_top (l, 8);
  return l;
}

static void
on_menu_edit_node (GtkMenuItem *item G_GNUC_UNUSED, gpointer user_data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (user_data);
  GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (screen));

  NodeEditState st;
  st.icon_id      = g_strdup (terminal_screen_get_icon (screen) ?: "");
  st.icon_color   = g_strdup (terminal_screen_get_icon_color (screen) ?: "");
  const char *cur_target = terminal_screen_get_color_target (screen);
  st.color_target = g_strdup ((cur_target != nullptr && *cur_target != '\0') ? cur_target : "both");
  st.icon_buttons = g_ptr_array_new ();
  st.swatches     = g_ptr_array_new ();
  st.syncing      = FALSE;

  GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Edit session node"),
                                                   GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : nullptr,
                                                   GtkDialogFlags (GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                   _("_Save"), GTK_RESPONSE_ACCEPT,
                                                   nullptr);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  GtkWidget *save_btn = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  if (save_btn != nullptr)
    gtk_style_context_add_class (gtk_widget_get_style_context (save_btn), "suggested-action");

  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  gtk_container_set_border_width (GTK_CONTAINER (content), 18);
  gtk_box_set_spacing (GTK_BOX (content), 2);
  gtk_widget_set_size_request (content, 440, -1);

  /* The Session name entry is created up front so the "?" popover's per-row
   * insert buttons can target it. */
  GtkWidget *name_entry = gtk_entry_new ();
  const char *cur_name = terminal_screen_get_display_name_template (screen);
  gtk_entry_set_text (GTK_ENTRY (name_entry), cur_name ? cur_name : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (name_entry), "$dir — $shell");
  gtk_entry_set_activates_default (GTK_ENTRY (name_entry), TRUE);

  /* --- Session name: bold label + "?" help -------------------------- */
  GtkWidget *name_label_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *name_lbl = gtk_label_new (nullptr);
  gtk_label_set_markup (GTK_LABEL (name_lbl), _("<b>Session name</b>"));
  gtk_label_set_xalign (GTK_LABEL (name_lbl), 0.0);
  GtkWidget *help_btn = gtk_button_new_with_label ("?");
  gtk_widget_set_tooltip_text (help_btn, _("Show available variables"));
  gtk_widget_set_focus_on_click (help_btn, FALSE);
  GtkWidget *help_pop = node_edit_build_help_popover (help_btn, name_entry);
  /* The popover floats free of the content tree (it's relative-to the "?"
   * button), so own it on the dialog to free it on close rather than leak. */
  g_object_ref_sink (help_pop);
  g_object_set_data_full (G_OBJECT (dialog), "axan-help-pop", help_pop, g_object_unref);
  g_signal_connect (help_btn, "clicked", G_CALLBACK (node_edit_help_clicked), help_pop);
  gtk_box_pack_start (GTK_BOX (name_label_row), name_lbl, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (name_label_row), help_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), name_label_row, FALSE, FALSE, 0);

  /* [icon preview | name entry] */
  GtkWidget *name_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *preview_frame = gtk_frame_new (nullptr);
  gtk_widget_set_size_request (preview_frame, 40, 40);
  gtk_widget_set_valign (preview_frame, GTK_ALIGN_CENTER);
  st.preview_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign (st.preview_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (st.preview_box, GTK_ALIGN_CENTER);
  gtk_container_add (GTK_CONTAINER (preview_frame), st.preview_box);
  gtk_box_pack_start (GTK_BOX (name_row), preview_frame, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (name_row), name_entry, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (content), name_row, FALSE, FALSE, 0);

  /* --- Icon: built-in button row + Browse --------------------------- */
  gtk_box_pack_start (GTK_BOX (content), node_edit_section_label (_("Icon")), FALSE, FALSE, 0);
  GtkWidget *icon_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon_flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (icon_flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (icon_flow), 9);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (icon_flow), 4);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (icon_flow), 4);
  /* Pass the current icon as `extra` so a stored custom path shows as a
   * (selectable) button at the end, same preservation the picker had. */
  terminal_sidebar_icon_enumerate_options (st.icon_id, node_edit_collect_icon, &st);
  for (guint i = 0; i < st.icon_buttons->len; i++)
    gtk_flow_box_insert (GTK_FLOW_BOX (icon_flow),
                         GTK_WIDGET (g_ptr_array_index (st.icon_buttons, i)), -1);
  GtkWidget *browse_btn = gtk_button_new_with_label (_("Browse to icon file…"));
  gtk_widget_set_valign (browse_btn, GTK_ALIGN_START);
  g_signal_connect (browse_btn, "clicked", G_CALLBACK (node_edit_browse_clicked), &st);
  gtk_box_pack_start (GTK_BOX (icon_row), icon_flow, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (icon_row), browse_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), icon_row, FALSE, FALSE, 0);

  /* --- Color: swatch row -------------------------------------------- */
  gtk_box_pack_start (GTK_BOX (content), node_edit_section_label (_("Color")), FALSE, FALSE, 0);
  GtkWidget *swatch_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  guint n_swatches = 0;
  const NodeEditSwatch *palette = node_edit_active_swatches (&n_swatches);
  for (guint i = 0; i < n_swatches; i++)
    gtk_box_pack_start (GTK_BOX (swatch_row),
                        node_edit_make_swatch (&st, &palette[i]), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), swatch_row, FALSE, FALSE, 0);

  /* --- Apply color to: label + hint + radios ------------------------ */
  GtkWidget *apply_label_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *apply_lbl = node_edit_section_label (_("Apply color to"));
  gtk_widget_set_valign (apply_lbl, GTK_ALIGN_BASELINE);
  GtkWidget *apply_hint = gtk_label_new (_("recolors glyph icons; image files keep their own colors"));
  gtk_label_set_xalign (GTK_LABEL (apply_hint), 0.0);
  gtk_widget_set_valign (apply_hint, GTK_ALIGN_BASELINE);
  gtk_style_context_add_class (gtk_widget_get_style_context (apply_hint), "dim-label");
  gtk_box_pack_start (GTK_BOX (apply_label_row), apply_lbl, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (apply_label_row), apply_hint, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), apply_label_row, FALSE, FALSE, 0);

  GtkWidget *apply_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);
  GtkWidget *r_icon = gtk_radio_button_new_with_label (nullptr, _("Icon"));
  GtkWidget *r_text = gtk_radio_button_new_with_label_from_widget (GTK_RADIO_BUTTON (r_icon), _("Text"));
  GtkWidget *r_both = gtk_radio_button_new_with_label_from_widget (GTK_RADIO_BUTTON (r_icon), _("Both"));
  g_object_set_data (G_OBJECT (r_icon), "axan-target", (gpointer) "icon");
  g_object_set_data (G_OBJECT (r_text), "axan-target", (gpointer) "text");
  g_object_set_data (G_OBJECT (r_both), "axan-target", (gpointer) "both");
  GtkWidget *r_active = g_str_equal (st.color_target, "icon") ? r_icon
                      : g_str_equal (st.color_target, "text") ? r_text : r_both;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (r_active), TRUE);
  g_signal_connect (r_icon, "toggled", G_CALLBACK (node_edit_apply_toggled), &st);
  g_signal_connect (r_text, "toggled", G_CALLBACK (node_edit_apply_toggled), &st);
  g_signal_connect (r_both, "toggled", G_CALLBACK (node_edit_apply_toggled), &st);
  gtk_box_pack_start (GTK_BOX (apply_row), r_icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (apply_row), r_text, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (apply_row), r_both, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), apply_row, FALSE, FALSE, 0);

  /* Initial selection sync: tick the matching icon button + swatch and paint
   * the first preview. */
  node_edit_select_icon (&st, st.icon_id);
  node_edit_select_color (&st, st.icon_color);

  gtk_widget_show_all (dialog);

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
    const char *name = gtk_entry_get_text (GTK_ENTRY (name_entry));
    terminal_screen_set_display_name_template (screen, name);
    terminal_screen_set_appearance (screen, st.icon_id, st.icon_color, st.color_target);
    axan_log_debug ("ctxmenu_edit_node",
                    "screen=%p name='%s' icon='%s' color='%s' target='%s'",
                    (void *) screen, name, st.icon_id, st.icon_color, st.color_target);
  }
  gtk_widget_destroy (dialog);

  g_free (st.icon_id);
  g_free (st.icon_color);
  g_free (st.color_target);
  g_ptr_array_free (st.icon_buttons, TRUE);
  g_ptr_array_free (st.swatches, TRUE);
}

/* Build an insensitive header item identifying the shell the menu is
 * acting on. The currently-selected shell highlights in the sidebar; the
 * right-click target is some OTHER row, and without a header the menu
 * gives no signal which one. Pairs the row's resolved icon (same path
 * the sidebar uses, including auto-assign) with the row's display label
 * (same text the row shows), so the header reads as a frozen snapshot
 * of the target row.
 *
 * gtk_widget_set_sensitive(FALSE) on the item makes it non-clickable and
 * dim — the GTK convention for "this is informational, not actionable."
 * The trailing separator gives it visual weight against the action
 * items below. */
static void
prepend_row_context_header (GtkMenuShell *menu, TerminalScreen *screen)
{
  GtkWidget *item = gtk_menu_item_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  /* Icon column — use the sidebar's full resolution path (explicit then
   * auto-assigned) so the header matches what the user sees on the row.
   * Placeholder image keeps label alignment consistent when no icon is
   * resolved. */
  GtkWidget *img = nullptr;
  TerminalSidebar *sidebar = sidebar_from_screen (screen);
  if (sidebar != nullptr)
    img = sidebar_resolve_expanded_row_icon (sidebar, screen);
  if (img == nullptr)
    img = gtk_image_new ();
  gtk_widget_set_size_request (img, 16, 16);
  gtk_box_pack_start (GTK_BOX (box), img, FALSE, FALSE, 0);

  /* Label uses the same computation as the sidebar row so the two stay
   * in sync (template expansion, OSC title fallback chain, etc.). Bold
   * for emphasis; the dim insensitive state still lets the bold read. */
  gs_free char *text = sidebar_compute_row_label_text (screen);
  gs_free char *markup = g_markup_printf_escaped ("<b>%s</b>", text ? text : "");
  GtkWidget *lbl = gtk_label_new (nullptr);
  gtk_label_set_markup (GTK_LABEL (lbl), markup);
  gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
  gtk_box_pack_start (GTK_BOX (box), lbl, TRUE, TRUE, 0);

  gtk_container_add (GTK_CONTAINER (item), box);
  gtk_widget_set_sensitive (item, FALSE);
  gtk_menu_shell_append (menu, item);

  GtkWidget *sep = gtk_separator_menu_item_new ();
  gtk_menu_shell_append (menu, sep);
}

static GtkMenu *
build_row_context_menu (GtkWidget *anchor, TerminalScreen *screen)
{
  GtkWidget *menu = gtk_menu_new ();
  GtkMenuShell *shell = GTK_MENU_SHELL (menu);

  /* Resolve cwd + branch once at menu-build time. Stashed on the items
   * that use them so the activate handler doesn't have to re-fetch
   * (and so the values stay consistent with what was true when the
   * user opened the menu). NULL values gate item sensitivity. */
  gs_free char *cwd = screen_resolve_cwd_path (screen);
  gs_free char *branch = (cwd != nullptr) ? screen_git_branch (cwd) : nullptr;

  prepend_row_context_header (shell, screen);

  /* --- lifecycle: New / Duplicate --------------------------------- */
  GtkWidget *item_new = gtk_menu_item_new_with_label (_("New Shell"));
  g_signal_connect (item_new, "activate", G_CALLBACK (on_menu_new_shell), anchor);
  gtk_menu_shell_append (shell, item_new);

  GtkWidget *item_dup = gtk_menu_item_new_with_label (_("Duplicate Shell"));
  g_signal_connect (item_dup, "activate", G_CALLBACK (on_menu_duplicate_shell), screen);
  gtk_menu_shell_append (shell, item_dup);

  gtk_menu_shell_append (shell, gtk_separator_menu_item_new ());

  /* --- identity: Edit session node -------------------------------- */
  /* One consolidated editor (M13 design handoff) replacing the older
   * separate Rename + Icon ▶ items: name template, icon, per-node color,
   * and apply-to. Session-only; Save pushes onto the live screen. */
  GtkWidget *item_edit = gtk_menu_item_new_with_label (_("Edit session node…"));
  g_signal_connect (item_edit, "activate", G_CALLBACK (on_menu_edit_node), screen);
  gtk_menu_shell_append (shell, item_edit);

  gtk_menu_shell_append (shell, gtk_separator_menu_item_new ());

  /* --- behavior: Read Only ---------------------------------------- */
  GtkWidget *item_ro = gtk_check_menu_item_new_with_label (_("Read Only"));
  gboolean read_only = !vte_terminal_get_input_enabled (VTE_TERMINAL (screen));
  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item_ro), read_only);
  g_signal_connect (item_ro, "toggled", G_CALLBACK (on_menu_read_only_toggled), screen);
  gtk_menu_shell_append (shell, item_ro);

  gtk_menu_shell_append (shell, gtk_separator_menu_item_new ());

  /* --- copy: Path / Branch ---------------------------------------- */
  GtkWidget *item_copy_path = gtk_menu_item_new_with_label (_("Copy Path"));
  if (cwd != nullptr) {
    g_object_set_data_full (G_OBJECT (item_copy_path), "axan-cwd", g_strdup (cwd), g_free);
    g_signal_connect (item_copy_path, "activate", G_CALLBACK (on_menu_copy_path), nullptr);
  } else {
    gtk_widget_set_sensitive (item_copy_path, FALSE);
    gtk_widget_set_tooltip_text (item_copy_path,
                                 _("This shell hasn't reported a working directory yet"));
  }
  gtk_menu_shell_append (shell, item_copy_path);

  GtkWidget *item_copy_branch = gtk_menu_item_new_with_label (_("Copy Branch"));
  if (branch != nullptr) {
    g_object_set_data_full (G_OBJECT (item_copy_branch), "axan-branch", g_strdup (branch), g_free);
    g_signal_connect (item_copy_branch, "activate", G_CALLBACK (on_menu_copy_branch), nullptr);
  } else {
    gtk_widget_set_sensitive (item_copy_branch, FALSE);
    gtk_widget_set_tooltip_text (item_copy_branch,
                                 _("This shell's working directory isn't a Git repository"));
  }
  gtk_menu_shell_append (shell, item_copy_branch);

  gtk_menu_shell_append (shell, gtk_separator_menu_item_new ());

  /* --- close ------------------------------------------------------ */
  GtkWidget *item_close = gtk_menu_item_new_with_label (_("Close Shell"));
  g_signal_connect (item_close, "activate", G_CALLBACK (on_menu_close_shell), screen);
  gtk_menu_shell_append (shell, item_close);

  gtk_widget_show_all (menu);
  return GTK_MENU (menu);
}

static GtkMenu *
build_background_context_menu (GtkWidget *anchor)
{
  GtkWidget *menu = gtk_menu_new ();

  GtkWidget *item_new = gtk_menu_item_new_with_label (_("New Shell"));
  g_signal_connect (item_new, "activate", G_CALLBACK (on_menu_new_shell), anchor);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_new);

  gtk_widget_show_all (menu);
  return GTK_MENU (menu);
}

/* Per-row right-click handler. Connected to each row's event_box at
 * add_screen time — the event_box owns the click in widget-coordinate
 * space, so we don't have to chase y-translation issues that affected the
 * old listbox-level handler (which closed the wrong row because event->y
 * came through in event_box-local coordinates, not listbox coordinates).
 *
 * user_data is the GtkListBoxRow* captured at connect time — we read the
 * screen back from its row data so the popup is unambiguously attached to
 * the right-clicked row, even if the row has since been re-indexed by a
 * reorder or its position scrolled out of view. */
static gboolean
on_row_button_press (GtkWidget *event_box G_GNUC_UNUSED,
                     GdkEventButton *event,
                     gpointer user_data)
{
  if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
    return GDK_EVENT_PROPAGATE;

  GtkListBoxRow *row = GTK_LIST_BOX_ROW (user_data);
  TerminalScreen *screen = screen_from_row (row);
  if (screen == nullptr)
    return GDK_EVENT_PROPAGATE;

  axan_log_debug ("ctxmenu", "right-click row idx=%d screen=%p",
                  gtk_list_box_row_get_index (row), (void *) screen);

  GtkMenu *menu = build_row_context_menu (GTK_WIDGET (row), screen);
  gtk_menu_attach_to_widget (menu, GTK_WIDGET (row), nullptr);
  gtk_menu_popup_at_pointer (menu, (GdkEvent *) event);
  return GDK_EVENT_STOP;
}

/* Listbox-level handler — only fires when the right-click landed on
 * empty space below the last row. Per-row clicks are handled by
 * on_row_button_press, which returns GDK_EVENT_STOP and prevents
 * propagation here. */
static gboolean
on_listbox_button_press (GtkWidget *list_box, GdkEventButton *event, gpointer user_data)
{
  if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
    return GDK_EVENT_PROPAGATE;

  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  GtkMenu *menu = build_background_context_menu (GTK_WIDGET (self));
  gtk_menu_attach_to_widget (menu, list_box, nullptr);
  gtk_menu_popup_at_pointer (menu, (GdkEvent *) event);
  return GDK_EVENT_STOP;
}

/* ----- drag-reorder ------------------------------------------------------ */
/*
 * Flat drag-and-drop reorder of sidebar rows. Hierarchy drops will come in a
 * follow-up pass; the design here is intended to generalize: the drop-target
 * computation already does coordinate hit-testing per row, which is where the
 * "make child of" vs "insert before/after sibling" distinction will hang.
 *
 * Implementation notes:
 *
 * - GtkListBoxRow has no GdkWindow (WATCHOUTS entry 5), so a drag *source* on
 *   the row itself receives no button-press events to initiate a drag. We
 *   wrap each row's content in a GtkEventBox (visible-window FALSE so the
 *   row's selection chrome still paints through) and attach the drag source
 *   to that. The drag *dest* lives on the listbox itself, which has its own
 *   window, so a single set of handlers covers all rows.
 *
 * - The dragged row's pointer travels through GtkSelectionData rather than
 *   being stashed in priv. That keeps state out of the sidebar instance and
 *   would let multiple windows drag-between in the future without collision,
 *   though the SAME_APP target means we are not currently exposing that path.
 *
 * - The insert-position math has one subtle off-by-one: if the source row is
 *   earlier in the list than the target, removing it shifts every later
 *   index down by one, so the target index must be decremented to compensate.
 *   Forgetting this puts the moved row one slot past where the user dropped.
 *
 * - The MDI hash table (screen_to_row) maps screen → row pointer. Reordering
 *   the listbox does not invalidate it because the row pointers do not change
 *   — we ref+remove+insert+unref the same GObject. The stack ordering is
 *   independent (it's keyed by stack-name strings) and irrelevant to display
 *   order; only listbox order matters for the sidebar.
 */

static void
on_row_drag_begin (GtkWidget      *widget,
                   GdkDragContext *context,
                   gpointer        user_data)
{
  /* Use a snapshot of the row as the drag cursor. Without this the user
   * drags an empty rectangle, which is disorienting when the rows look
   * similar and there is no other visible feedback besides the highlighted
   * drop target. */
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (user_data);
  GtkAllocation alloc;
  gtk_widget_get_allocation (GTK_WIDGET (row), &alloc);

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         alloc.width,
                                                         alloc.height);
  cairo_t *cr = cairo_create (surface);
  gtk_widget_draw (GTK_WIDGET (row), cr);
  cairo_destroy (cr);
  gtk_drag_set_icon_surface (context, surface);
  cairo_surface_destroy (surface);
}

static void
on_row_drag_data_get (GtkWidget        *widget,
                      GdkDragContext   *context,
                      GtkSelectionData *selection_data,
                      guint             info,
                      guint             time_,
                      gpointer          user_data)
{
  /* The row pointer itself is the payload. 32-bit format is GTK's convention
   * for opaque application-private data; the receiver reads it back with the
   * same width. SAME_APP target keeps this from ever leaving our process. */
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (user_data);
  gtk_selection_data_set (selection_data,
                          gtk_selection_data_get_target (selection_data),
                          32,
                          (const guchar *) &row,
                          sizeof (gpointer));
}

static gboolean
on_listbox_drag_motion (GtkWidget      *widget,
                        GdkDragContext *context,
                        int             x,
                        int             y,
                        guint           time_,
                        gpointer        user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  TerminalSidebarPrivate *priv = self->priv;
  GtkListBoxRow *row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (widget), y);

  /* Highlight the row the cursor is currently over, unhighlighting the
   * previous one. gtk_list_box_drag_unhighlight_row clears all highlights on
   * the box, so we only call it when the target row is actually changing. */
  if (priv->dnd_highlighted != row) {
    if (priv->dnd_highlighted != nullptr)
      gtk_list_box_drag_unhighlight_row (GTK_LIST_BOX (widget));
    if (row != nullptr)
      gtk_list_box_drag_highlight_row (GTK_LIST_BOX (widget), row);
    priv->dnd_highlighted = row;
  }

  gdk_drag_status (context, GDK_ACTION_MOVE, time_);
  return TRUE;
}

static void
on_listbox_drag_leave (GtkWidget      *widget,
                       GdkDragContext *context,
                       guint           time_,
                       gpointer        user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  TerminalSidebarPrivate *priv = self->priv;
  if (priv->dnd_highlighted != nullptr) {
    gtk_list_box_drag_unhighlight_row (GTK_LIST_BOX (widget));
    priv->dnd_highlighted = nullptr;
  }
}

static void
on_listbox_drag_data_received (GtkWidget        *widget,
                               GdkDragContext   *context,
                               int               x,
                               int               y,
                               GtkSelectionData *selection_data,
                               guint             info,
                               guint             time_,
                               gpointer          user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  TerminalSidebarPrivate *priv = self->priv;
  GtkListBox *list_box = GTK_LIST_BOX (widget);

  /* Clear any leftover drop highlight before we mutate the listbox; doing it
   * after the remove+insert would target the wrong row. */
  if (priv->dnd_highlighted != nullptr) {
    gtk_list_box_drag_unhighlight_row (list_box);
    priv->dnd_highlighted = nullptr;
  }

  const guchar *raw = gtk_selection_data_get_data (selection_data);
  if (raw == nullptr) {
    gtk_drag_finish (context, FALSE, FALSE, time_);
    return;
  }

  GtkListBoxRow *source_row = *((GtkListBoxRow * const *) raw);
  GtkListBoxRow *target_row = gtk_list_box_get_row_at_y (list_box, y);

  if (source_row == nullptr) {
    gtk_drag_finish (context, FALSE, FALSE, time_);
    return;
  }

  /* Cycle prevention: cannot make a row a child of its own descendant.
   * Without this, the parent chain becomes a loop and row_depth never
   * terminates. */
  if (target_row != nullptr && row_is_descendant_of (target_row, source_row)) {
    gtk_drag_finish (context, FALSE, FALSE, time_);
    return;
  }

  /* Drop-zone hit testing. Vertical quarters give "make sibling above"
   * (top 25 %), "make child" (middle 50 %), "make sibling below" (bottom
   * 25 %). The middle zone is widest because making-a-child is the new
   * gesture hierarchy provides — sibling reorder was already possible
   * with flat reorder. Dropping below the last row falls through to
   * "append at root level". */
  enum DropZone { DROP_SIBLING_BEFORE, DROP_CHILD, DROP_SIBLING_AFTER };
  enum DropZone zone = DROP_SIBLING_AFTER;
  GtkListBoxRow *new_parent = nullptr;
  int insert_index;

  if (target_row == nullptr) {
    /* Below all rows — append at root level. */
    insert_index = -1;
    new_parent = nullptr;
  } else {
    GtkAllocation target_alloc;
    gtk_widget_get_allocation (GTK_WIDGET (target_row), &target_alloc);
    int rel_y = y - target_alloc.y;
    int h = target_alloc.height;

    if (rel_y < h / 4) {
      zone = DROP_SIBLING_BEFORE;
      new_parent = row_get_parent (target_row);
      insert_index = gtk_list_box_row_get_index (target_row);
    } else if (rel_y > (3 * h) / 4) {
      zone = DROP_SIBLING_AFTER;
      new_parent = row_get_parent (target_row);
      /* "After" means after the target's entire subtree, not literally
       * the next index — otherwise the moved row would land between the
       * target and its first child. */
      insert_index = subtree_end_index (list_box, gtk_list_box_row_get_index (target_row));
    } else {
      zone = DROP_CHILD;
      new_parent = target_row;
      /* As a child, the moved row goes after the target's existing
       * descendants — same insertion index as sibling-after. */
      insert_index = subtree_end_index (list_box, gtk_list_box_row_get_index (target_row));
    }
  }

  /* No-op: source already in the requested position with the requested
   * parent. Skipping silently here keeps the listbox from doing a useless
   * remove+insert that would flash the selection. */
  if (source_row == target_row && zone == DROP_CHILD) {
    /* Drop on self in child zone is a no-op; otherwise sibling-before /
     * sibling-after on self can still be a valid "do nothing" — both
     * branches lead to the same final state. */
    gtk_drag_finish (context, FALSE, FALSE, time_);
    return;
  }

  /* Find the source's subtree range; everything in it travels together. */
  int source_start = gtk_list_box_row_get_index (source_row);
  int source_end = subtree_end_index (list_box, source_start);
  int subtree_size = source_end - source_start;

  /* If the subtree precedes the insertion point, removing it shifts the
   * insertion index down by the subtree's size. */
  if (insert_index >= 0 && source_start < insert_index)
    insert_index -= subtree_size;

  /* Collect the subtree (refs hold them alive across the remove). */
  GPtrArray *moving = g_ptr_array_new_full (subtree_size, nullptr);
  for (int i = source_start; i < source_end; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    g_object_ref (r);
    g_ptr_array_add (moving, r);
  }

  /* Remove back-to-front so each gtk_container_remove sees stable indices
   * for the rows still in the listbox. */
  for (int i = source_end - 1; i >= source_start; i--) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    gtk_container_remove (GTK_CONTAINER (list_box), GTK_WIDGET (r));
  }

  /* Reparent only the subtree root. Descendants point at their direct
   * parents which are still in the moving set, so their parent pointers
   * remain valid — only their *depth* changes, which we re-apply below. */
  row_set_parent (source_row, new_parent);

  /* Re-insert in order, then re-apply indent across the whole moved
   * subtree (the root's depth changed, all descendants follow). */
  int dest = (insert_index < 0) ? -1 : insert_index;
  for (guint i = 0; i < moving->len; i++) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (g_ptr_array_index (moving, i));
    gtk_list_box_insert (list_box, GTK_WIDGET (r),
                         (dest < 0) ? -1 : (dest + (int) i));
    g_object_unref (r);
  }

  /* After insert, find where the subtree actually landed and re-indent. */
  int new_source_index = gtk_list_box_row_get_index (source_row);
  if (new_source_index >= 0)
    subtree_apply_indent (list_box, new_source_index);

  g_ptr_array_free (moving, TRUE);

  /* GtkListBox does not preserve selection across remove+insert. If the
   * moved row was the active one, restore selection so the user does not
   * see the focused terminal change just because they re-ordered. */
  TerminalScreen *screen = (TerminalScreen *) g_object_get_data (G_OBJECT (source_row), "axan-screen");
  if (screen != nullptr && screen == priv->active_screen)
    gtk_list_box_select_row (list_box, source_row);

  sidebar_rebuild_collapsed_list (self);

  /* Window listens for this to refresh tab-move-left / tab-move-right
   * action sensitivity. With hierarchy in play those actions' semantics
   * are arguably weirder (move within siblings? across the tree?) — that
   * design question is parked until status-state work. */
  g_signal_emit_by_name (self, "screens-reordered");

  gtk_drag_finish (context, TRUE, FALSE, time_);
}

/* ----- toolbar ----------------------------------------------------------- */

static void
sidebar_update_count (TerminalSidebar *self)
{
  TerminalSidebarPrivate *priv = self->priv;
  guint n = g_hash_table_size (priv->screen_to_row);
  gs_free char *text = g_strdup_printf (ngettext ("%u shell", "%u shells", n), n);
  gtk_label_set_text (GTK_LABEL (priv->count_label), text);
}

/* ----- collapse state --------------------------------------------------- */

/* Abbreviation for an initial-tab cell, up to max_chars characters. Strips
 * punctuation, splits on spaces/dashes/underscores, and prefers one letter
 * from each leading word; falls back to a prefix of the single word when
 * there aren't enough words. max_chars maps to the collapsed-size step:
 * small=2, medium=3, large=4 — each step buys room for one more letter
 * before crowding the cell.
 *
 * Examples (max_chars=3):
 *   "dev-server"          → "ds"   (2 words, 2 letters; no third word)
 *   "main one two three"  → "mot"  (3 leading words, first letter each)
 *   "monorepo"            → "mon"  (single word, 3-char prefix)
 *   ""                    → "·"    (placeholder dot) */
static char *
sidebar_make_initials (const char *name, guint max_chars)
{
  if (max_chars == 0) max_chars = 2;
  if (max_chars > 8) max_chars = 8; /* hard cap; nothing useful past this */
  if (name == nullptr || *name == '\0')
    return g_strdup ("·");
  gs_free char *folded = g_utf8_strdown (name, -1);
  GString *cleaned = g_string_new (nullptr);
  gboolean prev_was_sep = TRUE;
  for (const char *p = folded; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (g_unichar_isalnum (c)) {
      g_string_append_unichar (cleaned, c);
      prev_was_sep = FALSE;
    } else if (!prev_was_sep) {
      g_string_append_c (cleaned, ' ');
      prev_was_sep = TRUE;
    }
  }
  /* trim trailing space */
  while (cleaned->len > 0 && cleaned->str[cleaned->len - 1] == ' ')
    g_string_truncate (cleaned, cleaned->len - 1);

  char **parts = g_strsplit (cleaned->str, " ", -1);
  g_string_free (cleaned, TRUE);

  guint nparts = g_strv_length (parts);
  GString *out = g_string_new (nullptr);

  /* Prefer one initial per word, up to max_chars words. */
  guint i = 0;
  while (i < nparts && out->len < max_chars) {
    if (parts[i][0] != '\0')
      g_string_append_c (out, parts[i][0]);
    i++;
  }

  /* If we ran out of words before filling max_chars (single short name, or
   * single multi-letter word), fall back to a prefix of the first word.
   * "monorepo" + max_chars=3 → "mon", not "m". */
  if (out->len < max_chars && nparts >= 1 && parts[0][0] != '\0') {
    g_string_truncate (out, 0);
    gsize len = strlen (parts[0]);
    gsize take = MIN ((gsize) max_chars, len);
    g_string_append_len (out, parts[0], take);
  }

  if (out->len == 0)
    g_string_assign (out, "·");

  g_strfreev (parts);
  return g_string_free (out, FALSE);
}

static void
sidebar_min_cell_clicked_cb (GtkButton *button, gpointer user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  TerminalScreen *screen = TERMINAL_SCREEN (g_object_get_data (G_OBJECT (button), "axan-screen"));
  if (screen == nullptr)
    return;
  GtkListBoxRow *row = sidebar_lookup_row (self, screen);
  if (row != nullptr)
    gtk_list_box_select_row (GTK_LIST_BOX (self->priv->list_box), row);
}

/* Build one minimized-cell button and append it to the column. The cell is
 * a flat button (no border chrome) holding a vertical box: status glyph row
 * on top, two-letter initials on the bottom. Active cells get a CSS class
 * the theme uses for the warm-highlight background; nested cells get a
 * `.nested` class the theme draws the dashed elbow on. */
/* Per-node recolor target tests (M13). color_target is "icon"/"text"/"both",
 * with empty/NULL meaning the "both" default — so the recolor paints the icon
 * unless the target is explicitly "text", and paints the label text unless the
 * target is explicitly "icon". */
static gboolean
sidebar_target_paints_icon (const char *target)
{
  return g_strcmp0 (target, "text") != 0;
}

static gboolean
sidebar_target_paints_text (const char *target)
{
  return g_strcmp0 (target, "icon") != 0;
}

/* The effective per-node icon color for a screen, as a concrete hex: the
 * stored token resolved against the active theme when the target paints the
 * icon, else NULL (paint with the theme fg). Centralised so the expanded row,
 * the collapsed cell, and the dialog preview all decide the icon tint
 * identically. */
static const char *
sidebar_screen_icon_tint (TerminalScreen *screen)
{
  const char *token = terminal_screen_get_icon_color (screen);
  if (token == nullptr || *token == '\0')
    return nullptr;
  if (!sidebar_target_paints_icon (terminal_screen_get_color_target (screen)))
    return nullptr;
  return sidebar_resolve_color_token (token);
}

/* The effective per-node text color for a screen, as a concrete hex: the
 * stored token resolved against the active theme when the target paints text,
 * else NULL (use the theme's default label color). */
static const char *
sidebar_screen_text_tint (TerminalScreen *screen)
{
  const char *token = terminal_screen_get_icon_color (screen);
  if (token == nullptr || *token == '\0')
    return nullptr;
  if (!sidebar_target_paints_text (terminal_screen_get_color_target (screen)))
    return nullptr;
  return sidebar_resolve_color_token (token);
}

/* Paint a label's text color via a per-label GtkCssProvider (kept on the
 * widget as object data so a later change just reloads its data rather than
 * stacking providers). USER priority so the node color wins over the theme,
 * including the selected-row state — a node tagged "red" stays red whether or
 * not its row is selected. @hex NULL/empty clears the override (the label
 * falls back to the theme's default color). Used for the M13 per-node "Apply
 * color to: Text/Both" recolor. We use a CSS provider rather than the
 * deprecated gtk_widget_override_color so the color survives GTK theme
 * recomputes and the row's frequent label-text updates. */
static void
sidebar_apply_label_color (GtkWidget *label, const char *hex)
{
  GtkStyleContext *ctx = gtk_widget_get_style_context (label);
  GtkCssProvider *prov =
    GTK_CSS_PROVIDER (g_object_get_data (G_OBJECT (label), "axan-color-provider"));

  if (prov == nullptr) {
    if (hex == nullptr || *hex == '\0')
      return; /* nothing set, nothing to clear */
    prov = gtk_css_provider_new ();
    gtk_style_context_add_provider (ctx, GTK_STYLE_PROVIDER (prov),
                                    GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_set_data_full (G_OBJECT (label), "axan-color-provider",
                            prov, g_object_unref);
  }

  gs_free char *css = (hex != nullptr && *hex != '\0')
    ? g_strdup_printf ("label { color: %s; }", hex)
    : g_strdup ("label {}");
  gtk_css_provider_load_from_data (prov, css, -1, nullptr);
}

static GtkWidget *
sidebar_make_collapsed_cell (TerminalSidebar *self, GtkListBoxRow *row)
{
  TerminalScreen *screen = screen_from_row (row);
  if (screen == nullptr)
    return nullptr;

  /* Cell dimensions scale with the configured size step. Explicit
   * size_request is the one thing GTK reliably honors through the
   * scrolled-window + viewport stack — earlier FILL/expand attempts
   * left cells at their tiny natural width. */
  int col_w = sidebar_collapsed_size_to_width (self->priv->collapsed_size);
  int cell_w = col_w - 6; /* 3px gutter on each side */
  if (cell_w < 18) cell_w = 18;
  int cell_h = cell_w * 13 / 10;
  if (cell_h > 96) cell_h = 96;
  if (cell_h < 28) cell_h = 28;

  GtkWidget *cell = gtk_button_new ();
  gtk_widget_set_size_request (cell, cell_w, cell_h);
  gtk_widget_set_halign (cell, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click (cell, FALSE);
  gtk_button_set_relief (GTK_BUTTON (cell), GTK_RELIEF_NONE);
  gtk_style_context_add_class (gtk_widget_get_style_context (cell),
                               "axan-collapsed-cell");
  /* Per-step size class so CSS can scale the inner glyph + initials
   * font sizes. The cell container alone growing wasn't visually
   * readable — the eye latches onto the glyph and the two-letter
   * abbreviation, both of which were stuck at 11px / 9px regardless
   * of cell width. */
  switch (self->priv->collapsed_size) {
  case 1:  gtk_style_context_add_class (gtk_widget_get_style_context (cell), "size-medium"); break;
  case 2:  gtk_style_context_add_class (gtk_widget_get_style_context (cell), "size-large"); break;
  case 0:
  default: gtk_style_context_add_class (gtk_widget_get_style_context (cell), "size-small"); break;
  }
  if (row_depth (row) > 0)
    gtk_style_context_add_class (gtk_widget_get_style_context (cell), "nested");
  if (row == gtk_list_box_get_selected_row (GTK_LIST_BOX (self->priv->list_box)))
    gtk_style_context_add_class (gtk_widget_get_style_context (cell), "active");

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_set_halign (vbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (vbox, GTK_ALIGN_CENTER);

  /* Top slot: resolved icon if the screen has one assigned, otherwise the
   * status glyph placeholder. The status glyph is hidden at the small
   * step — 20px-wide cells only have room for the initials. Medium and
   * large keep the glyph because there's vertical headroom.
   *
   * Icon sizing tracks the cell: it should fit the cell width minus a
   * 2px breathing-room margin, capped at the cell height minus the
   * initials line height (so the abbreviation still has its own row). */
  const char *icon_id = terminal_screen_get_icon (screen);
  gboolean has_icon = (icon_id != nullptr && icon_id[0] != '\0');
  gboolean is_small_step = (self->priv->collapsed_size == 0);

  /* Auto-assign fallback: when the user opted into auto-assign and the
   * shell has no explicit icon, hash the screen UUID into the curated
   * builtin set. The "builtin:<name>" string lives in a local stack buf
   * because the resolve call only reads it; we don't need to outlive
   * this scope. */
  gs_free char *auto_id = nullptr;
  if (!has_icon &&
      self->priv->app_settings != nullptr &&
      g_settings_get_boolean (self->priv->app_settings,
                              TERMINAL_SETTING_AUTO_ASSIGN_ICON_KEY)) {
    const char *uuid = terminal_screen_get_uuid (screen);
    const char *auto_name = terminal_sidebar_icon_auto_assign (uuid);
    if (auto_name != nullptr) {
      auto_id = g_strdup_printf ("builtin:%s", auto_name);
      icon_id = auto_id;
      has_icon = TRUE;
    }
  }

  axan_log_debug ("sidebar.icons",
                  "make_cell screen=%p icon='%s' step=%d",
                  screen, icon_id ? icon_id : "(none)", self->priv->collapsed_size);

  GtkWidget *top_slot = nullptr;
  if (has_icon) {
    int icon_px = cell_w - 2;
    if (icon_px > cell_h - 12) icon_px = cell_h - 12; /* leave room for initials */
    if (icon_px < 12) icon_px = 12;
    top_slot = terminal_sidebar_icon_resolve_colored (icon_id, icon_px,
                                                      sidebar_screen_icon_tint (screen));
    axan_log_debug ("sidebar.icons",
                    "resolve('%s', %d) -> %s",
                    icon_id, icon_px, top_slot ? "widget" : "NULL");
    /* resolve may return NULL on a bad icon string — fall through to
     * the glyph placeholder so the cell still renders. */
  }
  if (top_slot == nullptr && !is_small_step) {
    /* Status glyph placeholder — for now an outlined dot matching the
     * v3 "idle" treatment. Real status states (run/wait/need/exited)
     * ride on a separate follow-up; the glyph spot is already reserved
     * here so plumbing doesn't need to change when those land. */
    top_slot = gtk_label_new ("○");
    gtk_style_context_add_class (gtk_widget_get_style_context (top_slot),
                                 "axan-collapsed-glyph");
  }

  gs_free char *label_text = sidebar_compute_row_label_text (screen);
  guint max_initials = 2;
  if (self->priv->collapsed_size == 1) max_initials = 3;   /* medium */
  else if (self->priv->collapsed_size == 2) max_initials = 4; /* large */
  gs_free char *initials = sidebar_make_initials (label_text, max_initials);
  GtkWidget *ini = gtk_label_new (initials);
  gtk_style_context_add_class (gtk_widget_get_style_context (ini),
                               "axan-collapsed-ini");
  /* Per-node text recolor (M13): tint the initials when the node carries a
   * color targeting text. Collapsed cells are rebuilt wholesale on any
   * appearance change, so this picks up edits without a dedicated refresh. */
  sidebar_apply_label_color (ini, sidebar_screen_text_tint (screen));

  /* Tooltip: full name + (eventually) status. Mirrors the design tip in
   * 05b; matches the user's "name-recovery" interaction. */
  gtk_widget_set_tooltip_text (cell, label_text);

  if (top_slot != nullptr)
    gtk_box_pack_start (GTK_BOX (vbox), top_slot, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), ini, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (cell), vbox);

  g_object_set_data (G_OBJECT (cell), "axan-screen", screen);
  g_signal_connect (cell, "clicked",
                    G_CALLBACK (sidebar_min_cell_clicked_cb), self);

  gtk_widget_show_all (cell);
  return cell;
}

/* Rebuild the minimized column from the current listbox. Cheap (a dozen or
 * two cells); called whenever the cell column might be out of sync —
 * transitioning into minimized state, adding/removing/reordering rows
 * while minimized, or selection changes (so the active cell highlight
 * tracks). */
static void
sidebar_rebuild_collapsed_list (TerminalSidebar *self)
{
  TerminalSidebarPrivate *priv = self->priv;
  if (priv->collapsed_list == nullptr)
    return;
  /* Skip the work while the cell column is hidden — the collapsed view
   * page in the stack isn't drawn, so any work done here would be invisible
   * until the next state transition rebuilds anyway. The transition into
   * collapsed always calls this explicitly, so we won't miss the rebuild. */
  if (priv->state != TERMINAL_SIDEBAR_STATE_COLLAPSED)
    return;

  /* Tear down existing cells. */
  GList *children = gtk_container_get_children (GTK_CONTAINER (priv->collapsed_list));
  for (GList *l = children; l != nullptr; l = l->next)
    gtk_container_remove (GTK_CONTAINER (priv->collapsed_list), GTK_WIDGET (l->data));
  g_list_free (children);
  g_hash_table_remove_all (priv->cell_to_screen);

  /* Walk the listbox in current (pre-order) sequence. Top-level breaks get
   * a thin divider — same affordance as the design (a tiny horizontal rule
   * between sibling subtrees). */
  GList *rows = gtk_container_get_children (GTK_CONTAINER (priv->list_box));
  int prev_depth = -1;
  for (GList *l = rows; l != nullptr; l = l->next) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW (l->data);
    int depth = row_depth (row);
    if (prev_depth > 0 && depth == 0) {
      GtkWidget *div = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
      gtk_style_context_add_class (gtk_widget_get_style_context (div),
                                   "axan-collapsed-divider");
      gtk_widget_set_margin_start (div, 6);
      gtk_widget_set_margin_end (div, 6);
      gtk_widget_set_margin_top (div, 4);
      gtk_widget_set_margin_bottom (div, 4);
      gtk_widget_show (div);
      gtk_box_pack_start (GTK_BOX (priv->collapsed_list), div, FALSE, FALSE, 0);
    }
    GtkWidget *cell = sidebar_make_collapsed_cell (self, row);
    if (cell != nullptr) {
      gtk_box_pack_start (GTK_BOX (priv->collapsed_list), cell, FALSE, FALSE, 0);
      g_hash_table_insert (priv->cell_to_screen, cell, screen_from_row (row));
    }
    prev_depth = depth;
  }
  g_list_free (rows);
}

/* The chevron in the expanded toolbar and the expand button at the top of
 * the minimized column both route through cycle_state. The collapsed strip
 * goes one step further — clicking it jumps straight back to expanded,
 * skipping minimized, because that's the recovery move the user wants
 * (they hid the sidebar by accident or finished hiding it and now want it
 * back at full size). */
static void
sidebar_cycle_button_clicked_cb (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  terminal_sidebar_cycle_state (TERMINAL_SIDEBAR (user_data));
}

static void
sidebar_minimized_button_clicked_cb (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  terminal_sidebar_set_state (TERMINAL_SIDEBAR (user_data),
                              TERMINAL_SIDEBAR_STATE_EXPANDED);
}

static const char *
sidebar_state_to_nick (TerminalSidebarState s)
{
  switch (s) {
  case TERMINAL_SIDEBAR_STATE_MINIMIZED: return "minimized";
  case TERMINAL_SIDEBAR_STATE_COLLAPSED: return "collapsed";
  case TERMINAL_SIDEBAR_STATE_EXPANDED:
  default:                               return "expanded";
  }
}

static void
sidebar_apply_state (TerminalSidebar *self, TerminalSidebarState new_state)
{
  TerminalSidebarPrivate *priv = self->priv;
  TerminalSidebarState old_state = priv->state;
  if (new_state == old_state)
    return;

  /* Remember the expanded width when leaving expanded so we can restore it
   * on the way back. The user might have dragged the splitter to a custom
   * width — clobbering it on every cycle would feel rude. */
  if (old_state == TERMINAL_SIDEBAR_STATE_EXPANDED) {
    int pos = gtk_paned_get_position (GTK_PANED (self));
    if (pos >= 100)
      priv->last_expanded_width = (guint) pos;
  }

  priv->state = new_state;
  axan_log_info ("sidebar.state", "old=%s new=%s",
                 sidebar_state_to_nick (old_state),
                 sidebar_state_to_nick (new_state));

  if (priv->view_stack != nullptr)
    gtk_stack_set_visible_child_name (GTK_STACK (priv->view_stack),
                                      sidebar_state_to_nick (new_state));

  int target_pos = (int) priv->last_expanded_width;
  switch (new_state) {
  case TERMINAL_SIDEBAR_STATE_EXPANDED:
    target_pos = (priv->last_expanded_width >= 100) ? (int) priv->last_expanded_width
                                                    : SIDEBAR_DEFAULT_WIDTH;
    break;
  case TERMINAL_SIDEBAR_STATE_COLLAPSED:
    target_pos = sidebar_collapsed_size_to_width (priv->collapsed_size);
    sidebar_rebuild_collapsed_list (self);
    break;
  case TERMINAL_SIDEBAR_STATE_MINIMIZED:
    target_pos = SIDEBAR_MINIMIZED_WIDTH;
    break;
  }
  gtk_paned_set_position (GTK_PANED (self), target_pos);

  /* Persist. We write the nick string via the enum-aware setter so the
   * stored value survives schema-key reorderings. */
  if (priv->app_settings != nullptr) {
    g_settings_set_enum (priv->app_settings, SIDEBAR_GSETTINGS_KEY, (int) new_state);
  }
}

/* GSettings "changed::sidebar-state" handler. Mirrors external changes
 * into the running sidebar; sidebar_apply_state is a no-op when the
 * incoming value matches current state, so writes from this same widget
 * (which call g_settings_set_enum) don't bounce. */
static void
sidebar_settings_changed_cb (GSettings *settings,
                             const gchar *key G_GNUC_UNUSED,
                             gpointer user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  int v = g_settings_get_enum (settings, SIDEBAR_GSETTINGS_KEY);
  sidebar_apply_state (self, (TerminalSidebarState) v);
}

/* CollapsedSize enum value → concrete pixel width. The three steps are
 * defined at the top of this file; an unknown value falls back to small
 * so a forward-incompatible GSettings value doesn't render at zero. */
static int
sidebar_collapsed_size_to_width (int size)
{
  switch (size) {
  case 1: return SIDEBAR_COLLAPSED_W_MEDIUM;
  case 2: return SIDEBAR_COLLAPSED_W_LARGE;
  case 0:
  default: return SIDEBAR_COLLAPSED_W_SMALL;
  }
}

/* GSettings "changed::show-icons-in-expanded" handler. Flip visibility
 * on every row's stashed icon widget — no row rebuild needed because
 * the icon widget was constructed eagerly at row-creation time. Rows
 * without an icon (no explicit + auto-assign off) carry no stash and
 * are silently skipped. */
static void
sidebar_show_icons_in_expanded_changed_cb (GSettings *settings,
                                           const gchar *key G_GNUC_UNUSED,
                                           gpointer user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  gboolean show = g_settings_get_boolean (settings,
                                          TERMINAL_SETTING_SHOW_ICONS_IN_EXPANDED_KEY);
  GList *rows = gtk_container_get_children (GTK_CONTAINER (self->priv->list_box));
  for (GList *l = rows; l != nullptr; l = l->next) {
    GtkWidget *icon = GTK_WIDGET (g_object_get_data (G_OBJECT (l->data), "axan-row-icon"));
    if (icon != nullptr)
      gtk_widget_set_visible (icon, show);
  }
  g_list_free (rows);
}

/* GSettings "changed::collapsed-size" handler. Update the cached enum
 * value and re-apply geometry. No fallback chain through paned
 * positions or feedback loops — the prefs combobox is the only writer
 * for this key. */
static void
sidebar_collapsed_size_changed_cb (GSettings *settings,
                                   const gchar *key G_GNUC_UNUSED,
                                   gpointer user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  TerminalSidebarPrivate *priv = self->priv;
  int new_size = g_settings_get_enum (settings, TERMINAL_SETTING_COLLAPSED_SIZE_KEY);
  axan_log_debug ("sidebar.changed_cb",
                  "collapsed-size signal fired: old=%d new=%d",
                  priv->collapsed_size, new_size);
  priv->collapsed_size = new_size;
  sidebar_apply_collapsed_size (self);
}

/* Push the cached collapsed_size into the widget tree: paned position
 * (if currently collapsed) and a cell rebuild so each cell takes the
 * new pixel dimension. Safe to call any time after init — does nothing
 * for the parts that don't yet exist. Outer-column / chevron / scrolled
 * widths are no longer clamped here; see the comment block in the body
 * for why and what the long-term width-control plan is. */
static void
sidebar_apply_collapsed_size (TerminalSidebar *self)
{
  TerminalSidebarPrivate *priv = self->priv;
  int w = sidebar_collapsed_size_to_width (priv->collapsed_size);

  axan_log_debug ("sidebar.size", "apply collapsed_size=%d width=%d state=%d",
                  priv->collapsed_size, w, (int) priv->state);

  /* Intentionally NOT calling gtk_widget_set_size_request on coll_box /
   * coll_head or gtk_scrolled_window_set_min_content_width on
   * coll_scrolled here. Those clamps re-introduced the wiggle-corruption
   * pattern documented in WATCHOUTS entry 3: a runtime size constraint
   * propagating through the GtkStack and GtkPaned, contributing to the
   * SIGWINCH-storm conditions during fast window resize. Cells inside
   * coll_box have their own size_request (see sidebar_make_cell), so the
   * collapsed column still takes roughly the configured width by virtue
   * of its largest child. If the column drifts noticeably from the
   * configured small/medium/large step, the correct fix lives in CSS
   * (min-width on .axan-sidebar-collapsed) — which is consulted once at
   * style resolution rather than on every allocate cycle. */
  (void) w;
  if (priv->state == TERMINAL_SIDEBAR_STATE_COLLAPSED)
    gtk_paned_set_position (GTK_PANED (self), w);
  sidebar_rebuild_collapsed_list (self);
}

TerminalSidebarState
terminal_sidebar_get_state (TerminalSidebar *self)
{
  g_return_val_if_fail (TERMINAL_IS_SIDEBAR (self), TERMINAL_SIDEBAR_STATE_EXPANDED);
  return self->priv->state;
}

void
terminal_sidebar_set_state (TerminalSidebar *self, TerminalSidebarState state)
{
  g_return_if_fail (TERMINAL_IS_SIDEBAR (self));
  sidebar_apply_state (self, state);
}

void
terminal_sidebar_cycle_state (TerminalSidebar *self)
{
  g_return_if_fail (TERMINAL_IS_SIDEBAR (self));
  /* Cycle: expanded → collapsed (cells) → minimized (strip) → expanded.
   * Matches the user's mental model of "progressive narrowing." */
  TerminalSidebarState next;
  switch (self->priv->state) {
  case TERMINAL_SIDEBAR_STATE_EXPANDED:  next = TERMINAL_SIDEBAR_STATE_COLLAPSED; break;
  case TERMINAL_SIDEBAR_STATE_COLLAPSED: next = TERMINAL_SIDEBAR_STATE_MINIMIZED; break;
  case TERMINAL_SIDEBAR_STATE_MINIMIZED:
  default:                               next = TERMINAL_SIDEBAR_STATE_EXPANDED; break;
  }
  sidebar_apply_state (self, next);
}


/* ----- MDI vfuncs -------------------------------------------------------- */

/* Substitute the user's home directory at the start of `path` with "~".
 * Returns a newly-allocated string. If path doesn't begin with the home
 * directory, returns a plain copy. NULL/empty inputs return "". */
static char *
sidebar_collapse_home (const char *path)
{
  if (path == nullptr || *path == '\0')
    return g_strdup ("");
  const char *home = g_get_home_dir ();
  if (home == nullptr || *home == '\0')
    return g_strdup (path);
  gsize hlen = strlen (home);
  if (strncmp (path, home, hlen) != 0)
    return g_strdup (path);
  /* Match must end at a path separator or end-of-string — don't collapse
   * /home/alice-foo when home is /home/alice. */
  if (path[hlen] != '\0' && path[hlen] != '/')
    return g_strdup (path);
  return g_strdup_printf ("~%s", path + hlen);
}

/* Walk up from `start_dir` looking for a directory containing a .git entry.
 * Returns the toplevel directory (newly-allocated) or NULL if none found
 * before hitting the filesystem root. Caller frees with g_free. Cheap — one
 * stat per ancestor, typically only a handful.
 *
 * A `.git` file (worktree pointer, submodule) counts the same as a directory
 * — we just need git to consider this a repo; we don't dereference it. */
static char *
sidebar_find_git_toplevel (const char *start_dir)
{
  if (start_dir == nullptr || *start_dir == '\0')
    return nullptr;
  gs_free char *cur = g_strdup (start_dir);
  while (cur != nullptr && *cur != '\0' && !g_str_equal (cur, "/")) {
    gs_free char *git_path = g_build_filename (cur, ".git", nullptr);
    if (g_file_test (git_path, G_FILE_TEST_EXISTS)) {
      char *result = cur;
      cur = nullptr;
      return result;  /* steal */
    }
    char *parent = g_path_get_dirname (cur);
    g_free (cur);
    cur = parent;
  }
  return nullptr;
}

/* Read `<git_top>/.git/HEAD` and parse the branch name out of "ref:
 * refs/heads/<branch>". Returns "" for detached HEAD (HEAD contains a raw
 * SHA), missing file, or any other parse failure. .git/HEAD changes only
 * on branch switch / commit / detach, so this is cheap.
 *
 * For submodules where .git is a file pointing elsewhere, fall back to
 * NULL — supporting them properly means parsing the gitdir pointer, which
 * we can add when someone actually needs it. */
static char *
sidebar_git_branch_at (const char *git_toplevel)
{
  if (git_toplevel == nullptr)
    return g_strdup ("");
  gs_free char *head_path = g_build_filename (git_toplevel, ".git", "HEAD", nullptr);
  gs_free char *contents = nullptr;
  gsize len = 0;
  if (!g_file_get_contents (head_path, &contents, &len, nullptr))
    return g_strdup ("");
  /* Strip trailing whitespace. */
  while (len > 0 && (contents[len-1] == '\n' || contents[len-1] == '\r' ||
                     contents[len-1] == ' ' || contents[len-1] == '\t'))
    contents[--len] = '\0';
  if (g_str_has_prefix (contents, "ref: refs/heads/"))
    return g_strdup (contents + strlen ("ref: refs/heads/"));
  /* Either detached HEAD (raw SHA), packed-refs-only state, or worktree
   * pointer file. Treat as "no branch to display" — empty string keeps
   * the template short and avoids showing a 40-char hash in the sidebar. */
  return g_strdup ("");
}

/* Read up to 128 bytes from `path`, return the trimmed first line. Used by
 * the ${file:path} brace-form expansion so users can pipe arbitrary data
 * into a sidebar label via a sidecar file. `~` in the path expands to
 * $HOME; absolute and relative paths pass through unchanged. Returns ""
 * for missing/unreadable files so the template doesn't visibly fail. */
static char *
sidebar_read_file_var (const char *path)
{
  if (path == nullptr || *path == '\0')
    return g_strdup ("");
  gs_free char *resolved = nullptr;
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char *home = g_get_home_dir ();
    resolved = g_strdup_printf ("%s%s", home ? home : "", path + 1);
  } else {
    resolved = g_strdup (path);
  }

  GError *err = nullptr;
  GMappedFile *mapped = g_mapped_file_new (resolved, FALSE, &err);
  if (mapped == nullptr) {
    g_clear_error (&err);
    return g_strdup ("");
  }
  const char *data = g_mapped_file_get_contents (mapped);
  gsize len = g_mapped_file_get_length (mapped);
  if (len > 128)
    len = 128;
  /* First line only — split at \n if present. */
  const char *nl = (const char *) memchr (data, '\n', len);
  if (nl != nullptr)
    len = (gsize) (nl - data);
  /* Trim trailing whitespace. */
  while (len > 0 && (data[len-1] == ' ' || data[len-1] == '\t' || data[len-1] == '\r'))
    len--;
  char *out = g_strndup (data, len);
  g_mapped_file_unref (mapped);
  return out;
}

/* Look up a single variable name for sidebar_expand_name_template.
 *
 * Supported names (case-insensitive, underscores/hyphens normalized):
 *   pwd, cwd        — full current working directory
 *   dir             — basename of the current working directory
 *   ~ (special)     — current directory with $HOME collapsed to ~
 *   user            — username
 *   host, hostname  — short hostname
 *   shell           — basename of $SHELL
 *   cmd, title      — VTE window title
 *   branch, gitbranch — git branch via .git/HEAD walk-up
 *   repo, gitrepo   — basename of git toplevel via .git walk-up
 *
 * Returns NULL for unknown names so the expander can emit them literally. */
static char *
sidebar_lookup_template_var (const char *name, TerminalScreen *screen)
{
  /* Special case: a single tilde ($~) requests the home-collapsed pwd —
   * tilde isn't part of our normal identifier charset so we have to spot
   * it before the normalization step (which would drop it). */
  if (g_str_equal (name, "~")) {
    gs_free char *cwd = terminal_screen_get_current_dir (screen);
    return sidebar_collapse_home (cwd);
  }

  /* Normalize lookup: lowercase + strip underscores/hyphens so $host-name
   * and $host_name both resolve to the same lookup key as $hostname. */
  gs_free char *key = g_ascii_strdown (name, -1);
  for (char *p = key, *q = key; ; p++) {
    if (*p == '-' || *p == '_') continue;
    *q++ = *p;
    if (*p == '\0') break;
  }

  if (g_str_equal (key, "pwd") || g_str_equal (key, "cwd")) {
    char *cwd = terminal_screen_get_current_dir (screen);
    return cwd ? cwd : g_strdup ("");
  }
  if (g_str_equal (key, "dir")) {
    gs_free char *cwd = terminal_screen_get_current_dir (screen);
    if (cwd && *cwd != '\0')
      return g_path_get_basename (cwd);
    return g_strdup ("");
  }
  if (g_str_equal (key, "user")) {
    const char *u = g_get_user_name ();
    return g_strdup (u ? u : "");
  }
  if (g_str_equal (key, "host") || g_str_equal (key, "hostname")) {
    const char *h = g_get_host_name ();
    return g_strdup (h ? h : "");
  }
  if (g_str_equal (key, "shell")) {
    const char *s = g_getenv ("SHELL");
    if (s == nullptr || *s == '\0')
      return g_strdup ("");
    return g_path_get_basename (s);
  }
  if (g_str_equal (key, "cmd") || g_str_equal (key, "title")) {
    const char *t = terminal_screen_get_title (screen);
    return g_strdup (t ? t : "");
  }
  if (g_str_equal (key, "branch") || g_str_equal (key, "gitbranch")) {
    gs_free char *cwd = terminal_screen_get_current_dir (screen);
    gs_free char *top = sidebar_find_git_toplevel (cwd);
    return sidebar_git_branch_at (top);
  }
  if (g_str_equal (key, "repo") || g_str_equal (key, "gitrepo")) {
    gs_free char *cwd = terminal_screen_get_current_dir (screen);
    gs_free char *top = sidebar_find_git_toplevel (cwd);
    if (top == nullptr)
      return g_strdup ("");
    return g_path_get_basename (top);
  }
  return nullptr;
}

/* Expand variable references in `tmpl` against the screen's runtime context.
 *
 * Syntax:
 *   $name        — variable lookup, name is [A-Za-z0-9_-]+
 *   $~           — special-cased single-char name (home-collapsed pwd)
 *   ${file:path} — brace-delimited extended form. The only supported prefix
 *                  today is `file:` which reads the file's first line.
 *                  Reserved for future `${env:VAR}`, `${cmd:command}`, etc.
 *
 * Unknown names emit literally (including the `$`) so typos are visible.
 *
 * Caller owns the returned string (g_free). */
static char *
sidebar_expand_name_template (const char *tmpl, TerminalScreen *screen)
{
  if (tmpl == nullptr)
    return g_strdup ("");
  GString *out = g_string_new (nullptr);
  const char *p = tmpl;
  while (*p) {
    if (*p != '$') {
      g_string_append_c (out, *p);
      p++;
      continue;
    }
    /* Brace form: ${...}. Scan for matching `}` with no escaping/nesting —
     * the contents are a path or identifier, neither of which legitimately
     * contains `}`. If we don't find the closer the rest of the template
     * is treated as the body (defensive against malformed input). */
    if (p[1] == '{') {
      const char *body_start = p + 2;
      const char *body_end = strchr (body_start, '}');
      if (body_end == nullptr) {
        /* Unterminated brace — emit the `${` literally and keep going so
         * the user sees their template instead of dropping everything. */
        g_string_append (out, "${");
        p += 2;
        continue;
      }
      gs_free char *body = g_strndup (body_start, body_end - body_start);
      /* Today only `file:` is recognized. Anything else falls through to
       * the literal-emit branch so future prefixes can be added safely. */
      if (g_str_has_prefix (body, "file:")) {
        gs_free char *content = sidebar_read_file_var (body + 5);
        g_string_append (out, content);
      } else {
        g_string_append (out, "${");
        g_string_append (out, body);
        g_string_append_c (out, '}');
      }
      p = body_end + 1;
      continue;
    }
    /* `$~` special case — home-collapsed pwd. */
    if (p[1] == '~') {
      gs_free char *val = sidebar_lookup_template_var ("~", screen);
      g_string_append (out, val ? val : "");
      p += 2;
      continue;
    }
    /* Past the $: collect the variable name. */
    const char *start = p + 1;
    const char *end = start;
    while (g_ascii_isalnum (*end) || *end == '_' || *end == '-')
      end++;
    if (end == start) {
      /* Lone $ with no var name following — emit literally. */
      g_string_append_c (out, '$');
      p++;
      continue;
    }
    gs_free char *name = g_strndup (start, end - start);
    gs_free char *val = sidebar_lookup_template_var (name, screen);
    if (val != nullptr) {
      g_string_append (out, val);
    } else {
      /* Unknown name: emit `$name` so the user sees their typo. */
      g_string_append_c (out, '$');
      g_string_append (out, name);
    }
    p = end;
  }
  return g_string_free (out, FALSE);
}

/* Compute the display string for a sidebar row given its screen.
 *
 * Precedence:
 *   1. If the screen carries a non-empty display-name template (set via
 *      Startup-tab Name field on the source entry), expand it. This wins
 *      unconditionally — the user opted into a custom label.
 *   2. VTE window title (OSC 0/2). What most shells emit at the prompt
 *      and what tools like `claude --name X` set explicitly.
 *   3. Basename of the current directory. `terminal_screen_get_current_dir`
 *      first tries OSC 7 (emitted by bash/zsh/fish with default prompts and
 *      remembered by VTE even after the shell exec'd something quiet like
 *      `top`), then falls back to the cwd that was passed to the spawn.
 *      For "~" the basename is "alice" or similar — fine, that's the home
 *      directory's actual name; we don't try to detect and re-collapse to ~.
 *   4. Literal "shell" so the row is never empty.
 *
 * Caller owns the returned string (g_free). */
static char *
sidebar_compute_row_label_text (TerminalScreen *screen)
{
  const char *tmpl = terminal_screen_get_display_name_template (screen);
  if (tmpl != nullptr && *tmpl != '\0')
    return sidebar_expand_name_template (tmpl, screen);

  const char *title = terminal_screen_get_title (screen);
  if (title != nullptr && *title != '\0')
    return g_strdup (title);

  gs_free char *cwd = terminal_screen_get_current_dir (screen);
  if (cwd != nullptr && *cwd != '\0') {
    char *base = g_path_get_basename (cwd);
    if (base != nullptr && *base != '\0' && !g_str_equal (base, "/"))
      return base;
    g_free (base);
  }

  return g_strdup (_("shell"));
}

static void
sidebar_row_label_update (TerminalScreen *screen, GtkLabel *label)
{
  gs_free char *text = sidebar_compute_row_label_text (screen);
  gtk_label_set_label (label, text);
}

/* "notify::title" handler — fires when VTE emits window-title-changed. */
static void
sidebar_row_label_notify_cb (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  sidebar_row_label_update (TERMINAL_SCREEN (obj), GTK_LABEL (user_data));
}

/* "current-directory-uri-changed" handler — fires on OSC 7. */
static void
sidebar_row_label_cwd_cb (VteTerminal *vte, gpointer user_data)
{
  sidebar_row_label_update (TERMINAL_SCREEN (vte), GTK_LABEL (user_data));
}

/* One-shot "contents-changed" listener. The initial label computed at row
 * creation may be just "shell" because the screen's exec_data (which holds
 * the spawn cwd) isn't populated yet — the DBus `exec` call lands after
 * create_instance returns. By the time the shell writes its first byte the
 * spawn has settled and the cwd fallback path will succeed; recompute and
 * disconnect so we're not paying a signal-handler tax on every redraw. */
static void
sidebar_row_label_first_output_cb (VteTerminal *vte, gpointer user_data)
{
  GtkLabel *label = GTK_LABEL (user_data);
  sidebar_row_label_update (TERMINAL_SCREEN (vte), label);
  g_signal_handlers_disconnect_by_func (vte,
                                        (gpointer) sidebar_row_label_first_output_cb,
                                        user_data);
}

/* Resolve the expanded-row icon widget for a given screen, honouring the
 * explicit-icon-first then auto-assign-hash precedence used at add_screen
 * time. Returns NULL when no icon should render — caller treats NULL as
 * "no icon column for this row". The returned widget is a floating ref;
 * pack into a container to consume. Centralised so the live icon-changed
 * handler stays in sync with the initial add_screen build. */
static GtkWidget *
sidebar_resolve_expanded_row_icon (TerminalSidebar *self, TerminalScreen *screen)
{
  TerminalSidebarPrivate *priv = self->priv;
  const char *icon_id = terminal_screen_get_icon (screen);
  gboolean has_icon = (icon_id != nullptr && icon_id[0] != '\0');
  gs_free char *auto_id = nullptr;
  if (!has_icon && priv->app_settings != nullptr &&
      g_settings_get_boolean (priv->app_settings,
                              TERMINAL_SETTING_AUTO_ASSIGN_ICON_KEY)) {
    const char *uuid = terminal_screen_get_uuid (screen);
    const char *auto_name = terminal_sidebar_icon_auto_assign (uuid);
    if (auto_name != nullptr) {
      auto_id = g_strdup_printf ("builtin:%s", auto_name);
      icon_id = auto_id;
      has_icon = TRUE;
    }
  }
  /* Per-node recolor (M13): paint a symbolic glyph in the node color when the
   * target includes the icon; custom image files keep their own pixels. */
  return has_icon
    ? terminal_sidebar_icon_resolve_colored (icon_id, 16, sidebar_screen_icon_tint (screen))
    : nullptr;
}

/* icon-changed handler: rebuild the expanded-row icon widget in place and
 * trigger a collapsed-cell rebuild. Called whenever set_icon mutates the
 * stored value — context menu picks, future smart-match assignments, etc.
 *
 * The expanded-row rebuild swaps the old GtkImage for a freshly resolved
 * one and reorders it back to the first position (left of the label). We
 * don't tear down the whole row because it owns drag-source registration,
 * the right-click handler, and other per-row state that's expensive to
 * re-establish. Collapsed cells are cheaper to rebuild wholesale and the
 * existing rebuild_collapsed_list helper handles all of them at once. */
/* Swap one expanded row's icon widget for a freshly-resolved one. Shared by
 * the per-screen icon-changed handler and the global recolor refresh so both
 * build the icon identically. Does NOT rebuild collapsed cells — callers do
 * that once after touching the rows they care about. */
static void
sidebar_refresh_row_icon (TerminalSidebar *self, TerminalScreen *screen)
{
  TerminalSidebarPrivate *priv = self->priv;

  GtkListBoxRow *row = GTK_LIST_BOX_ROW (g_hash_table_lookup (priv->screen_to_row, screen));
  if (row == nullptr)
    return;
  GtkBox *row_box = GTK_BOX (g_object_get_data (G_OBJECT (row), "axan-row-box"));
  if (row_box == nullptr)
    return;

  GtkWidget *old_icon = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "axan-row-icon"));
  if (old_icon != nullptr) {
    gtk_container_remove (GTK_CONTAINER (row_box), old_icon);
    g_object_set_data (G_OBJECT (row), "axan-row-icon", nullptr);
  }

  GtkWidget *new_icon = sidebar_resolve_expanded_row_icon (self, screen);
  if (new_icon != nullptr) {
    gtk_widget_set_no_show_all (new_icon, TRUE);
    gboolean show = priv->app_settings != nullptr &&
                    g_settings_get_boolean (priv->app_settings,
                                            TERMINAL_SETTING_SHOW_ICONS_IN_EXPANDED_KEY);
    gtk_widget_set_visible (new_icon, show);
    gtk_box_pack_start (row_box, new_icon, FALSE, FALSE, 0);
    gtk_box_reorder_child (row_box, new_icon, 0);
    g_object_set_data (G_OBJECT (row), "axan-row-icon", new_icon);
  }
}

/* Re-apply (or clear) the per-node text recolor on an expanded row's label.
 * Paired with sidebar_refresh_row_icon so an appearance change repaints both
 * surfaces. The label widget is stashed on the row at add_screen time. */
static void
sidebar_refresh_row_label_color (TerminalSidebar *self, TerminalScreen *screen)
{
  GtkListBoxRow *row = GTK_LIST_BOX_ROW (g_hash_table_lookup (self->priv->screen_to_row, screen));
  if (row == nullptr)
    return;
  GtkWidget *label = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "axan-row-label"));
  if (label == nullptr)
    return;
  sidebar_apply_label_color (label, sidebar_screen_text_tint (screen));
}

static void
sidebar_screen_icon_changed_cb (TerminalScreen *screen, gpointer user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  sidebar_refresh_row_icon (self, screen);
  sidebar_refresh_row_label_color (self, screen);
  sidebar_rebuild_collapsed_list (self);
}

/* GSettings "changed::recolor-icons" handler. Recoloring is applied at
 * pixbuf-load time inside the icon resolver, so flipping it requires
 * rebuilding the icon widgets: refresh every expanded row's icon, then
 * rebuild collapsed cells once. */
static void
sidebar_recolor_icons_changed_cb (GSettings *settings G_GNUC_UNUSED,
                                  const gchar *key G_GNUC_UNUSED,
                                  gpointer user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  GList *screens = g_hash_table_get_keys (self->priv->screen_to_row);
  for (GList *l = screens; l != nullptr; l = l->next)
    sidebar_refresh_row_icon (self, TERMINAL_SCREEN (l->data));
  g_list_free (screens);
  sidebar_rebuild_collapsed_list (self);
}

static void
terminal_sidebar_add_screen (TerminalMdiContainer *container,
                             TerminalScreen       *screen,
                             int                   position)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  TerminalSidebarPrivate *priv = self->priv;

  g_warn_if_fail (gtk_widget_get_parent (GTK_WIDGET (screen)) == nullptr);

  /* Wrap the screen in the same screen-container the rest of the codebase
   * expects (find-bar, etc.) and place it in the stack. */
  GtkWidget *screen_container = terminal_screen_container_new (screen);
  gtk_widget_show (screen_container);
  gtk_stack_add_named (GTK_STACK (priv->stack), screen_container, stack_name_for_screen (screen));

  /* Build the row. A single label whose text follows the screen's
   * window title (OSC 0/2) with fallbacks — see sidebar_row_label_update
   * for the precedence chain. max_width_chars caps the label's *natural*
   * width so a long shell title does not inflate the sidebar (and
   * therefore the window) on first map. */
  GtkWidget *label = gtk_label_new (nullptr);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 24);
  sidebar_row_label_update (screen, GTK_LABEL (label));
  g_signal_connect_object (screen, "notify::title",
                           G_CALLBACK (sidebar_row_label_notify_cb),
                           label, GConnectFlags (0));
  /* Template changes recompute too — if the runtime ever offers a way to
   * change the template after spawn (rename-row UI, etc.) this keeps the
   * label in sync. Templates that reference $title or $pwd react via the
   * notify::title and current-directory-uri-changed handlers, but the
   * template ITSELF changing is a separate axis. */
  g_signal_connect_object (screen, "notify::display-name-template",
                           G_CALLBACK (sidebar_row_label_notify_cb),
                           label, GConnectFlags (0));
  /* OSC 7 changes don't fire notify::title on TerminalScreen — listen to
   * VTE directly so a shell switching directories before running a quiet
   * tool (e.g. `cd somewhere && top`) still updates the sidebar label. */
  g_signal_connect_object (screen, "current-directory-uri-changed",
                           G_CALLBACK (sidebar_row_label_cwd_cb),
                           label, GConnectFlags (0));
  /* One-shot: when the spawn's first output arrives, exec_data is set —
   * recompute so the cwd fallback can replace a transient "shell" with the
   * real directory name. */
  g_signal_connect_object (screen, "contents-changed",
                           G_CALLBACK (sidebar_row_label_first_output_cb),
                           label, GConnectFlags (0));
  /* Live icon updates — context menu picks fire icon-changed; the handler
   * rebuilds this row's icon widget and refreshes collapsed cells. Connect
   * to `self` (sidebar) rather than the label, since icon repaint affects
   * multiple widgets and needs the full sidebar state. */
  g_signal_connect_object (screen, "icon-changed",
                           G_CALLBACK (sidebar_screen_icon_changed_cb),
                           self, GConnectFlags (0));

  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start (row_box, 8);
  gtk_widget_set_margin_end (row_box, 8);
  gtk_widget_set_margin_top (row_box, 4);
  gtk_widget_set_margin_bottom (row_box, 4);

  /* Optional row icon. Resolved the same way as the collapsed cell —
   * explicit-icon first, then auto-assign hash if the auto-assign toggle
   * is on. We always build the widget (when there is an icon to render)
   * but gate its visibility on show-icons-in-expanded. A change-signal
   * handler later in init flips visible flags across all live rows so
   * the toggle takes effect without rebuilding rows.
   *
   * No_show_all stops gtk_widget_show_all on the row from clobbering
   * our explicit hidden state. The visibility-sync helper still drives
   * the icon explicitly. */
  GtkWidget *row_icon = sidebar_resolve_expanded_row_icon (self, screen);
  if (row_icon != nullptr) {
    gtk_widget_set_no_show_all (row_icon, TRUE);
    gboolean show = priv->app_settings != nullptr &&
                    g_settings_get_boolean (priv->app_settings,
                                            TERMINAL_SETTING_SHOW_ICONS_IN_EXPANDED_KEY);
    gtk_widget_set_visible (row_icon, show);
    gtk_box_pack_start (GTK_BOX (row_box), row_icon, FALSE, FALSE, 0);
    /* Stash on the row happens once the row widget exists; see below. */
  }
  /* Per-node text recolor (M13): tint the label when the node carries a color
   * targeting text. Applied here for the initial paint; the icon-changed
   * handler re-applies it on later edits. */
  sidebar_apply_label_color (label, sidebar_screen_text_tint (screen));
  gtk_box_pack_start (GTK_BOX (row_box), label, TRUE, TRUE, 0);

  /* Wrap the content in an event box to give the drag source something with
   * a GdkWindow to attach to. visible-window=FALSE keeps the listbox's own
   * selection chrome painted through unchanged. */
  GtkWidget *event_box = gtk_event_box_new ();
  gtk_event_box_set_visible_window (GTK_EVENT_BOX (event_box), FALSE);
  gtk_container_add (GTK_CONTAINER (event_box), row_box);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_container_add (GTK_CONTAINER (row), event_box);
  g_object_set_data (G_OBJECT (row), "axan-screen", screen);
  /* Stash row_box on the row so row_apply_indent can update margin-start
   * without walking through the event-box wrapper each time. */
  g_object_set_data (G_OBJECT (row), "axan-row-box", row_box);
  /* Stash the label so the icon-changed handler can re-apply the per-node
   * text recolor without walking the box children. */
  g_object_set_data (G_OBJECT (row), "axan-row-label", label);
  /* Stash the optional icon widget so the show-icons-in-expanded handler
   * can flip visibility without re-walking the box children. NULL is a
   * valid stash value (g_object_set_data tolerates it) — handler does
   * a non-null check anyway. */
  if (row_icon != nullptr)
    g_object_set_data (G_OBJECT (row), "axan-row-icon", row_icon);
  row_apply_indent (GTK_LIST_BOX_ROW (row));
  gtk_widget_show_all (row);

  /* Drag source: button-1 drag on the row body initiates a move. The row
   * pointer travels as user_data so drag-begin and drag-data-get can find it
   * without consulting priv. The destination side lives on the listbox; see
   * terminal_sidebar_init. */
  gtk_drag_source_set (event_box,
                       GDK_BUTTON1_MASK,
                       sidebar_dnd_targets,
                       G_N_ELEMENTS (sidebar_dnd_targets),
                       GDK_ACTION_MOVE);
  g_signal_connect (event_box, "drag-begin",
                    G_CALLBACK (on_row_drag_begin), row);
  g_signal_connect (event_box, "drag-data-get",
                    G_CALLBACK (on_row_drag_data_get), row);

  /* Right-click context menu — handled per-row so the row pointer is
   * captured directly, sidestepping the y-coordinate translation that
   * made the listbox-level handler close the wrong shell. user_data is
   * the row; the handler reads the screen back from row data. */
  g_signal_connect (event_box, "button-press-event",
                    G_CALLBACK (on_row_button_press), row);

  int insert_pos = (position < 0) ? -1 : position;
  gtk_list_box_insert (GTK_LIST_BOX (priv->list_box), row, insert_pos);

  g_hash_table_insert (priv->screen_to_row, screen, row);
  sidebar_update_count (self);
  sidebar_rebuild_collapsed_list (self);

  /* Make the new screen the stack's visible child *now*, before anyone
   * queries the widget tree for its natural size. GtkNotebook auto-selected
   * its first inserted page; GtkStack does not, and that left the right pane
   * reporting zero width at startup. Doing this here also stabilizes the
   * old/new pair we hand to screen-switched below. */
  if (priv->active_screen == nullptr) {
    priv->syncing_selection = TRUE;
    gtk_stack_set_visible_child_name (GTK_STACK (priv->stack), stack_name_for_screen (screen));
    gtk_list_box_select_row (GTK_LIST_BOX (priv->list_box), GTK_LIST_BOX_ROW (row));
    priv->syncing_selection = FALSE;

    TerminalScreen *old = priv->active_screen;
    priv->active_screen = screen;
    g_signal_emit_by_name (container, "screen-added", screen);
    g_signal_emit_by_name (container, "screen-switched", old, screen);
  } else {
    g_signal_emit_by_name (container, "screen-added", screen);
  }
}

static void
terminal_sidebar_remove_screen (TerminalMdiContainer *container,
                                TerminalScreen       *screen)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  TerminalSidebarPrivate *priv = self->priv;

  GtkListBoxRow *row = sidebar_lookup_row (self, screen);
  if (!row)
    return;

  TerminalScreenContainer *sc = terminal_screen_container_get_from_screen (screen);
  if (sc)
    gtk_container_remove (GTK_CONTAINER (priv->stack), GTK_WIDGET (sc));

  /* Hoist children one level: when a parent shell exits, its direct children
   * are reparented to its parent (grandparent of the children) rather than
   * orphaned to root or removed. This preserves the user's intent — they
   * grouped these shells under something for a reason, and the next-nearest
   * grouping is the least surprising fallback. Then re-apply indent across
   * the affected subtrees. */
  GtkListBox *list_box = GTK_LIST_BOX (priv->list_box);
  GtkListBoxRow *new_parent_for_children = row_get_parent (row);
  int row_index = gtk_list_box_row_get_index (row);
  int subtree_end = subtree_end_index (list_box, row_index);

  for (int i = row_index + 1; i < subtree_end; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    if (r != nullptr && row_get_parent (r) == row)
      row_set_parent (r, new_parent_for_children);
  }

  gtk_container_remove (GTK_CONTAINER (priv->list_box), GTK_WIDGET (row));
  g_hash_table_remove (priv->screen_to_row, screen);

  /* The remaining rows in what was the source subtree need their indent
   * recomputed — direct children went up one level, but their descendants
   * are now also one level shallower in the parent chain. After the remove,
   * those rows now occupy indices [row_index, subtree_end - 1). */
  for (int i = row_index; i < subtree_end - 1; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    if (r != nullptr)
      row_apply_indent (r);
  }

  sidebar_update_count (self);
  sidebar_rebuild_collapsed_list (self);

  if (priv->active_screen == screen)
    priv->active_screen = nullptr;

  g_signal_emit_by_name (container, "screen-removed", screen);
}

static TerminalScreen *
terminal_sidebar_get_active_screen (TerminalMdiContainer *container)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  return self->priv->active_screen;
}

static void
terminal_sidebar_set_active_screen (TerminalMdiContainer *container,
                                    TerminalScreen       *screen)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  GtkListBoxRow *row = sidebar_lookup_row (self, screen);
  if (row)
    gtk_list_box_select_row (GTK_LIST_BOX (self->priv->list_box), row);
}

static GList *
terminal_sidebar_list_screen_containers (TerminalMdiContainer *container)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  GList *rows = gtk_container_get_children (GTK_CONTAINER (self->priv->list_box));
  for (GList *l = rows; l != nullptr; l = l->next) {
    TerminalScreen *screen = screen_from_row (GTK_LIST_BOX_ROW (l->data));
    l->data = terminal_screen_container_get_from_screen (screen);
  }
  return rows;
}

static GList *
terminal_sidebar_list_screens (TerminalMdiContainer *container)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  GList *rows = gtk_container_get_children (GTK_CONTAINER (self->priv->list_box));
  for (GList *l = rows; l != nullptr; l = l->next)
    l->data = screen_from_row (GTK_LIST_BOX_ROW (l->data));
  return rows;
}

static int
terminal_sidebar_get_n_screens (TerminalMdiContainer *container)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  return g_hash_table_size (self->priv->screen_to_row);
}

static int
terminal_sidebar_get_active_screen_num (TerminalMdiContainer *container)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self->priv->list_box));
  if (!row)
    return -1;
  return gtk_list_box_row_get_index (row);
}

static void
terminal_sidebar_set_active_screen_num (TerminalMdiContainer *container,
                                        int                   position)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->priv->list_box), position);
  if (row)
    gtk_list_box_select_row (GTK_LIST_BOX (self->priv->list_box), row);
}

static void
terminal_sidebar_reorder_screen (TerminalMdiContainer *container,
                                 TerminalScreen       *screen,
                                 int                   direction)
{
  /* Window code calls this with direction == +1 or -1 for "move active
   * tab one slot left/right." Implement by removing + reinserting the row. */
  TerminalSidebar *self = TERMINAL_SIDEBAR (container);
  TerminalSidebarPrivate *priv = self->priv;

  g_return_if_fail (direction == 1 || direction == -1);

  GtkListBoxRow *row = sidebar_lookup_row (self, screen);
  if (!row)
    return;

  int n = (int) g_hash_table_size (priv->screen_to_row);
  int pos = gtk_list_box_row_get_index (row);
  int new_pos = pos + direction;
  if (new_pos < 0)        new_pos = n - 1;
  else if (new_pos >= n)  new_pos = 0;

  g_object_ref (row);
  gtk_container_remove (GTK_CONTAINER (priv->list_box), GTK_WIDGET (row));
  gtk_list_box_insert (GTK_LIST_BOX (priv->list_box), GTK_WIDGET (row), new_pos);
  g_object_unref (row);

  gtk_list_box_select_row (GTK_LIST_BOX (priv->list_box), row);
  sidebar_rebuild_collapsed_list (self);
  g_signal_emit_by_name (container, "screens-reordered");
}

static void
terminal_sidebar_mdi_iface_init (TerminalMdiContainerInterface *iface)
{
  iface->add_screen              = terminal_sidebar_add_screen;
  iface->remove_screen           = terminal_sidebar_remove_screen;
  iface->get_active_screen       = terminal_sidebar_get_active_screen;
  iface->set_active_screen       = terminal_sidebar_set_active_screen;
  iface->list_screens            = terminal_sidebar_list_screens;
  iface->list_screen_containers  = terminal_sidebar_list_screen_containers;
  iface->get_n_screens           = terminal_sidebar_get_n_screens;
  iface->get_active_screen_num   = terminal_sidebar_get_active_screen_num;
  iface->set_active_screen_num   = terminal_sidebar_set_active_screen_num;
  iface->reorder_screen          = terminal_sidebar_reorder_screen;
}

/* ----- signals ----------------------------------------------------------- */

/* Idle-deferred focus grab for click-to-select (see on_row_selected). The
 * screen is ref'd by the scheduler; a row closed between scheduling and
 * dispatch leaves the widget unmapped, in which case grabbing would be at
 * best a no-op and at worst focus a doomed widget, so check first. */
static gboolean
sidebar_focus_screen_idle_cb (gpointer data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (data);
  if (gtk_widget_get_mapped (GTK_WIDGET (screen)))
    gtk_widget_grab_focus (GTK_WIDGET (screen));
  g_object_unref (screen);
  return G_SOURCE_REMOVE;
}

static void
on_row_selected (GtkListBox    *list_box G_GNUC_UNUSED,
                 GtkListBoxRow *row,
                 gpointer       user_data)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (user_data);
  TerminalSidebarPrivate *priv = self->priv;

  if (priv->syncing_selection)
    return;

  TerminalScreen *new_screen = screen_from_row (row);
  TerminalScreen *old_screen = priv->active_screen;

  if (new_screen == old_screen)
    return;

  if (new_screen)
    gtk_stack_set_visible_child_name (GTK_STACK (priv->stack), stack_name_for_screen (new_screen));

  priv->active_screen = new_screen;
  /* Refresh the minimized column so the active-cell highlight follows
   * selection. Cheap (cell count is tiny) and only does work when the
   * minimized view is actually visible — see the early-out in the
   * rebuild routine. */
  sidebar_rebuild_collapsed_list (self);
  g_signal_emit_by_name (self, "screen-switched", old_screen, new_screen);

  /* Click-to-select should land keyboard focus in the terminal so the user
   * can type immediately — TerminalNotebook gets this for free by routing
   * grab_focus to the active screen, but the sidebar's GtkListBox grabs
   * focus onto the clicked *row* AFTER emitting row-selected (its cursor
   * update runs post-selection), so a synchronous grab here loses the race.
   * Defer to an idle, which runs after the listbox finishes the click.
   *
   * Gate on a pointer event so this only fires for clicks (row list and
   * minimized-cell buttons both reach here with the button event current) —
   * arrow-key navigation through the tree must keep focus in the sidebar,
   * or the user could only ever move one row before focus jumped away. */
  if (new_screen != nullptr) {
    GdkEvent *ev = gtk_get_current_event ();
    gboolean from_pointer = ev != nullptr &&
      (ev->type == GDK_BUTTON_PRESS   || ev->type == GDK_2BUTTON_PRESS ||
       ev->type == GDK_BUTTON_RELEASE ||
       ev->type == GDK_TOUCH_BEGIN    || ev->type == GDK_TOUCH_END);
    if (ev != nullptr)
      gdk_event_free (ev);
    if (from_pointer)
      g_idle_add (sidebar_focus_screen_idle_cb, g_object_ref (new_screen));
  }
}

/* ----- ctor / dtor ------------------------------------------------------- */

static void
terminal_sidebar_init (TerminalSidebar *self)
{
  TerminalSidebarPrivate *priv = (TerminalSidebarPrivate *) terminal_sidebar_get_instance_private (self);
  self->priv = priv;

  priv->screen_to_row = g_hash_table_new (g_direct_hash, g_direct_equal);
  priv->cell_to_screen = g_hash_table_new (g_direct_hash, g_direct_equal);
  priv->last_expanded_width = SIDEBAR_DEFAULT_WIDTH;
  /* Default collapsed_size to small (the floor) before any widgets are
   * built. The stored preference is loaded later in this same init,
   * but the column widgets need a valid width *at construction*. */
  priv->collapsed_size = 0; /* small */
  priv->state = TERMINAL_SIDEBAR_STATE_EXPANDED;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);

  /* The left pane holds a GtkStack with three swappable views — full row
   * list, minimized initial-tabs, collapsed hint strip. Keeping all three in
   * the tree simultaneously avoids re-parenting the listbox (which would
   * disconnect drag/drop targets and signal handlers) every time the user
   * cycles state. The view_stack just toggles visibility. */
  priv->view_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (priv->view_stack), GTK_STACK_TRANSITION_TYPE_NONE);
  /* GtkStack defaults to hhomogeneous=TRUE, which sizes the stack to the
   * widest child — so even when the 26px collapsed view is visible, the
   * stack reports the expanded view's ~320px as its minimum width and
   * the GtkPaned refuses to clamp below that. Turning hhomogeneous off
   * lets each visible page report its own width independently. Same
   * reasoning for vhomogeneous, though height contention is less likely
   * to bite. */
  gtk_stack_set_hhomogeneous (GTK_STACK (priv->view_stack), FALSE);
  gtk_stack_set_vhomogeneous (GTK_STACK (priv->view_stack), FALSE);

  /* ----- Expanded view (page "expanded") --------------------------------- */
  GtkWidget *left_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Toolbar. Per the v3 brief: count pill, left-aligned, whitespace to the
   * right, chevron cycle button on the right. */
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start (toolbar, 8);
  gtk_widget_set_margin_end (toolbar, 4);
  gtk_widget_set_margin_top (toolbar, 6);
  gtk_widget_set_margin_bottom (toolbar, 6);
  priv->count_label = gtk_label_new (nullptr);
  gtk_label_set_xalign (GTK_LABEL (priv->count_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (priv->count_label),
                               "axan-sidebar-count-pill");
  gtk_box_pack_start (GTK_BOX (toolbar), priv->count_label, FALSE, FALSE, 0);

  /* Cycle chevron. Single-arrow icon, no chrome — matches the design's `›`
   * affordance. Click cycles expanded → minimized → collapsed → expanded. */
  GtkWidget *cycle_btn = gtk_button_new_from_icon_name ("pan-start-symbolic",
                                                        GTK_ICON_SIZE_BUTTON);
  gtk_button_set_relief (GTK_BUTTON (cycle_btn), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (cycle_btn, FALSE);
  gtk_widget_set_tooltip_text (cycle_btn, _("Cycle sidebar (Ctrl+Shift+B)"));
  gtk_style_context_add_class (gtk_widget_get_style_context (cycle_btn),
                               "axan-sidebar-cycle");
  g_signal_connect (cycle_btn, "clicked",
                    G_CALLBACK (sidebar_cycle_button_clicked_cb), self);
  gtk_box_pack_end (GTK_BOX (toolbar), cycle_btn, FALSE, FALSE, 0);

  GtkWidget *scrolled = gtk_scrolled_window_new (nullptr, nullptr);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (scrolled), 320);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scrolled), FALSE);
  priv->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (priv->list_box), GTK_SELECTION_BROWSE);
  gtk_container_add (GTK_CONTAINER (scrolled), priv->list_box);

  gtk_box_pack_start (GTK_BOX (left_box), toolbar, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (left_box), scrolled, TRUE, TRUE, 0);
  gtk_widget_show_all (left_box);
  gtk_stack_add_named (GTK_STACK (priv->view_stack), left_box, "expanded");

  /* ----- Collapsed view (page "collapsed") ------------------------------- */
  /* Narrow column of initial-tab cells. Width comes from the
   * collapsed-size enum step (small=26/medium=44/large=72); to actually
   * hit that width the scrolled window's min content tracks too, and CSS
   * zeros button padding so GTK's default theme padding doesn't inflate
   * past the requested width. */
  GtkWidget *coll_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  int init_w = sidebar_collapsed_size_to_width (priv->collapsed_size);
  /* See apply_collapsed_size for why coll_box has no size_request.
   * init_w is still used below to seed the scrolled-window's
   * min_content_width — set once at init, not re-applied on every
   * apply_collapsed_size, so it's not on the wiggle-prone runtime path. */
  gtk_style_context_add_class (gtk_widget_get_style_context (coll_box),
                               "axan-sidebar-collapsed");
  priv->coll_box = coll_box;

  /* Cycle chevron at the top of the collapsed column. Points left —
   * the next state in the cycle is more collapsed (the minimized strip),
   * so the arrow indicates the direction the click sends the sidebar.
   * The headerbar already provides a new-shell button, so we drop the
   * bottom "+" entirely. */
  GtkWidget *coll_head = gtk_button_new_from_icon_name ("pan-start-symbolic",
                                                        GTK_ICON_SIZE_MENU);
  gtk_button_set_relief (GTK_BUTTON (coll_head), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (coll_head, FALSE);
  gtk_widget_set_tooltip_text (coll_head, _("Cycle sidebar (Ctrl+Shift+B)"));
  /* See apply_collapsed_size for why no size_request. The chevron's
   * natural button width is close to the small step (~26px); larger
   * steps will show the chevron narrower than the cells below it —
   * acceptable visual quirk pending a CSS-based width control. */
  gtk_style_context_add_class (gtk_widget_get_style_context (coll_head),
                               "axan-collapsed-head");
  g_signal_connect (coll_head, "clicked",
                    G_CALLBACK (sidebar_cycle_button_clicked_cb), self);
  priv->coll_head = coll_head;

  GtkWidget *coll_scrolled = gtk_scrolled_window_new (nullptr, nullptr);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (coll_scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  /* min_content_width has to be at least the cell width — with 0 plus
   * propagate-natural-width=FALSE, the scrolled window collapsed to 0px
   * and the cells inside got hidden. The value updates with the configured
   * step via sidebar_apply_collapsed_size. */
  gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (coll_scrolled),
                                             init_w - 6);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (coll_scrolled), FALSE);
  priv->coll_scrolled = coll_scrolled;
  priv->collapsed_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign (priv->collapsed_list, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (priv->collapsed_list, 2);
  gtk_widget_set_margin_bottom (priv->collapsed_list, 4);
  gtk_container_add (GTK_CONTAINER (coll_scrolled), priv->collapsed_list);

  gtk_box_pack_start (GTK_BOX (coll_box), coll_head,     FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (coll_box), coll_scrolled, TRUE,  TRUE,  0);
  gtk_widget_show_all (coll_box);
  gtk_stack_add_named (GTK_STACK (priv->view_stack), coll_box, "collapsed");

  /* ----- Minimized view (page "minimized") ------------------------------- */
  /* Thin strip showing only the expand chevron. 14px wide so a small icon
   * fits without inflating the strip; CSS zeros padding around the icon
   * so the actual rendered width matches the requested size. Clicking
   * jumps straight back to expanded — that's the recovery move from
   * this state. */
  GtkWidget *min_btn = gtk_button_new_from_icon_name ("pan-end-symbolic",
                                                      GTK_ICON_SIZE_MENU);
  gtk_button_set_relief (GTK_BUTTON (min_btn), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (min_btn, FALSE);
  gtk_widget_set_tooltip_text (min_btn, _("Expand sidebar (Ctrl+Shift+B)"));
  gtk_widget_set_size_request (min_btn, SIDEBAR_MINIMIZED_WIDTH, -1);
  gtk_style_context_add_class (gtk_widget_get_style_context (min_btn),
                               "axan-sidebar-minimized");
  g_signal_connect (min_btn, "clicked",
                    G_CALLBACK (sidebar_minimized_button_clicked_cb), self);
  gtk_widget_show (min_btn);
  gtk_stack_add_named (GTK_STACK (priv->view_stack), min_btn, "minimized");

  gtk_widget_show (priv->view_stack);
  gtk_stack_set_visible_child_name (GTK_STACK (priv->view_stack), "expanded");

  /* Right pane: stack of screen containers. Natural-size propagation
   * happens at the screen-container layer (it sets
   * propagate-natural-width on its inner GtkScrolledWindow), so the
   * stack just forwards its visible child's size — no size_request
   * floor needed here, the user can shrink the window freely after
   * launch. */
  priv->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (priv->stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_show (priv->stack);

  gtk_paned_pack1 (GTK_PANED (self), priv->view_stack, FALSE, FALSE);
  gtk_paned_pack2 (GTK_PANED (self), priv->stack, TRUE, FALSE);
  gtk_paned_set_position (GTK_PANED (self), SIDEBAR_DEFAULT_WIDTH);


  /* Persistence. Two keys drive the initial state:
   *
   *   startup-sidebar-state — what state to open in. `last-used` (the
   *     default) means "honor sidebar-state"; any other value pins
   *     every new window to that explicit state regardless of how
   *     sidebar-state has drifted via the cycle accelerator.
   *
   *   sidebar-state — the live cycle value. Persists across windows
   *     so cycling the chevron in one window changes the others to
   *     match. Also the fallback when startup-sidebar-state is
   *     last-used.
   *
   * The 'changed' signal on sidebar-state propagates external writes
   * (gsettings set, second-window cycle) into this sidebar — adjusting
   * the key from the command line behaves the same as cycling from
   * inside axan. */
  /* Share TerminalApp's global settings instance. Creating a separate
   * GSettings on the default backend caused prefs writes (which go
   * through TerminalApp's custom backend) to silently not notify the
   * sidebar — same key, different bus. Ref so we own a strong handle
   * for the lifetime of the sidebar. */
  priv->app_settings = G_SETTINGS (g_object_ref (terminal_app_get_global_settings (terminal_app_get ())));
  /* Read collapsed size step and push it into the column widgets so a
   * non-default stored preference takes effect on the first paint —
   * the widgets were built earlier with the small (default) step
   * baked into their size_request. */
  priv->collapsed_size = g_settings_get_enum (priv->app_settings,
                                              TERMINAL_SETTING_COLLAPSED_SIZE_KEY);
  sidebar_apply_collapsed_size (self);

  int startup_pref = g_settings_get_enum (priv->app_settings,
                                          TERMINAL_SETTING_STARTUP_SIDEBAR_STATE_KEY);
  int initial_state;
  if (startup_pref == 0) {
    /* last-used: honor the live sidebar-state */
    initial_state = g_settings_get_enum (priv->app_settings, SIDEBAR_GSETTINGS_KEY);
  } else {
    /* Pinned states are offset by one in the StartupSidebarState enum
     * (0=last-used, 1=expanded, 2=collapsed, 3=minimized) but in
     * SidebarState (0=expanded, 1=collapsed, 2=minimized), so subtract
     * one to land in the right ordinal. */
    initial_state = startup_pref - 1;
  }
  if (initial_state == TERMINAL_SIDEBAR_STATE_MINIMIZED ||
      initial_state == TERMINAL_SIDEBAR_STATE_COLLAPSED) {
    sidebar_apply_state (self, (TerminalSidebarState) initial_state);
  }

  /* React to external collapsed-size changes (prefs combobox, gsettings
   * set) by re-applying the column width and rebuilding cells. */
  g_signal_connect (priv->app_settings, "changed::collapsed-size",
                    G_CALLBACK (sidebar_collapsed_size_changed_cb), self);
  /* Auto-assign-icon toggle: cells use auto-assign at render time, so a
   * rebuild is all we need to flip every cell over (or back). Reuse the
   * existing collapsed-size handler signature by ignoring the key —
   * sidebar_rebuild_collapsed_list short-circuits when not in collapsed
   * state, so off-state toggles cost nothing. */
  g_signal_connect_swapped (priv->app_settings,
                            "changed::" TERMINAL_SETTING_AUTO_ASSIGN_ICON_KEY,
                            G_CALLBACK (sidebar_rebuild_collapsed_list), self);
  /* show-icons-in-expanded toggle: walks live rows and flips visibility
   * on each stashed icon widget. No row rebuild — the icon widgets were
   * built eagerly at row-creation. */
  g_signal_connect (priv->app_settings,
                    "changed::" TERMINAL_SETTING_SHOW_ICONS_IN_EXPANDED_KEY,
                    G_CALLBACK (sidebar_show_icons_in_expanded_changed_cb), self);
  /* recolor-icons toggle: rebuilds icon widgets (recolor is applied at
   * load time in the resolver) across expanded rows and collapsed cells. */
  g_signal_connect (priv->app_settings,
                    "changed::" TERMINAL_SETTING_RECOLOR_ICONS_KEY,
                    G_CALLBACK (sidebar_recolor_icons_changed_cb), self);
  /* React to external changes (gsettings set, second-window cycle) by
   * mirroring the stored value into this sidebar instance. The
   * notify-self check in sidebar_apply_state short-circuits if the
   * value is already current, so writes from this sidebar don't bounce. */
  g_signal_connect (priv->app_settings, "changed::" SIDEBAR_GSETTINGS_KEY,
                    G_CALLBACK (sidebar_settings_changed_cb), self);

  g_signal_connect (priv->list_box, "row-selected",
                    G_CALLBACK (on_row_selected), self);
  g_signal_connect (priv->list_box, "button-press-event",
                    G_CALLBACK (on_listbox_button_press), self);

  /* Drag destination for row reorder. GTK_DEST_DEFAULT_ALL handles the
   * boilerplate (status replies, target check) so we only see drag-motion,
   * drag-leave, and drag-data-received for actual same-app row drops. */
  gtk_drag_dest_set (priv->list_box,
                     GTK_DEST_DEFAULT_ALL,
                     sidebar_dnd_targets,
                     G_N_ELEMENTS (sidebar_dnd_targets),
                     GDK_ACTION_MOVE);
  g_signal_connect (priv->list_box, "drag-motion",
                    G_CALLBACK (on_listbox_drag_motion), self);
  g_signal_connect (priv->list_box, "drag-leave",
                    G_CALLBACK (on_listbox_drag_leave), self);
  g_signal_connect (priv->list_box, "drag-data-received",
                    G_CALLBACK (on_listbox_drag_data_received), self);

  sidebar_update_count (self);
}

static void
terminal_sidebar_finalize (GObject *object)
{
  TerminalSidebar *self = TERMINAL_SIDEBAR (object);
  g_clear_pointer (&self->priv->screen_to_row, g_hash_table_destroy);
  g_clear_pointer (&self->priv->cell_to_screen, g_hash_table_destroy);
  g_clear_object (&self->priv->app_settings);
  G_OBJECT_CLASS (terminal_sidebar_parent_class)->finalize (object);
}

static void
terminal_sidebar_class_init (TerminalSidebarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = terminal_sidebar_finalize;
}

GtkWidget *
terminal_sidebar_new (void)
{
  return GTK_WIDGET (g_object_new (TERMINAL_TYPE_SIDEBAR, nullptr));
}

/* Pre-order walk of the listbox, emitting one (id, parent_id, name, dir, cmd)
 * tuple per row. Fresh UUIDs are generated for ids — preferences treats this
 * as a brand-new set of entries replacing whatever was there. parent_id
 * references the *generated* id of the parent row (looked up via the
 * row→uuid map we build during the walk), so the hierarchy is captured
 * faithfully without sharing identity with whatever Startup-entry originally
 * spawned the row. */
GVariant *
terminal_sidebar_capture_entries (TerminalSidebar *self)
{
  g_return_val_if_fail (TERMINAL_IS_SIDEBAR (self), nullptr);
  TerminalSidebarPrivate *priv = self->priv;

  GVariantBuilder vb;
  g_variant_builder_init (&vb, G_VARIANT_TYPE ("a(ssssssss)"));

  /* row pointer → newly-minted UUID. Used so that the second row referencing
   * the first as parent emits the first row's *generated* id, not the
   * screen's runtime identity. */
  GHashTable *row_to_uuid = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                   nullptr, g_free);

  GList *children = gtk_container_get_children (GTK_CONTAINER (priv->list_box));
  /* First pass: mint UUIDs so we can resolve parent references on the
   * second pass (the parent appears before its child in pre-order, but
   * generating UUIDs separately keeps the emit loop simple). */
  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (l->data);
    if (screen_from_row (r) == nullptr)
      continue;
    g_hash_table_insert (row_to_uuid, r, g_uuid_string_random ());
  }

  for (GList *l = children; l != nullptr; l = l->next) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (l->data);
    TerminalScreen *screen = screen_from_row (r);
    if (screen == nullptr)
      continue;

    const char *my_id = (const char *) g_hash_table_lookup (row_to_uuid, r);
    GtkListBoxRow *parent_row = row_get_parent (r);
    const char *parent_id = "";
    if (parent_row != nullptr) {
      const char *pid = (const char *) g_hash_table_lookup (row_to_uuid, parent_row);
      if (pid != nullptr)
        parent_id = pid;
    }

    const char *name = terminal_screen_get_display_name_template (screen);
    if (name == nullptr) name = "";

    gs_free char *cwd = terminal_screen_get_current_dir (screen);
    const char *dir = cwd ? cwd : "";

    /* Command intentionally empty — see header comment. Icon, color, and
     * color_target empty for the same reason: captured rows record structure
     * (name + hierarchy + cwd), not per-shell appearance metadata, which
     * lives in the profile's declared default-launch-entries (set in the
     * Startup editor or the TOML), not in the live row state. */
    g_variant_builder_add (&vb, "(ssssssss)", my_id, parent_id, name, dir, "", "", "", "");
  }
  g_list_free (children);
  g_hash_table_destroy (row_to_uuid);

  return g_variant_builder_end (&vb);
}

/* Public: reparent `child`'s row under `parent`'s row in the sidebar tree.
 * Used by the startup-hierarchy path (terminal-gdbus.cc) so each freshly-
 * spawned screen, when it was declared as a child in default-launch-entries,
 * lands in the right place in the tree.
 *
 * Mirrors the post-drop work done in on_listbox_drag_data_received: update
 * the parent pointer, move the subtree to sit right after the parent's
 * existing descendants (preserving the pre-order invariant), and re-apply
 * indent so depth-driven margins update.
 *
 * Returns FALSE if either screen is not in this sidebar or if applying the
 * reparent would form a cycle (parent is a descendant of child). FALSE is
 * also returned for trivially-malformed input (self-parent, null screens).
 */
gboolean
terminal_sidebar_set_screen_parent (TerminalSidebar *self,
                                    struct _TerminalScreen *child,
                                    struct _TerminalScreen *parent)
{
  g_return_val_if_fail (TERMINAL_IS_SIDEBAR (self), FALSE);
  if (child == nullptr || (TerminalScreen *) child == (TerminalScreen *) parent)
    return FALSE;

  GtkListBoxRow *child_row = sidebar_lookup_row (self, (TerminalScreen *) child);
  if (child_row == nullptr)
    return FALSE;

  GtkListBoxRow *parent_row = nullptr;
  if (parent != nullptr) {
    parent_row = sidebar_lookup_row (self, (TerminalScreen *) parent);
    if (parent_row == nullptr)
      return FALSE;
    /* Cycle guard: parent must not be in child's subtree. */
    if (row_is_descendant_of (parent_row, child_row) || parent_row == child_row)
      return FALSE;
  }

  GtkListBox *list_box = GTK_LIST_BOX (gtk_widget_get_parent (GTK_WIDGET (child_row)));
  if (list_box == nullptr)
    return FALSE;

  /* Compute destination index: right after the parent's last descendant in
   * pre-order, or end-of-list if parent is null (child becomes a root). */
  int source_start = gtk_list_box_row_get_index (child_row);
  int source_end = subtree_end_index (list_box, source_start);
  int subtree_size = source_end - source_start;

  int insert_index;
  if (parent_row == nullptr) {
    insert_index = -1;  /* append to end (root) */
  } else {
    int parent_index = gtk_list_box_row_get_index (parent_row);
    insert_index = subtree_end_index (list_box, parent_index);
    /* If subtree precedes the insertion point, removing it shifts the
     * insertion index left by subtree_size. */
    if (source_start < insert_index)
      insert_index -= subtree_size;
  }

  /* Collect, remove, reparent root, re-insert, re-indent. Identical mechanics
   * to the drop handler — kept inline rather than refactored into a shared
   * helper because the drop handler has additional drop-zone math we don't
   * need here. If a third caller appears, factor it out. */
  GPtrArray *moving = g_ptr_array_new_full (subtree_size, nullptr);
  for (int i = source_start; i < source_end; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    g_object_ref (r);
    g_ptr_array_add (moving, r);
  }
  for (int i = source_end - 1; i >= source_start; i--) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (list_box, i);
    gtk_container_remove (GTK_CONTAINER (list_box), GTK_WIDGET (r));
  }

  row_set_parent (child_row, parent_row);

  int dest = (insert_index < 0) ? -1 : insert_index;
  for (guint i = 0; i < moving->len; i++) {
    GtkListBoxRow *r = GTK_LIST_BOX_ROW (g_ptr_array_index (moving, i));
    gtk_list_box_insert (list_box, GTK_WIDGET (r),
                         (dest < 0) ? -1 : (dest + (int) i));
    g_object_unref (r);
  }
  g_ptr_array_free (moving, TRUE);

  int new_source_index = gtk_list_box_row_get_index (child_row);
  if (new_source_index >= 0)
    subtree_apply_indent (list_box, new_source_index);

  g_signal_emit_by_name (self, "screens-reordered");
  return TRUE;
}

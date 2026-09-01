/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miller columns view, as popularised by NeXTSTEP and macOS Finder.
 *
 * Each column shows the contents of one directory. Selecting a folder in
 * column N expands its row in the shared tree model and shows its children
 * in column N+1. All the heavy lifting (loading directories, file
 * operations, context menus, drag and drop) is already done by
 * NautilusFilesView for us, because it supports several directories being
 * loaded at once for the sake of the expandable rows of the list view.
 */

#include "nautilus-columns-view.h"

#include "nautilus-enums.h"
#include "nautilus-file.h"
#include "nautilus-global-preferences.h"
#include "nautilus-list-base-private.h"
#include "nautilus-name-cell.h"
#include "nautilus-view-cell.h"
#include "nautilus-view-item.h"
#include "nautilus-view-model.h"

#include <glib/gi18n.h>

/* Width of a single column, in pixels, per zoom level. Finder uses a fixed
 * width which the user may drag. We follow the zoom level instead, for now. */
#define COLUMN_WIDTH_SMALL 200
#define COLUMN_WIDTH_MEDIUM 240
#define COLUMN_WIDTH_LARGE 320

typedef struct
{
    NautilusColumnsView *view; /* Unowned. */

    /* The row whose children this column shows, or NULL for the root
     * directory of the view. Owned. */
    GtkTreeListRow *parent_row;

    GtkWidget *scrolled_window; /* Owned by the columns box. */
    GtkListView *list_view;     /* Owned by the scrolled window. */
    GtkFilterListModel *filter_model;
    GtkMultiSelection *selection;
} Column;

struct _NautilusColumnsView
{
    NautilusListBase parent_instance;

    GtkWidget *columns_box;
    GPtrArray *columns; /* of Column* */

    gint zoom_level;

    gboolean directories_first;

    GQuark sort_attribute;
    gboolean reversed;

    /* Set while we are propagating a selection between the columns and the
     * shared model, to avoid re-entering the selection handling. */
    gboolean syncing_selection;

    /* Maps GtkTreeListRow* to its position in the shared model, plus one, so
     * that zero can mean "absent". Rebuilt lazily. */
    GHashTable *position_map;

    /* Set of GtkListItem* which are currently bound, so that their :position
     * can be refreshed when the shared model changes. */
    GHashTable *bound_items;

    gulong model_items_changed_id;
    NautilusViewModel *observed_model; /* Unowned. */
};

G_DEFINE_TYPE (NautilusColumnsView, nautilus_columns_view, NAUTILUS_TYPE_LIST_BASE)

enum
{
    LOAD_SUBDIRECTORY,
    UNLOAD_SUBDIRECTORY,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static const NautilusViewInfo columns_view_info =
{
    .view_id = NAUTILUS_VIEW_COLUMNS_ID,
    .zoom_level_min = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
    .zoom_level_max = NAUTILUS_LIST_ZOOM_LEVEL_LARGE,
    .zoom_level_standard = NAUTILUS_LIST_ZOOM_LEVEL_MEDIUM,
};

static void create_column (NautilusColumnsView *self,
                           GtkTreeListRow      *parent_row);
static void truncate_columns (NautilusColumnsView *self,
                              guint                n_kept);

static NautilusViewInfo
real_get_view_info (NautilusListBase *list_base)
{
    return columns_view_info;
}

/* ---------------------------------------------------------------------- */
/* Position mapping                                                       */
/*                                                                        */
/* The cells of every column live in their own GtkListView, so the        */
/* position GTK reports for them is local to that column. The base class,  */
/* however, uses cell positions against the shared model. So we translate. */
/* ---------------------------------------------------------------------- */

static GHashTable *
ensure_position_map (NautilusColumnsView *self)
{
    NautilusViewModel *model;
    guint n_items;

    if (self->position_map != NULL)
    {
        return self->position_map;
    }

    self->position_map = g_hash_table_new (NULL, NULL);

    model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));
    if (model == NULL)
    {
        return self->position_map;
    }

    n_items = g_list_model_get_n_items (G_LIST_MODEL (model));
    for (guint i = 0; i < n_items; i++)
    {
        g_autoptr (GtkTreeListRow) row = g_list_model_get_item (G_LIST_MODEL (model), i);

        /* The model holds a reference, so the borrowed key stays valid. */
        g_hash_table_insert (self->position_map, row, GUINT_TO_POINTER (i + 1));
    }

    return self->position_map;
}

static gboolean
lookup_shared_position (NautilusColumnsView *self,
                        GtkTreeListRow      *row,
                        guint               *out_position)
{
    gpointer value = g_hash_table_lookup (ensure_position_map (self), row);

    if (value == NULL)
    {
        return FALSE;
    }

    *out_position = GPOINTER_TO_UINT (value) - 1;
    return TRUE;
}

static void
refresh_bound_positions (NautilusColumnsView *self)
{
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init (&iter, self->bound_items);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        GtkListItem *listitem = key;
        GtkWidget *cell = gtk_list_item_get_child (listitem);
        /* gtk_list_item_get_item() does not transfer a reference, so take one
         * right away, before any early exit. */
        GtkTreeListRow *unowned_row = gtk_list_item_get_item (listitem);
        g_autoptr (GtkTreeListRow) row = (unowned_row != NULL) ? g_object_ref (unowned_row) : NULL;
        guint position;

        if (cell == NULL || row == NULL)
        {
            continue;
        }

        if (lookup_shared_position (self, row, &position))
        {
            g_object_set (cell, "position", position, NULL);
        }
    }
}

static void
on_shared_model_items_changed (NautilusColumnsView *self)
{
    g_clear_pointer (&self->position_map, g_hash_table_unref);
    refresh_bound_positions (self);
}

/* ---------------------------------------------------------------------- */
/* Columns                                                                */
/* ---------------------------------------------------------------------- */

static gboolean
column_filter_func (gpointer item,
                    gpointer user_data)
{
    GtkTreeListRow *row = item;
    GtkTreeListRow *wanted_parent = user_data;
    g_autoptr (GtkTreeListRow) parent = gtk_tree_list_row_get_parent (row);

    return parent == wanted_parent;
}

static guint
column_index (NautilusColumnsView *self,
              Column              *column)
{
    for (guint i = 0; i < self->columns->len; i++)
    {
        if (g_ptr_array_index (self->columns, i) == column)
        {
            return i;
        }
    }

    g_return_val_if_reached (0);
}

static void
column_free (gpointer data)
{
    Column *column = data;

    g_clear_object (&column->parent_row);
    g_free (column);
}

static void
sync_selection_to_shared_model (NautilusColumnsView *self,
                                Column              *column)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));
    g_autoptr (GtkBitset) column_selection = NULL;
    g_autoptr (GtkBitset) new_selection = NULL;
    g_autoptr (GtkBitset) mask = NULL;
    GtkBitsetIter iter;
    guint i;
    guint n_items;

    if (model == NULL)
    {
        return;
    }

    new_selection = gtk_bitset_new_empty ();
    column_selection = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (column->selection));

    for (gboolean valid = gtk_bitset_iter_init_first (&iter, column_selection, &i);
         valid;
         valid = gtk_bitset_iter_next (&iter, &i))
    {
        g_autoptr (GtkTreeListRow) row = g_list_model_get_item (G_LIST_MODEL (column->filter_model), i);
        guint position;

        if (row != NULL && lookup_shared_position (self, row, &position))
        {
            gtk_bitset_add (new_selection, position);
        }
    }

    n_items = g_list_model_get_n_items (G_LIST_MODEL (model));
    mask = gtk_bitset_new_range (0, n_items);

    gtk_selection_model_set_selection (GTK_SELECTION_MODEL (model), new_selection, mask);
}

/* Returns the row selected in @column, but only if it is the sole selected
 * item. Miller columns only drill down on a single selection. */
static GtkTreeListRow *
get_lone_selected_row (Column *column)
{
    g_autoptr (GtkBitset) selection = NULL;

    selection = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (column->selection));
    if (gtk_bitset_get_size (selection) != 1)
    {
        return NULL;
    }

    return g_list_model_get_item (G_LIST_MODEL (column->filter_model),
                                  gtk_bitset_get_minimum (selection));
}

static void
update_child_column (NautilusColumnsView *self,
                     Column              *column)
{
    guint index = column_index (self, column);
    g_autoptr (GtkTreeListRow) row = get_lone_selected_row (column);
    g_autoptr (NautilusViewItem) item = NULL;

    /* Whatever was to the right of this column is stale now. */
    truncate_columns (self, index + 1);

    if (row == NULL)
    {
        return;
    }

    item = gtk_tree_list_row_get_item (row);
    if (item == NULL || !nautilus_file_is_directory (nautilus_view_item_get_file (item)))
    {
        return;
    }

    /* Ask NautilusFilesView to start monitoring the directory before we
     * expand the row, so that its contents can start arriving right away. */
    g_signal_emit (self, signals[LOAD_SUBDIRECTORY], 0, item);
    gtk_tree_list_row_set_expanded (row, TRUE);

    create_column (self, row);
}

static void
on_column_selection_changed (GtkSelectionModel *selection_model,
                             guint              position,
                             guint              n_items,
                             gpointer           user_data)
{
    Column *column = user_data;
    NautilusColumnsView *self = column->view;

    if (self->syncing_selection)
    {
        return;
    }

    self->syncing_selection = TRUE;

    /* Only one column at a time holds the selection, like in Finder. */
    for (guint i = 0; i < self->columns->len; i++)
    {
        Column *other = g_ptr_array_index (self->columns, i);

        if (other != column)
        {
            gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (other->selection));
        }
    }

    sync_selection_to_shared_model (self, column);
    update_child_column (self, column);

    self->syncing_selection = FALSE;
}

static void
setup_cell (GtkSignalListItemFactory *factory,
            GtkListItem              *listitem,
            gpointer                  user_data)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (user_data);
    NautilusViewCell *cell;
    GtkExpression *expression;

    cell = nautilus_name_cell_new (NAUTILUS_LIST_BASE (self));
    gtk_list_item_set_child (listitem, GTK_WIDGET (cell));
    setup_cell_common (G_OBJECT (listitem), cell);
    setup_cell_hover_inner_target (cell,
                                   nautilus_name_cell_get_content (NAUTILUS_NAME_CELL (cell)));

    g_object_bind_property (self, "icon-size",
                            cell, "icon-size",
                            G_BINDING_SYNC_CREATE);

    /* Use file display name as accessible label, as the other views do. */
    expression = gtk_property_expression_new (GTK_TYPE_LIST_ITEM, NULL, "item");
    expression = gtk_property_expression_new (GTK_TYPE_TREE_LIST_ROW, expression, "item");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_VIEW_ITEM, expression, "file");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_FILE, expression, "a11y-name");
    gtk_expression_bind (expression, listitem, "accessible-label", listitem);
}

static void
bind_cell (GtkSignalListItemFactory *factory,
           GtkListItem              *listitem,
           gpointer                  user_data)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (user_data);
    GtkWidget *cell = gtk_list_item_get_child (listitem);
    g_autoptr (GtkTreeListRow) row = NULL;
    g_autoptr (NautilusViewItem) item = NULL;
    guint position;

    row = gtk_list_item_get_item (listitem);
    g_return_if_fail (row != NULL);
    /* Not transferred by gtk_list_item_get_item(). */
    g_object_ref (row);

    item = gtk_tree_list_row_get_item (row);
    g_return_if_fail (item != NULL);

    nautilus_view_item_set_item_ui (item, cell);

    /* Translate the column-local position into the shared model's. */
    if (lookup_shared_position (self, row, &position))
    {
        g_object_set (cell, "position", position, NULL);
    }

    g_hash_table_add (self->bound_items, listitem);
}

static void
unbind_cell (GtkSignalListItemFactory *factory,
             GtkListItem              *listitem,
             gpointer                  user_data)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (user_data);
    /* Not transferred by gtk_list_item_get_item(). */
    GtkTreeListRow *unowned_row = gtk_list_item_get_item (listitem);
    g_autoptr (GtkTreeListRow) row = (unowned_row != NULL) ? g_object_ref (unowned_row) : NULL;
    g_autoptr (NautilusViewItem) item = NULL;

    g_hash_table_remove (self->bound_items, listitem);

    if (row == NULL)
    {
        return;
    }

    item = gtk_tree_list_row_get_item (row);
    if (item != NULL)
    {
        nautilus_view_item_set_item_ui (item, NULL);
    }
}

static void
on_list_view_activated (GtkListView *list_view,
                        guint        position,
                        gpointer     user_data)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (user_data);

    nautilus_list_base_activate_selection (NAUTILUS_LIST_BASE (self), FALSE);
}

static int
get_column_width (NautilusColumnsView *self)
{
    switch (self->zoom_level)
    {
        case NAUTILUS_LIST_ZOOM_LEVEL_SMALL:
        {
            return COLUMN_WIDTH_SMALL;
        }

        case NAUTILUS_LIST_ZOOM_LEVEL_LARGE:
        {
            return COLUMN_WIDTH_LARGE;
        }

        default:
        {
            return COLUMN_WIDTH_MEDIUM;
        }
    }
}

static void
create_column (NautilusColumnsView *self,
               GtkTreeListRow      *parent_row)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));
    GtkListItemFactory *factory;
    g_autoptr (GtkCustomFilter) filter = NULL;
    Column *column;

    if (model == NULL)
    {
        return;
    }

    column = g_new0 (Column, 1);
    column->view = self;
    column->parent_row = (parent_row != NULL) ? g_object_ref (parent_row) : NULL;

    filter = gtk_custom_filter_new (column_filter_func, column->parent_row, NULL);
    column->filter_model = gtk_filter_list_model_new (g_object_ref (G_LIST_MODEL (model)),
                                                      GTK_FILTER (g_steal_pointer (&filter)));
    /* gtk_multi_selection_new() takes ownership of the model, and we keep
     * our own reference in column->filter_model. */
    column->selection = gtk_multi_selection_new (g_object_ref (G_LIST_MODEL (column->filter_model)));

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (setup_cell), self);
    g_signal_connect (factory, "bind", G_CALLBACK (bind_cell), self);
    g_signal_connect (factory, "unbind", G_CALLBACK (unbind_cell), self);

    column->list_view = GTK_LIST_VIEW (gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (column->selection)),
                                                          factory));
    /* As in the other views, we roll our own click handling. */
    gtk_list_view_set_single_click_activate (column->list_view, FALSE);
    gtk_list_view_set_tab_behavior (column->list_view, GTK_LIST_TAB_ITEM);
    g_signal_connect (column->list_view, "activate",
                      G_CALLBACK (on_list_view_activated), self);

    column->scrolled_window = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (column->scrolled_window),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (column->scrolled_window),
                                   GTK_WIDGET (column->list_view));
    gtk_widget_set_size_request (column->scrolled_window, get_column_width (self), -1);
    gtk_widget_set_vexpand (column->scrolled_window, TRUE);
    gtk_widget_add_css_class (column->scrolled_window, "nautilus-columns-view-column");

    g_signal_connect (column->selection, "selection-changed",
                      G_CALLBACK (on_column_selection_changed), column);

    g_ptr_array_add (self->columns, column);
    gtk_box_append (GTK_BOX (self->columns_box), column->scrolled_window);
}

static void
truncate_columns (NautilusColumnsView *self,
                  guint                n_kept)
{
    while (self->columns->len > n_kept)
    {
        Column *column = g_ptr_array_index (self->columns, self->columns->len - 1);

        /* Tell NautilusFilesView it may stop monitoring this directory. */
        if (column->parent_row != NULL)
        {
            g_autoptr (NautilusViewItem) item = gtk_tree_list_row_get_item (column->parent_row);

            gtk_tree_list_row_set_expanded (column->parent_row, FALSE);

            if (item != NULL)
            {
                g_signal_emit (self, signals[UNLOAD_SUBDIRECTORY], 0, item);
            }
        }

        gtk_box_remove (GTK_BOX (self->columns_box), column->scrolled_window);

        g_clear_object (&column->selection);
        g_clear_object (&column->filter_model);

        /* Frees the Column, via column_free(). */
        g_ptr_array_remove_index (self->columns, self->columns->len - 1);
    }
}

static void
rebuild_columns (NautilusColumnsView *self)
{
    truncate_columns (self, 0);
    g_clear_pointer (&self->position_map, g_hash_table_unref);
    create_column (self, NULL);
}

static void
on_model_changed (NautilusColumnsView *self)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));

    if (self->observed_model != NULL && self->model_items_changed_id != 0)
    {
        g_signal_handler_disconnect (self->observed_model, self->model_items_changed_id);
        self->model_items_changed_id = 0;
        self->observed_model = NULL;
    }

    truncate_columns (self, 0);
    g_clear_pointer (&self->position_map, g_hash_table_unref);

    if (model == NULL)
    {
        return;
    }

    self->observed_model = model;
    self->model_items_changed_id =
        g_signal_connect_swapped (model, "items-changed",
                                  G_CALLBACK (on_shared_model_items_changed), self);

    create_column (self, NULL);
}

/* ---------------------------------------------------------------------- */
/* NautilusListBase vfuncs                                                */
/* ---------------------------------------------------------------------- */

static gint
nautilus_columns_view_sort (gconstpointer a,
                            gconstpointer b,
                            gpointer      user_data)
{
    NautilusColumnsView *self = user_data;
    NautilusFile *file_a = nautilus_view_item_get_file ((NautilusViewItem *) a);
    NautilusFile *file_b = nautilus_view_item_get_file ((NautilusViewItem *) b);

    return nautilus_file_compare_for_sort_by_attribute_q (file_a, file_b,
                                                          self->sort_attribute,
                                                          self->directories_first,
                                                          self->reversed);
}

static void
update_sort_directories_first (NautilusColumnsView *self)
{
    NautilusFile *directory_as_file = nautilus_list_base_get_directory_as_file (NAUTILUS_LIST_BASE (self));
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));

    if (directory_as_file != NULL &&
        (nautilus_file_is_in_search (directory_as_file) ||
         nautilus_file_is_in_recent (directory_as_file)))
    {
        self->directories_first = FALSE;
    }
    else
    {
        self->directories_first = g_settings_get_boolean (gtk_filechooser_preferences,
                                                          NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST);
    }

    if (model != NULL)
    {
        nautilus_view_model_sort (model);
    }
}

static void
nautilus_columns_view_setup_directory (NautilusListBase  *list_base,
                                       NautilusDirectory *new_directory)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);

    NAUTILUS_LIST_BASE_CLASS (nautilus_columns_view_parent_class)->setup_directory (list_base, new_directory);

    update_sort_directories_first (self);
    rebuild_columns (self);
}

static guint
real_get_icon_size (NautilusListBase *list_base)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);

    switch (self->zoom_level)
    {
        case NAUTILUS_LIST_ZOOM_LEVEL_SMALL:
        {
            return NAUTILUS_LIST_ICON_SIZE_SMALL;
        }

        case NAUTILUS_LIST_ZOOM_LEVEL_LARGE:
        {
            return NAUTILUS_LIST_ICON_SIZE_LARGE;
        }

        default:
        {
            return NAUTILUS_LIST_ICON_SIZE_MEDIUM;
        }
    }
}

static int
real_get_zoom_level (NautilusListBase *list_base)
{
    return NAUTILUS_COLUMNS_VIEW (list_base)->zoom_level;
}

static void
real_set_zoom_level (NautilusListBase *list_base,
                     int               new_level)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);

    g_return_if_fail (new_level >= columns_view_info.zoom_level_min &&
                      new_level <= columns_view_info.zoom_level_max);

    self->zoom_level = new_level;

    for (guint i = 0; i < self->columns->len; i++)
    {
        Column *column = g_ptr_array_index (self->columns, i);

        gtk_widget_set_size_request (column->scrolled_window, get_column_width (self), -1);
    }

    g_object_notify (G_OBJECT (self), "icon-size");
}

static void
real_scroll_to (NautilusListBase   *list_base,
                guint               position,
                GtkListScrollFlags  flags,
                GtkScrollInfo      *scroll)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);
    NautilusViewModel *model = nautilus_list_base_get_model (list_base);
    g_autoptr (GtkTreeListRow) row = NULL;

    if (model == NULL)
    {
        g_clear_pointer (&scroll, gtk_scroll_info_unref);
        return;
    }

    row = g_list_model_get_item (G_LIST_MODEL (model), position);
    if (row == NULL)
    {
        g_clear_pointer (&scroll, gtk_scroll_info_unref);
        return;
    }

    /* Find the column showing this row, and scroll within it. */
    for (guint i = 0; i < self->columns->len; i++)
    {
        Column *column = g_ptr_array_index (self->columns, i);
        guint n_items = g_list_model_get_n_items (G_LIST_MODEL (column->filter_model));

        for (guint j = 0; j < n_items; j++)
        {
            g_autoptr (GtkTreeListRow) candidate = g_list_model_get_item (G_LIST_MODEL (column->filter_model), j);

            if (candidate == row)
            {
                gtk_list_view_scroll_to (column->list_view, j, flags,
                                         g_steal_pointer (&scroll));
                return;
            }
        }
    }

    g_clear_pointer (&scroll, gtk_scroll_info_unref);
}

static void
real_set_enable_rubberband (NautilusListBase *list_base,
                            gboolean          enabled)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);

    for (guint i = 0; i < self->columns->len; i++)
    {
        Column *column = g_ptr_array_index (self->columns, i);

        gtk_list_view_set_enable_rubberband (column->list_view, enabled);
    }
}

static GVariant *
real_get_sort_state (NautilusListBase *list_base)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);

    return g_variant_take_ref (g_variant_new ("(sb)",
                                              g_quark_to_string (self->sort_attribute),
                                              self->reversed));
}

static void
real_set_sort_state (NautilusListBase *list_base,
                     GVariant         *value)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (list_base);
    NautilusViewModel *model = nautilus_list_base_get_model (list_base);
    const gchar *target_name;
    g_autoptr (GtkCustomSorter) sorter = NULL;

    g_return_if_fail (model != NULL);

    g_variant_get (value, "(&sb)", &target_name, &self->reversed);
    self->sort_attribute = g_quark_from_string (target_name);

    sorter = gtk_custom_sorter_new (nautilus_columns_view_sort, self, NULL);
    nautilus_view_model_set_sorter (model, GTK_SORTER (sorter));
}

/* ---------------------------------------------------------------------- */
/* GObject                                                                */
/* ---------------------------------------------------------------------- */

static void
dispose (GObject *object)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (object);

    if (self->observed_model != NULL && self->model_items_changed_id != 0)
    {
        g_signal_handler_disconnect (self->observed_model, self->model_items_changed_id);
        self->model_items_changed_id = 0;
        self->observed_model = NULL;
    }

    if (self->columns != NULL)
    {
        for (guint i = 0; i < self->columns->len; i++)
        {
            Column *column = g_ptr_array_index (self->columns, i);

            g_clear_object (&column->selection);
            g_clear_object (&column->filter_model);
        }
        g_ptr_array_set_size (self->columns, 0);
    }

    G_OBJECT_CLASS (nautilus_columns_view_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    NautilusColumnsView *self = NAUTILUS_COLUMNS_VIEW (object);

    g_clear_pointer (&self->columns, g_ptr_array_unref);
    g_clear_pointer (&self->position_map, g_hash_table_unref);
    g_clear_pointer (&self->bound_items, g_hash_table_unref);

    G_OBJECT_CLASS (nautilus_columns_view_parent_class)->finalize (object);
}

static void
nautilus_columns_view_class_init (NautilusColumnsViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusListBaseClass *list_base_class = NAUTILUS_LIST_BASE_CLASS (klass);

    object_class->dispose = dispose;
    object_class->finalize = finalize;

    list_base_class->get_icon_size = real_get_icon_size;
    list_base_class->get_sort_state = real_get_sort_state;
    list_base_class->get_view_info = real_get_view_info;
    list_base_class->get_zoom_level = real_get_zoom_level;
    list_base_class->scroll_to = real_scroll_to;
    list_base_class->set_enable_rubberband = real_set_enable_rubberband;
    list_base_class->set_sort_state = real_set_sort_state;
    list_base_class->set_zoom_level = real_set_zoom_level;
    list_base_class->setup_directory = nautilus_columns_view_setup_directory;

    signals[LOAD_SUBDIRECTORY] = g_signal_new ("load-subdirectory",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 1, NAUTILUS_TYPE_VIEW_ITEM);
    signals[UNLOAD_SUBDIRECTORY] = g_signal_new ("unload-subdirectory",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0, NULL, NULL, NULL,
                                                 G_TYPE_NONE, 1, NAUTILUS_TYPE_VIEW_ITEM);
}

static void
nautilus_columns_view_init (NautilusColumnsView *self)
{
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (NAUTILUS_LIST_BASE (self));

    gtk_widget_add_css_class (GTK_WIDGET (self), "nautilus-columns-view");

    self->columns = g_ptr_array_new_with_free_func (column_free);
    self->bound_items = g_hash_table_new (NULL, NULL);
    self->zoom_level = columns_view_info.zoom_level_standard;
    self->sort_attribute = g_quark_from_string ("name");

    /* Columns scroll horizontally as a whole; each column scrolls vertically
     * on its own. */
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);

    self->columns_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->columns_box, "nautilus-columns-view-box");
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window), self->columns_box);

    nautilus_list_base_setup_gestures (NAUTILUS_LIST_BASE (self));

    g_signal_connect_swapped (self, "notify::model", G_CALLBACK (on_model_changed), self);

    g_signal_connect_object (gtk_filechooser_preferences,
                             "changed::" NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST,
                             G_CALLBACK (update_sort_directories_first),
                             self,
                             G_CONNECT_SWAPPED);
}

NautilusColumnsView *
nautilus_columns_view_new (void)
{
    return g_object_new (NAUTILUS_TYPE_COLUMNS_VIEW, NULL);
}

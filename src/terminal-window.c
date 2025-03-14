/*
 * Copyright © 2001 Havoc Pennington
 * Copyright © 2002 Red Hat, Inc.
 * Copyright © 2007, 2008, 2009 Christian Persch
 * Copyright (C) 2012-2021 MATE Developers
 *
 * Mate-terminal is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mate-terminal is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <string.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <gdk/gdkkeysyms.h>

#include "terminal-accels.h"
#include "terminal-app.h"
#include "terminal-debug.h"
#include "terminal-encoding.h"
#include "terminal-intl.h"
#include "terminal-screen-container.h"
#include "terminal-search-dialog.h"
#include "terminal-tab-label.h"
#include "terminal-tabs-menu.h"
#include "terminal-util.h"
#include "terminal-window.h"

#ifdef ENABLE_SKEY
#include "skey-popup.h"
#endif

static gboolean detach_tab = FALSE;

struct _TerminalWindowPrivate
{
    GtkActionGroup *action_group;
    GtkUIManager *ui_manager;
    guint ui_id;

    GtkActionGroup *profiles_action_group;
    guint profiles_ui_id;

    GtkActionGroup *encodings_action_group;
    guint encodings_ui_id;

    TerminalTabsMenu *tabs_menu;

    TerminalScreenPopupInfo *popup_info;
    guint remove_popup_info_idle;

    GtkActionGroup *new_terminal_action_group;
    guint new_terminal_ui_id;

    GtkWidget *menubar;
    GtkWidget *notebook;
    GtkWidget *main_vbox;
    TerminalScreen *active_screen;

    /* Size of a character cell in pixels */
    int old_char_width;
    int old_char_height;

    /* Width and height added to the actual terminal grid by "chrome" inside
     * what was traditionally the X11 window: menu bar, title bar,
     * style-provided padding. This must be included when resizing the window
     * and also included in geometry hints. */
    int old_chrome_width;
    int old_chrome_height;

    /* Width and height of the padding around the geometry widget. */
    int old_padding_width;
    int old_padding_height;

    void *old_geometry_widget; /* only used for pointer value as it may be freed */

    GtkWidget *confirm_close_dialog;
    GtkWidget *search_find_dialog;

    guint menubar_visible : 1;
    guint use_default_menubar_visibility : 1;

    /* Compositing manager integration */
    guint have_argb_visual : 1;

    /* Used to clear stray "demands attention" flashing on our window when we
     * unmap and map it to switch to an ARGB visual.
     */
    guint clear_demands_attention : 1;

    guint disposed : 1;
    guint present_on_insert : 1;

    /* Workaround until gtk+ bug #535557 is fixed */
    guint icon_title_set : 1;

    gint64 focus_time;

    /* should we copy selection to clibpoard */
    int copy_selection;
};

#define PROFILE_DATA_KEY "GT::Profile"

#define FILE_NEW_TERMINAL_TAB_UI_PATH     "/menubar/File/FileNewTabProfiles"
#define FILE_NEW_TERMINAL_WINDOW_UI_PATH  "/menubar/File/FileNewWindowProfiles"
#define SET_ENCODING_UI_PATH              "/menubar/Terminal/TerminalSetEncoding/EncodingsPH"
#define SET_ENCODING_ACTION_NAME_PREFIX   "TerminalSetEncoding"

#define PROFILES_UI_PATH        "/menubar/Terminal/TerminalProfiles/ProfilesPH"
#define PROFILES_POPUP_UI_PATH  "/Popup/PopupTerminalProfiles/ProfilesPH"

#define SIZE_TO_UI_PATH            "/menubar/Terminal/TerminalSizeToPH"
#define SIZE_TO_ACTION_NAME_PREFIX "TerminalSizeTo"

#define STOCK_NEW_TAB     "tab-new"

#define ENCODING_DATA_KEY "encoding"

#if 1
/*
 * We don't want to enable content saving until vte supports it async.
 * So we disable this code for stable versions.
 */
#include "terminal-version.h"

#if (TERMINAL_MINOR_VERSION & 1) != 0
#define ENABLE_SAVE
#else
#undef ENABLE_SAVE
#endif
#endif

static void terminal_window_dispose     (GObject             *object);
static void terminal_window_finalize    (GObject             *object);
static gboolean terminal_window_state_event (GtkWidget            *widget,
        GdkEventWindowState  *event);

static gboolean terminal_window_delete_event (GtkWidget *widget,
        GdkEvent *event,
        gpointer data);
static gboolean terminal_window_focus_in_event (GtkWidget *widget,
                                                GdkEventFocus *event,
                                                gpointer data);

static gboolean notebook_button_press_cb     (GtkWidget *notebook,
        GdkEventButton *event,
        GSettings *settings);
static gboolean window_key_press_cb     (GtkWidget *notebook,
        GdkEventKey *event,
        GSettings *settings);
static gboolean notebook_popup_menu_cb       (GtkWidget *notebook,
        TerminalWindow *window);
static void notebook_page_selected_callback  (GtkWidget       *notebook,
        GtkWidget       *page,
        guint            page_num,
        TerminalWindow  *window);
static void notebook_page_added_callback     (GtkWidget       *notebook,
        GtkWidget       *container,
        guint            page_num,
        TerminalWindow  *window);
static void notebook_page_removed_callback   (GtkWidget       *notebook,
        GtkWidget       *container,
        guint            page_num,
        TerminalWindow  *window);
static gboolean notebook_scroll_event_cb     (GtkWidget      *notebook,
                                              GdkEventScroll *event,
                                              TerminalWindow *window);

/* Menu action callbacks */
static void file_new_window_callback          (GtkAction *action,
        TerminalWindow *window);
static void file_new_tab_callback             (GtkAction *action,
        TerminalWindow *window);
static void file_new_profile_callback         (GtkAction *action,
        TerminalWindow *window);
static void file_close_window_callback        (GtkAction *action,
        TerminalWindow *window);
static void file_save_contents_callback       (GtkAction *action,
        TerminalWindow *window);
static void file_close_tab_callback           (GtkAction *action,
        TerminalWindow *window);
static void edit_copy_callback                (GtkAction *action,
        TerminalWindow *window);
static void edit_paste_callback               (GtkAction *action,
        TerminalWindow *window);
static void edit_select_all_callback          (GtkAction *action,
        TerminalWindow *window);
static void edit_keybindings_callback         (GtkAction *action,
        TerminalWindow *window);
static void edit_profiles_callback            (GtkAction *action,
        TerminalWindow *window);
static void edit_current_profile_callback     (GtkAction *action,
        TerminalWindow *window);
static void view_menubar_toggled_callback     (GtkToggleAction *action,
        TerminalWindow *window);
static void view_fullscreen_toggled_callback  (GtkToggleAction *action,
        TerminalWindow *window);
static void view_zoom_in_callback             (GtkAction *action,
        TerminalWindow *window);
static void view_zoom_out_callback            (GtkAction *action,
        TerminalWindow *window);
static void view_zoom_normal_callback         (GtkAction *action,
        TerminalWindow *window);
static void search_find_callback              (GtkAction *action,
        TerminalWindow *window);
static void search_find_next_callback         (GtkAction *action,
        TerminalWindow *window);
static void search_find_prev_callback         (GtkAction *action,
        TerminalWindow *window);
static void search_clear_highlight_callback   (GtkAction *action,
        TerminalWindow *window);
static void terminal_next_or_previous_profile_cb (GtkAction *action,
        TerminalWindow *window);
static void terminal_set_title_callback       (GtkAction *action,
        TerminalWindow *window);
static void terminal_add_encoding_callback    (GtkAction *action,
        TerminalWindow *window);
static void terminal_reset_callback           (GtkAction *action,
        TerminalWindow *window);
static void terminal_reset_clear_callback     (GtkAction *action,
        TerminalWindow *window);
static void tabs_next_or_previous_tab_cb      (GtkAction *action,
        TerminalWindow *window);
static void tabs_move_left_callback           (GtkAction *action,
        TerminalWindow *window);
static void tabs_move_right_callback          (GtkAction *action,
        TerminalWindow *window);
static void tabs_detach_tab_callback          (GtkAction *action,
        TerminalWindow *window);
static void help_contents_callback        (GtkAction *action,
        TerminalWindow *window);
static void help_about_callback           (GtkAction *action,
        TerminalWindow *window);

static gboolean find_larger_zoom_factor  (double  current,
        double *found);
static gboolean find_smaller_zoom_factor (double  current,
        double *found);

static void terminal_window_show (GtkWidget *widget);

static gboolean confirm_close_window_or_tab (TerminalWindow *window,
        TerminalScreen *screen);

static void
profile_set_callback (TerminalScreen *screen,
                      TerminalProfile *old_profile,
                      TerminalWindow *window);
static void
sync_screen_icon_title (TerminalScreen *screen,
                        GParamSpec *psepc,
                        TerminalWindow *window);

G_DEFINE_TYPE_WITH_PRIVATE (TerminalWindow, terminal_window, GTK_TYPE_WINDOW)

/* Menubar mnemonics & accel settings handling */

static void
app_setting_notify_cb (TerminalApp *app,
                       GParamSpec *pspec,
                       GdkScreen *screen)
{
    GtkSettings *settings;
    const char *prop_name;

    if (pspec)
        prop_name = pspec->name;
    else
        prop_name = NULL;

    settings = gtk_settings_get_for_screen (screen);

    if (!prop_name || prop_name == I_(TERMINAL_APP_ENABLE_MNEMONICS))
    {
        gboolean enable_mnemonics;

        g_object_get (app, TERMINAL_APP_ENABLE_MNEMONICS, &enable_mnemonics, NULL);
        g_object_set (settings, "gtk-enable-mnemonics", enable_mnemonics, NULL);
    }

    if (!prop_name || prop_name == I_(TERMINAL_APP_ENABLE_MENU_BAR_ACCEL))
    {
        /* const */ char *saved_menubar_accel;
        gboolean enable_menubar_accel;

        /* FIXME: Once gtk+ bug 507398 is fixed, use that to reset the property instead */
        /* Now this is a bad hack on so many levels. */
        saved_menubar_accel = g_object_get_data (G_OBJECT (settings), "GT::gtk-menu-bar-accel");
        if (!saved_menubar_accel)
        {
            g_object_get (settings, "gtk-menu-bar-accel", &saved_menubar_accel, NULL);
            g_object_set_data_full (G_OBJECT (settings), "GT::gtk-menu-bar-accel",
                                    saved_menubar_accel, (GDestroyNotify) g_free);
        }

        g_object_get (app, TERMINAL_APP_ENABLE_MENU_BAR_ACCEL, &enable_menubar_accel, NULL);
        if (enable_menubar_accel)
            g_object_set (settings, "gtk-menu-bar-accel", saved_menubar_accel, NULL);
        else
            g_object_set (settings, "gtk-menu-bar-accel", NULL, NULL);
    }
}

static void
app_setting_notify_destroy_cb (GdkScreen *screen)
{
    g_signal_handlers_disconnect_by_func (terminal_app_get (),
                                          G_CALLBACK (app_setting_notify_cb),
                                          screen);
}

/* utility functions */

/*
  Derived from XParseGeometry() in X.org

  Copyright 1985, 1986, 1987, 1998  The Open Group

  All Rights Reserved.

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR
  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
  OTHER DEALINGS IN THE SOFTWARE.

  Except as contained in this notice, the name of The Open Group shall
  not be used in advertising or otherwise to promote the sale, use or
  other dealings in this Software without prior written authorization
  from The Open Group.
*/

/*
 *    XParseGeometry parses strings of the form
 *   "=<width>x<height>{+-}<xoffset>{+-}<yoffset>", where
 *   width, height, xoffset, and yoffset are unsigned integers.
 *   Example: "=80x24+300-49"
 *   The equal sign is optional.
 *   It returns a bitmask that indicates which of the four values
 *   were actually found in the string. For each value found,
 *   the corresponding argument is updated; for each value
 *   not found, the corresponding argument is left unchanged.
 */

/* The following code is from Xlib, and is minimally modified, so we
 * can track any upstream changes if required. Don’t change this
 * code. Or if you do, put in a huge comment marking which thing
 * changed.
 */

static int
terminal_window_ReadInteger (char  *string,
                             char **NextString)
{
    register int Result = 0;
    int Sign = 1;

    if (*string == '+')
	string++;
    else if (*string == '-')
    {
	string++;
	Sign = -1;
    }
    for (; (*string >= '0') && (*string <= '9'); string++)
    {
	Result = (Result * 10) + (*string - '0');
    }
    *NextString = string;
    if (Sign >= 0)
	return (Result);
    else
	return (-Result);
}

/*
 * Bitmask returned by XParseGeometry(). Each bit tells if the corresponding
 * value (x, y, width, height) was found in the parsed string.
 */
#define NoValue         0x0000
#define XValue          0x0001
#define YValue          0x0002
#define WidthValue      0x0004
#define HeightValue     0x0008
#define XNegative       0x0010
#define YNegative       0x0020

static int
terminal_window_XParseGeometry (const char *string,
                                int *x, int *y,
                                unsigned int *width,
                                unsigned int *height)
{
	int mask = NoValue;
	register char *strind;
	unsigned int tempWidth = 0, tempHeight = 0;
	int tempX = 0, tempY = 0;
	char *nextCharacter;

	if ( (string == NULL) || (*string == '\0')) return(mask);
	if (*string == '=')
		string++;  /* ignore possible '=' at beg of geometry spec */

	strind = (char *)string;
	if (*strind != '+' && *strind != '-' && *strind != 'x') {
		tempWidth = terminal_window_ReadInteger(strind, &nextCharacter);
		if (strind == nextCharacter)
		    return (0);
		strind = nextCharacter;
		mask |= WidthValue;
	}

	if (*strind == 'x' || *strind == 'X') {
		strind++;
		tempHeight = terminal_window_ReadInteger(strind, &nextCharacter);
		if (strind == nextCharacter)
		    return (0);
		strind = nextCharacter;
		mask |= HeightValue;
	}

	if ((*strind == '+') || (*strind == '-')) {
		if (*strind == '-') {
			strind++;
			tempX = -terminal_window_ReadInteger(strind, &nextCharacter);
			if (strind == nextCharacter)
			    return (0);
			strind = nextCharacter;
			mask |= XNegative;

		}
		else
		{	strind++;
			tempX = terminal_window_ReadInteger(strind, &nextCharacter);
			if (strind == nextCharacter)
			    return(0);
			strind = nextCharacter;
		}
		mask |= XValue;
		if ((*strind == '+') || (*strind == '-')) {
			if (*strind == '-') {
				strind++;
				tempY = -terminal_window_ReadInteger(strind, &nextCharacter);
				if (strind == nextCharacter)
				    return(0);
				strind = nextCharacter;
				mask |= YNegative;

			}
			else
			{
				strind++;
				tempY = terminal_window_ReadInteger(strind, &nextCharacter);
				if (strind == nextCharacter)
				    return(0);
				strind = nextCharacter;
			}
			mask |= YValue;
		}
	}

	/* If strind isn't at the end of the string the it's an invalid
		geometry specification. */

	if (*strind != '\0') return (0);

	if (mask & XValue)
	    *x = tempX;
	if (mask & YValue)
	    *y = tempY;
	if (mask & WidthValue)
            *width = tempWidth;
	if (mask & HeightValue)
            *height = tempHeight;
	return (mask);
}

static char *
escape_underscores (const char *name)
{
    GString *escaped_name;

    g_assert (name != NULL);

    /* Who'd use more that 4 underscores in a profile name... */
    escaped_name = g_string_sized_new (strlen (name) + 4 + 1);

    while (*name)
    {
        if (*name == '_')
            g_string_append (escaped_name, "__");
        else
            g_string_append_c (escaped_name, *name);
        name++;
    }

    return g_string_free (escaped_name, FALSE);
}

static int
find_tab_num_at_pos (GtkNotebook *notebook,
                     int screen_x,
                     int screen_y)
{
    GtkPositionType tab_pos;
    int page_num = 0;
    GtkNotebook *nb = GTK_NOTEBOOK (notebook);
    GtkWidget *page;
    GtkAllocation tab_allocation;

    tab_pos = gtk_notebook_get_tab_pos (GTK_NOTEBOOK (notebook));

    while ((page = gtk_notebook_get_nth_page (nb, page_num)))
    {
        GtkWidget *tab;
        int max_x, max_y, x_root, y_root;

        tab = gtk_notebook_get_tab_label (nb, page);
        g_return_val_if_fail (tab != NULL, -1);

        if (!gtk_widget_get_mapped (GTK_WIDGET (tab)))
        {
            page_num++;
            continue;
        }

        gdk_window_get_origin (gtk_widget_get_window (tab), &x_root, &y_root);

        gtk_widget_get_allocation (tab, &tab_allocation);
        max_x = x_root + tab_allocation.x + tab_allocation.width;
        max_y = y_root + tab_allocation.y + tab_allocation.height;

        if ((tab_pos == GTK_POS_TOP || tab_pos == GTK_POS_BOTTOM) && screen_x <= max_x)
            return page_num;

        if ((tab_pos == GTK_POS_LEFT || tab_pos == GTK_POS_RIGHT) && screen_y <= max_y)
            return page_num;

        page_num++;
    }

    return -1;
}

static void
terminal_set_profile_toggled_callback (GtkToggleAction *action,
                                       TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalProfile *profile;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    if (!gtk_toggle_action_get_active (action))
        return;
    G_GNUC_END_IGNORE_DEPRECATIONS;

    if (priv->active_screen == NULL)
        return;

    profile = g_object_get_data (G_OBJECT (action), PROFILE_DATA_KEY);
    g_assert (profile);

    if (_terminal_profile_get_forgotten (profile))
        return;

    g_signal_handlers_block_by_func (priv->active_screen, G_CALLBACK (profile_set_callback), window);
    terminal_screen_set_profile (priv->active_screen, profile);
    g_signal_handlers_unblock_by_func (priv->active_screen, G_CALLBACK (profile_set_callback), window);
}

static void
profile_visible_name_notify_cb (TerminalProfile *profile,
                                GParamSpec *pspec,
                                GtkAction *action)
{
    const char *visible_name;
    char *dot, *display_name;
    guint num;

    visible_name = terminal_profile_get_property_string (profile, TERMINAL_PROFILE_VISIBLE_NAME);
    display_name = escape_underscores (visible_name);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    dot = strchr (gtk_action_get_name (action), '.');
    G_GNUC_END_IGNORE_DEPRECATIONS;
    if (dot != NULL)
    {
        char *free_me;

        num = g_ascii_strtoll (dot + 1, NULL, 10);

        free_me = display_name;
        if (num < 10)
            /* Translators: This is the label of a menu item to choose a profile.
             * _%d is used as the accelerator (with d between 1 and 9), and
             * the %s is the name of the terminal profile.
             */
            display_name = g_strdup_printf (_("_%d. %s"), num, display_name);
        else if (num < 36)
            /* Translators: This is the label of a menu item to choose a profile.
             * _%c is used as the accelerator (it will be a character between A and Z),
             * and the %s is the name of the terminal profile.
             */
            display_name = g_strdup_printf (_("_%c. %s"), ('A' + num - 10), display_name);
        else
            free_me = NULL;

        g_free (free_me);
    }

    g_object_set (action, "label", display_name, NULL);
    g_free (display_name);
}

static void
disconnect_profiles_from_actions_in_group (GtkActionGroup *action_group)
{
    GList *actions, *l;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    actions = gtk_action_group_list_actions (action_group);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    for (l = actions; l != NULL; l = l->next)
    {
        GObject *action = G_OBJECT (l->data);
        TerminalProfile *profile;

        profile = g_object_get_data (action, PROFILE_DATA_KEY);
        if (!profile)
            continue;

        g_signal_handlers_disconnect_by_func (profile, G_CALLBACK (profile_visible_name_notify_cb), action);
    }
    g_list_free (actions);
}

static void
terminal_window_update_set_profile_menu_active_profile (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalProfile *new_active_profile;
    GList *actions, *l;

    if (!priv->profiles_action_group)
        return;

    if (!priv->active_screen)
        return;

    new_active_profile = terminal_screen_get_profile (priv->active_screen);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    actions = gtk_action_group_list_actions (priv->profiles_action_group);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    for (l = actions; l != NULL; l = l->next)
    {
        GObject *action = G_OBJECT (l->data);
        TerminalProfile *profile;

        profile = g_object_get_data (action, PROFILE_DATA_KEY);
        if (profile != new_active_profile)
            continue;

        g_signal_handlers_block_by_func (action, G_CALLBACK (terminal_set_profile_toggled_callback), window);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action), TRUE);
        G_GNUC_END_IGNORE_DEPRECATIONS;
        g_signal_handlers_unblock_by_func (action, G_CALLBACK (terminal_set_profile_toggled_callback), window);

        break;
    }
    g_list_free (actions);
}

static void
terminal_window_update_set_profile_menu (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalProfile *active_profile;
    GtkActionGroup *action_group;
    GtkAction *action;
    GList *profiles, *p;
    GSList *group;
    guint n;
    gboolean single_profile;

    /* Remove the old UI */
    if (priv->profiles_ui_id != 0)
    {
        gtk_ui_manager_remove_ui (priv->ui_manager, priv->profiles_ui_id);
        priv->profiles_ui_id = 0;
    }

    if (priv->profiles_action_group != NULL)
    {
        disconnect_profiles_from_actions_in_group (priv->profiles_action_group);
        gtk_ui_manager_remove_action_group (priv->ui_manager,
                                            priv->profiles_action_group);
        priv->profiles_action_group = NULL;
    }

    profiles = terminal_app_get_profile_list (terminal_app_get ());

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "TerminalProfiles");
    single_profile = !profiles || profiles->next == NULL; /* list length <= 1 */
    gtk_action_set_sensitive (action, !single_profile);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    if (profiles == NULL)
        return;

    if (priv->active_screen)
        active_profile = terminal_screen_get_profile (priv->active_screen);
    else
        active_profile = NULL;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action_group = priv->profiles_action_group = gtk_action_group_new ("Profiles");
    G_GNUC_END_IGNORE_DEPRECATIONS;
    gtk_ui_manager_insert_action_group (priv->ui_manager, action_group, -1);
    g_object_unref (action_group);

    priv->profiles_ui_id = gtk_ui_manager_new_merge_id (priv->ui_manager);

    group = NULL;
    n = 0;
    for (p = profiles; p != NULL; p = p->next)
    {
        TerminalProfile *profile = (TerminalProfile *) p->data;
        GtkRadioAction *profile_action;
        char name[32];

        g_snprintf (name, sizeof (name), "TerminalSetProfile%u", n++);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        profile_action = gtk_radio_action_new (name,
                                               NULL,
                                               NULL,
                                               NULL,
                                               n);

        gtk_radio_action_set_group (profile_action, group);
        group = gtk_radio_action_get_group (profile_action);

        if (profile == active_profile)
            gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (profile_action), TRUE);
        G_GNUC_END_IGNORE_DEPRECATIONS;

        g_object_set_data_full (G_OBJECT (profile_action),
                                PROFILE_DATA_KEY,
                                g_object_ref (profile),
                                (GDestroyNotify) g_object_unref);
        profile_visible_name_notify_cb (profile, NULL, GTK_ACTION (profile_action));
        g_signal_connect (profile, "notify::" TERMINAL_PROFILE_VISIBLE_NAME,
                          G_CALLBACK (profile_visible_name_notify_cb), profile_action);
        g_signal_connect (profile_action, "toggled",
                          G_CALLBACK (terminal_set_profile_toggled_callback), window);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        gtk_action_group_add_action (action_group, GTK_ACTION (profile_action));
        G_GNUC_END_IGNORE_DEPRECATIONS;
        g_object_unref (profile_action);

        gtk_ui_manager_add_ui (priv->ui_manager, priv->profiles_ui_id,
                               PROFILES_UI_PATH,
                               name, name,
                               GTK_UI_MANAGER_MENUITEM, FALSE);
        gtk_ui_manager_add_ui (priv->ui_manager, priv->profiles_ui_id,
                               PROFILES_POPUP_UI_PATH,
                               name, name,
                               GTK_UI_MANAGER_MENUITEM, FALSE);
    }

    g_list_free (profiles);
}

static void
terminal_window_create_new_terminal_action (TerminalWindow *window,
        TerminalProfile *profile,
        const char *name,
        guint num,
        GCallback callback)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_new (name, NULL, NULL, NULL);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    g_object_set_data_full (G_OBJECT (action),
                            PROFILE_DATA_KEY,
                            g_object_ref (profile),
                            (GDestroyNotify) g_object_unref);
    profile_visible_name_notify_cb (profile, NULL, action);
    g_signal_connect (profile, "notify::" TERMINAL_PROFILE_VISIBLE_NAME,
                      G_CALLBACK (profile_visible_name_notify_cb), action);
    g_signal_connect (action, "activate", callback, window);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    gtk_action_group_add_action (priv->new_terminal_action_group, action);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    g_object_unref (action);
}

static void
terminal_window_update_new_terminal_menus (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkActionGroup *action_group;
    GtkAction *action;
    GList *profiles, *p;
    guint n;
    gboolean have_single_profile;

    /* Remove the old UI */
    if (priv->new_terminal_ui_id != 0)
    {
        gtk_ui_manager_remove_ui (priv->ui_manager, priv->new_terminal_ui_id);
        priv->new_terminal_ui_id = 0;
    }

    if (priv->new_terminal_action_group != NULL)
    {
        disconnect_profiles_from_actions_in_group (priv->new_terminal_action_group);
        gtk_ui_manager_remove_action_group (priv->ui_manager,
                                            priv->new_terminal_action_group);
        priv->new_terminal_action_group = NULL;
    }

    profiles = terminal_app_get_profile_list (terminal_app_get ());
    have_single_profile = !profiles || !profiles->next;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "FileNewTab");
    gtk_action_set_visible (action, have_single_profile);
    action = gtk_action_group_get_action (priv->action_group, "FileNewWindow");
    gtk_action_set_visible (action, have_single_profile);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    if (have_single_profile)
    {
        g_list_free (profiles);
        return;
    }

    /* Now build the submenus */

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action_group = priv->new_terminal_action_group = gtk_action_group_new ("NewTerminal");
    G_GNUC_END_IGNORE_DEPRECATIONS;
    gtk_ui_manager_insert_action_group (priv->ui_manager, action_group, -1);
    g_object_unref (action_group);

    priv->new_terminal_ui_id = gtk_ui_manager_new_merge_id (priv->ui_manager);

    n = 0;
    for (p = profiles; p != NULL; p = p->next)
    {
        TerminalProfile *profile = (TerminalProfile *) p->data;
        char name[32];

        g_snprintf (name, sizeof (name), "FileNewTab.%u", n);
        terminal_window_create_new_terminal_action (window,
                profile,
                name,
                n,
                G_CALLBACK (file_new_tab_callback));

        gtk_ui_manager_add_ui (priv->ui_manager, priv->new_terminal_ui_id,
                               FILE_NEW_TERMINAL_TAB_UI_PATH,
                               name, name,
                               GTK_UI_MANAGER_MENUITEM, FALSE);

        g_snprintf (name, sizeof (name), "FileNewWindow.%u", n);
        terminal_window_create_new_terminal_action (window,
                profile,
                name,
                n,
                G_CALLBACK (file_new_window_callback));

        gtk_ui_manager_add_ui (priv->ui_manager, priv->new_terminal_ui_id,
                               FILE_NEW_TERMINAL_WINDOW_UI_PATH,
                               name, name,
                               GTK_UI_MANAGER_MENUITEM, FALSE);

        ++n;
    }

    g_list_free (profiles);
}

static void
terminal_set_encoding_callback (GtkToggleAction *action,
                                TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalEncoding *encoding;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    if (!gtk_toggle_action_get_active (action))
        return;
    G_GNUC_END_IGNORE_DEPRECATIONS;

    if (priv->active_screen == NULL)
        return;

    encoding = g_object_get_data (G_OBJECT (action), ENCODING_DATA_KEY);
    g_assert (encoding);

    vte_terminal_set_encoding (VTE_TERMINAL (priv->active_screen),
                               terminal_encoding_get_charset (encoding), NULL);
}

static void
terminal_window_update_encoding_menu (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalApp *app;
    GtkActionGroup *action_group;
    GSList *group;
    guint n;
    GSList *encodings, *l;
    const char *charset;
    TerminalEncoding *active_encoding;

    /* Remove the old UI */
    if (priv->encodings_ui_id != 0)
    {
        gtk_ui_manager_remove_ui (priv->ui_manager, priv->encodings_ui_id);
        priv->encodings_ui_id = 0;
    }

    if (priv->encodings_action_group != NULL)
    {
        gtk_ui_manager_remove_action_group (priv->ui_manager,
                                            priv->encodings_action_group);
        priv->encodings_action_group = NULL;
    }

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action_group = priv->encodings_action_group = gtk_action_group_new ("Encodings");
    G_GNUC_END_IGNORE_DEPRECATIONS;
    gtk_ui_manager_insert_action_group (priv->ui_manager, action_group, -1);
    g_object_unref (action_group);

    priv->encodings_ui_id = gtk_ui_manager_new_merge_id (priv->ui_manager);

    if (priv->active_screen)
        charset = vte_terminal_get_encoding (VTE_TERMINAL (priv->active_screen));
    else
        charset = "current";

    app = terminal_app_get ();
    active_encoding = terminal_app_ensure_encoding (app, charset);

    encodings = terminal_app_get_active_encodings (app);

    if (g_slist_find (encodings, active_encoding) == NULL)
        encodings = g_slist_append (encodings, terminal_encoding_ref (active_encoding));

    group = NULL;
    n = 0;
    for (l = encodings; l != NULL; l = l->next)
    {
        TerminalEncoding *e = (TerminalEncoding *) l->data;
        GtkRadioAction *encoding_action;
        char name[128];
        char *display_name;

        g_snprintf (name, sizeof (name), SET_ENCODING_ACTION_NAME_PREFIX "%s", terminal_encoding_get_id (e));
        display_name = g_strdup_printf ("%s (%s)", e->name, terminal_encoding_get_charset (e));

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        encoding_action = gtk_radio_action_new (name,
                                                display_name,
                                                NULL,
                                                NULL,
                                                n);
        g_free (display_name);

        gtk_radio_action_set_group (encoding_action, group);
        group = gtk_radio_action_get_group (encoding_action);

        if (charset && strcmp (terminal_encoding_get_id (e), charset) == 0)
            gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (encoding_action), TRUE);
        G_GNUC_END_IGNORE_DEPRECATIONS;

        g_signal_connect (encoding_action, "toggled",
                          G_CALLBACK (terminal_set_encoding_callback), window);

        g_object_set_data_full (G_OBJECT (encoding_action), ENCODING_DATA_KEY,
                                terminal_encoding_ref (e),
                                (GDestroyNotify) terminal_encoding_unref);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        gtk_action_group_add_action (action_group, GTK_ACTION (encoding_action));
        G_GNUC_END_IGNORE_DEPRECATIONS;
        g_object_unref (encoding_action);

        gtk_ui_manager_add_ui (priv->ui_manager, priv->encodings_ui_id,
                               SET_ENCODING_UI_PATH,
                               name, name,
                               GTK_UI_MANAGER_MENUITEM, FALSE);
    }

    g_slist_foreach (encodings, (GFunc) terminal_encoding_unref, NULL);
    g_slist_free (encodings);
}

static void
terminal_window_update_encoding_menu_active_encoding (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;
    char name[128];

    if (!priv->active_screen)
        return;
    if (!priv->encodings_action_group)
        return;

    g_snprintf (name, sizeof (name), SET_ENCODING_ACTION_NAME_PREFIX "%s",
                vte_terminal_get_encoding (VTE_TERMINAL (priv->active_screen)));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->encodings_action_group, name);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    if (!action)
        return;

    g_signal_handlers_block_by_func (action, G_CALLBACK (terminal_set_encoding_callback), window);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action), TRUE);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    g_signal_handlers_unblock_by_func (action, G_CALLBACK (terminal_set_encoding_callback), window);
}

static void
terminal_size_to_cb (GtkAction *action,
                     TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    const char *name;
    char *end = NULL;
    guint width, height;

    if (priv->active_screen == NULL)
        return;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    name = gtk_action_get_name (action) + strlen (SIZE_TO_ACTION_NAME_PREFIX);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    width = g_ascii_strtoull (name, &end, 10);
    g_assert (end && *end == 'x');
    height = g_ascii_strtoull (end + 1, &end, 10);
    g_assert (end && *end == '\0');

    vte_terminal_set_size (VTE_TERMINAL (priv->active_screen), width, height);

    terminal_window_update_size (window, priv->active_screen, TRUE);
}

static void
terminal_window_update_size_to_menu (TerminalWindow *window)
{
    static const struct
    {
        guint grid_width;
        guint grid_height;
    } predefined_sizes[] =
    {
        { 80, 24 },
        { 80, 43 },
        { 132, 24 },
        { 132, 43 }
    };
    TerminalWindowPrivate *priv = window->priv;
    guint i;

    /* We only install this once, so there's no need for a separate action group
     * and any cleanup + build-new-one action here.
     */

    for (i = 0; i < G_N_ELEMENTS (predefined_sizes); ++i)
    {
        guint grid_width = predefined_sizes[i].grid_width;
        guint grid_height = predefined_sizes[i].grid_height;
        GtkAction *action;
        char name[40];
        char *display_name;

        g_snprintf (name, sizeof (name), SIZE_TO_ACTION_NAME_PREFIX "%ux%u",
                    grid_width, grid_height);

        /* If there are ever more than 9 of these, extend this to use A..Z as mnemonics,
         * like we do for the profiles menu.
         */
        display_name = g_strdup_printf ("_%u. %ux%u", i + 1, grid_width, grid_height);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        action = gtk_action_new (name, display_name, NULL, NULL);
        G_GNUC_END_IGNORE_DEPRECATIONS;
        g_free (display_name);

        g_signal_connect (action, "activate",
                          G_CALLBACK (terminal_size_to_cb), window);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        gtk_action_group_add_action (priv->action_group, action);
        G_GNUC_END_IGNORE_DEPRECATIONS;
        g_object_unref (action);

        gtk_ui_manager_add_ui (priv->ui_manager, priv->ui_id,
                               SIZE_TO_UI_PATH,
                               name, name,
                               GTK_UI_MANAGER_MENUITEM, FALSE);
    }
}

/* Actions stuff */

static void
terminal_window_update_copy_sensitivity (TerminalScreen *screen,
                                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;
    gboolean can_copy;

    if (screen != priv->active_screen)
        return;

    can_copy = vte_terminal_get_has_selection (VTE_TERMINAL (screen));

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "EditCopy");
    gtk_action_set_sensitive (action, can_copy);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    if (can_copy && priv->copy_selection)
#if VTE_CHECK_VERSION (0, 50, 0)
        vte_terminal_copy_clipboard_format (VTE_TERMINAL(screen), VTE_FORMAT_TEXT);
#else
        vte_terminal_copy_clipboard(VTE_TERMINAL(screen));
#endif
}

static void
terminal_window_update_zoom_sensitivity (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreen *screen;
    GtkAction *action;
    double current, zoom;

    screen = priv->active_screen;
    if (screen == NULL)
        return;

    current = terminal_screen_get_font_scale (screen);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "ViewZoomOut");
    gtk_action_set_sensitive (action, find_smaller_zoom_factor (current, &zoom));
    action = gtk_action_group_get_action (priv->action_group, "ViewZoomIn");
    gtk_action_set_sensitive (action, find_larger_zoom_factor (current, &zoom));
    G_GNUC_END_IGNORE_DEPRECATIONS;
}

static void
terminal_window_update_search_sensitivity (TerminalScreen *screen,
        TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;
    gboolean can_search;

    if (screen != priv->active_screen)
        return;

    can_search = vte_terminal_search_get_regex (VTE_TERMINAL (screen)) != NULL;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "SearchFindNext");
    gtk_action_set_sensitive (action, can_search);
    action = gtk_action_group_get_action (priv->action_group, "SearchFindPrevious");
    gtk_action_set_sensitive (action, can_search);
    action = gtk_action_group_get_action (priv->action_group, "SearchClearHighlight");
    gtk_action_set_sensitive (action, can_search);
    G_GNUC_END_IGNORE_DEPRECATIONS;
}

static void
update_edit_menu_cb (GtkClipboard *clipboard,
                     GdkAtom *targets,
                     int n_targets,
                     TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;
    gboolean can_paste, can_paste_uris;

    can_paste = targets != NULL && gtk_targets_include_text (targets, n_targets);
    can_paste_uris = targets != NULL && gtk_targets_include_uri (targets, n_targets);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "EditPaste");
    gtk_action_set_sensitive (action, can_paste);
    action = gtk_action_group_get_action (priv->action_group, "EditPasteURIPaths");
    gtk_action_set_visible (action, can_paste_uris);
    gtk_action_set_sensitive (action, can_paste_uris);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    /* Ref was added in gtk_clipboard_request_targets below */
    g_object_unref (window);
}

static void
update_edit_menu(TerminalWindow *window)
{
    GtkClipboard *clipboard;

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_request_targets (clipboard,
                                   (GtkClipboardTargetsReceivedFunc) update_edit_menu_cb,
                                   g_object_ref (window));
}

static void
screen_resize_window_cb (TerminalScreen *screen,
                         guint width,
                         guint height,
                         TerminalWindow* window)
{
    TerminalWindowPrivate *priv = window->priv;
    VteTerminal *terminal = VTE_TERMINAL (screen);
    GtkWidget *widget = GTK_WIDGET (screen);

    /* Don't do anything if we're maximised or fullscreened */
    // FIXME: realized && ... instead?
    if (!gtk_widget_get_realized (widget) ||
            (gdk_window_get_state (gtk_widget_get_window (widget)) & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) != 0)
        return;

    vte_terminal_set_size (terminal, width, height);

    if (screen != priv->active_screen)
        return;

    terminal_window_update_size (window, screen, TRUE);
}

static void
terminal_window_update_tabs_menu_sensitivity (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkNotebook *notebook = GTK_NOTEBOOK (priv->notebook);
    GtkActionGroup *action_group = priv->action_group;
    GtkAction *action;
    int num_pages, page_num;
    gboolean not_first, not_last;

    if (priv->disposed)
        return;

    num_pages = gtk_notebook_get_n_pages (notebook);
    page_num = gtk_notebook_get_current_page (notebook);
    not_first = page_num > 0;
    not_last = page_num + 1 < num_pages;

    /* Hide the tabs menu in single-tab windows */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (action_group, "Tabs");
    gtk_action_set_visible (action, num_pages > 1);

#if 1
    /* NOTE: We always make next/prev actions sensitive except in
     * single-tab windows, so the corresponding shortcut key escape code
     * isn't sent to the terminal. See bug #453193 and bug #138609.
     * This also makes tab cycling work, bug #92139.
     * FIXME: Find a better way to do this.
     */
    action = gtk_action_group_get_action (action_group, "TabsPrevious");
    gtk_action_set_sensitive (action, num_pages > 1);
    action = gtk_action_group_get_action (action_group, "TabsNext");
    gtk_action_set_sensitive (action, num_pages > 1);
#else
    /* This would be correct, but see the comment above. */
    action = gtk_action_group_get_action (action_group, "TabsPrevious");
    gtk_action_set_sensitive (action, not_first);
    action = gtk_action_group_get_action (action_group, "TabsNext");
    gtk_action_set_sensitive (action, not_last);
#endif

    action = gtk_action_group_get_action (action_group, "TabsMoveLeft");
    gtk_action_set_sensitive (action, not_first);
    action = gtk_action_group_get_action (action_group, "TabsMoveRight");
    gtk_action_set_sensitive (action, not_last);
    action = gtk_action_group_get_action (action_group, "TabsDetach");
    gtk_action_set_sensitive (action, num_pages > 1);
    action = gtk_action_group_get_action (action_group, "FileCloseTab");
    gtk_action_set_sensitive (action, num_pages > 1);
    G_GNUC_END_IGNORE_DEPRECATIONS;
}

static void
update_tab_visibility (TerminalWindow *window,
                       int             change)
{
    TerminalWindowPrivate *priv = window->priv;
    gboolean show_tabs;
    guint num;

    num = gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->notebook));

    show_tabs = (num + change) > 1;
    gtk_notebook_set_show_tabs (GTK_NOTEBOOK (priv->notebook), show_tabs);
}

static GtkNotebook *
handle_tab_droped_on_desktop (GtkNotebook *source_notebook,
                              GtkWidget   *container,
                              gint         x,
                              gint         y,
                              gpointer     data)
{
    TerminalWindow *source_window;
    TerminalWindow *new_window;
    TerminalWindowPrivate *new_priv;

    source_window = TERMINAL_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (source_notebook)));
    g_return_val_if_fail (TERMINAL_IS_WINDOW (source_window), NULL);

    new_window = terminal_app_new_window (terminal_app_get (),
                                          gtk_widget_get_screen (GTK_WIDGET (source_window)));
    new_priv = new_window->priv;
    new_priv->present_on_insert = TRUE;

    update_tab_visibility (source_window, -1);
    update_tab_visibility (new_window, +1);

    return GTK_NOTEBOOK (new_priv->notebook);
}

/* Terminal screen popup menu handling */

static void
popup_open_url_callback (GtkAction *action,
                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreenPopupInfo *info = priv->popup_info;

    if (info == NULL)
        return;

    terminal_util_open_url (GTK_WIDGET (window), info->string, info->flavour,
                            gtk_get_current_event_time ());
}

static void
popup_copy_url_callback (GtkAction *action,
                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreenPopupInfo *info = priv->popup_info;
    GtkClipboard *clipboard;

    if (info == NULL)
        return;

    if (info->string == NULL)
        return;

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text (clipboard, info->string, -1);
}

static void
popup_leave_fullscreen_callback (GtkAction *action,
                                 TerminalWindow *window)
{
    gtk_window_unfullscreen (GTK_WINDOW (window));
}

static void
remove_popup_info (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (priv->remove_popup_info_idle != 0)
    {
        g_source_remove (priv->remove_popup_info_idle);
        priv->remove_popup_info_idle = 0;
    }

    if (priv->popup_info != NULL)
    {
        terminal_screen_popup_info_unref (priv->popup_info);
        priv->popup_info = NULL;
    }
}

static gboolean
idle_remove_popup_info (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    priv->remove_popup_info_idle = 0;
    remove_popup_info (window);
    return FALSE;
}

static void
unset_popup_info (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    /* Unref the event from idle since we still need it
     * from the action callbacks which will run before idle.
     */
    if (priv->remove_popup_info_idle == 0 &&
            priv->popup_info != NULL)
    {
        priv->remove_popup_info_idle =
            g_idle_add ((GSourceFunc) idle_remove_popup_info, window);
    }
}

static void
popup_menu_deactivate_callback (GtkWidget *popup,
                                TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *im_menu_item;

    g_signal_handlers_disconnect_by_func
    (popup, G_CALLBACK (popup_menu_deactivate_callback), window);

    im_menu_item = gtk_ui_manager_get_widget (priv->ui_manager,
                   "/Popup/PopupInputMethods");
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (im_menu_item), NULL);

    unset_popup_info (window);
}

static void
popup_clipboard_targets_received_cb (GtkClipboard *clipboard,
                                     GdkAtom *targets,
                                     int n_targets,
                                     TerminalScreenPopupInfo *info)
{
    TerminalWindow *window = info->window;
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreen *screen = info->screen;
    GtkWidget *popup_menu;
    GtkAction *action;
    gboolean can_paste, can_paste_uris, show_link, show_email_link, show_call_link, show_input_method_menu;
    int n_pages;

    if (!gtk_widget_get_realized (GTK_WIDGET (screen)))
    {
        terminal_screen_popup_info_unref (info);
        return;
    }

    /* Now we know that the screen is realized, we know that the window is still alive */
    remove_popup_info (window);

    priv->popup_info = info; /* adopt the ref added when requesting the clipboard */

    n_pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->notebook));

    can_paste = targets != NULL && gtk_targets_include_text (targets, n_targets);
    can_paste_uris = targets != NULL && gtk_targets_include_uri (targets, n_targets);
    show_link = info->string != NULL && (info->flavour == FLAVOR_AS_IS || info->flavour == FLAVOR_DEFAULT_TO_HTTP);
    show_email_link = info->string != NULL && info->flavour == FLAVOR_EMAIL;
    show_call_link = info->string != NULL && info->flavour == FLAVOR_VOIP_CALL;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "PopupSendEmail");
    gtk_action_set_visible (action, show_email_link);
    action = gtk_action_group_get_action (priv->action_group, "PopupCopyEmailAddress");
    gtk_action_set_visible (action, show_email_link);
    action = gtk_action_group_get_action (priv->action_group, "PopupCall");
    gtk_action_set_visible (action, show_call_link);
    action = gtk_action_group_get_action (priv->action_group, "PopupCopyCallAddress");
    gtk_action_set_visible (action, show_call_link);
    action = gtk_action_group_get_action (priv->action_group, "PopupOpenLink");
    gtk_action_set_visible (action, show_link);
    action = gtk_action_group_get_action (priv->action_group, "PopupCopyLinkAddress");
    gtk_action_set_visible (action, show_link);

    action = gtk_action_group_get_action (priv->action_group, "PopupCloseWindow");
    gtk_action_set_visible (action, n_pages <= 1);
    action = gtk_action_group_get_action (priv->action_group, "PopupCloseTab");
    gtk_action_set_visible (action, n_pages > 1);

    action = gtk_action_group_get_action (priv->action_group, "PopupCopy");
    gtk_action_set_sensitive (action, vte_terminal_get_has_selection (VTE_TERMINAL (screen)));
    action = gtk_action_group_get_action (priv->action_group, "PopupPaste");
    gtk_action_set_sensitive (action, can_paste);
    action = gtk_action_group_get_action (priv->action_group, "PopupPasteURIPaths");
    gtk_action_set_visible (action, can_paste_uris);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    g_object_get (gtk_widget_get_settings (GTK_WIDGET (window)),
                  "gtk-show-input-method-menu", &show_input_method_menu,
                  NULL);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "PopupInputMethods");
    gtk_action_set_visible (action, show_input_method_menu);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    popup_menu = gtk_ui_manager_get_widget (priv->ui_manager, "/Popup");
    g_signal_connect (popup_menu, "deactivate",
                      G_CALLBACK (popup_menu_deactivate_callback), window);

    /* Pseudo activation of the popup menu's action */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "Popup");
    gtk_action_activate (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    if (info->button == 0)
        gtk_menu_shell_select_first (GTK_MENU_SHELL (popup_menu), FALSE);

    if (!gtk_menu_get_attach_widget (GTK_MENU (popup_menu)))
        gtk_menu_attach_to_widget (GTK_MENU (popup_menu),GTK_WIDGET (screen),NULL);

    gtk_menu_popup (GTK_MENU (popup_menu),
                    NULL, NULL,
                    NULL, NULL,
                    info->button,
                    info->timestamp);
}

static void
screen_show_popup_menu_callback (TerminalScreen *screen,
                                 TerminalScreenPopupInfo *info,
                                 TerminalWindow *window)
{
    GtkClipboard *clipboard;

    g_return_if_fail (info->window == window);

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_request_targets (clipboard,
                                   (GtkClipboardTargetsReceivedFunc) popup_clipboard_targets_received_cb,
                                   terminal_screen_popup_info_ref (info));
}

static gboolean
screen_match_clicked_cb (TerminalScreen *screen,
                         const char *match,
                         int flavour,
                         guint state,
                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (screen != priv->active_screen)
        return FALSE;

    switch (flavour)
    {
#ifdef ENABLE_SKEY
    case FLAVOR_SKEY:
        terminal_skey_do_popup (GTK_WINDOW (window), screen, match);
        break;
#endif
    default:
        gtk_widget_grab_focus (GTK_WIDGET (screen));
        terminal_util_open_url (GTK_WIDGET (window), match, flavour,
                                gtk_get_current_event_time ());
        break;
    }

    return TRUE;
}

static void
screen_close_cb (TerminalScreen *screen,
                 TerminalWindow *window)
{
    terminal_window_remove_screen (window, screen);
}

static gboolean
terminal_window_accel_activate_cb (GtkAccelGroup  *accel_group,
                                   GObject        *acceleratable,
                                   guint           keyval,
                                   GdkModifierType modifier,
                                   TerminalWindow *window)
{
    GtkAccelGroupEntry *entries;
    guint n_entries;
    gboolean retval = FALSE;

    entries = gtk_accel_group_query (accel_group, keyval, modifier, &n_entries);
    if (n_entries > 0)
    {
        const char *accel_path;

        accel_path = g_quark_to_string (entries[0].accel_path_quark);

        if (g_str_has_prefix (accel_path, "<Actions>/Main/"))
        {
            const char *action_name;

            /* We want to always consume these accelerators, even if the corresponding
             * action is insensitive, so the corresponding shortcut key escape code
             * isn't sent to the terminal. See bug #453193, bug #138609 and bug #559728.
             * This also makes tab cycling work, bug #92139. (NOT!)
             */

            action_name = I_(accel_path + strlen ("<Actions>/Main/"));

#if 0
            if (gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->notebook)) > 1 &&
                    (action_name == I_("TabsPrevious") || action_name == I_("TabsNext")))
                retval = TRUE;
            else
#endif
                if (action_name == I_("EditCopy") ||
                        action_name == I_("PopupCopy") ||
                        action_name == I_("EditPaste") ||
                        action_name == I_("PopupPaste"))
                    retval = TRUE;
        }
    }

    return retval;
}

/*****************************************/

#ifdef MATE_ENABLE_DEBUG
static void
terminal_window_size_allocate_cb (GtkWidget *widget,
                                  GtkAllocation *allocation)
{
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] size-alloc result %d : %d at (%d, %d)\n",
                           widget,
                           allocation->width, allocation->height,
                           allocation->x, allocation->y);
}
#endif /* MATE_ENABLE_DEBUG */

static void
terminal_window_realize (GtkWidget *widget)
{
    TerminalWindow *window = TERMINAL_WINDOW (widget);
    TerminalWindowPrivate *priv = window->priv;
#if defined(GDK_WINDOWING_X11) || defined(GDK_WINDOWING_WAYLAND)
    GdkScreen *screen;
    GtkAllocation widget_allocation;
    GdkVisual *visual;

    gtk_widget_get_allocation (widget, &widget_allocation);
    screen = gtk_widget_get_screen (GTK_WIDGET (window));

    if (gdk_screen_is_composited (screen) &&
        (visual = gdk_screen_get_rgba_visual (screen)) != NULL)
    {
          /* Set RGBA visual if possible so VTE can use real transparency */
        gtk_widget_set_visual (GTK_WIDGET (widget), visual);
        priv->have_argb_visual = TRUE;
    }
    else
    {
        gtk_widget_set_visual (GTK_WIDGET (window), gdk_screen_get_system_visual (screen));
        priv->have_argb_visual = FALSE;
    }
#endif

    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] realize, size %d : %d at (%d, %d)\n",
                           widget,
                           widget_allocation.width, widget_allocation.height,
                           widget_allocation.x, widget_allocation.y);

    GTK_WIDGET_CLASS (terminal_window_parent_class)->realize (widget);

    /* Need to do this now since this requires the window to be realized */
    if (priv->active_screen != NULL)
        sync_screen_icon_title (priv->active_screen, NULL, window);
}

static gboolean
terminal_window_map_event (GtkWidget    *widget,
                           GdkEventAny  *event)
{
    TerminalWindow *window = TERMINAL_WINDOW (widget);
    TerminalWindowPrivate *priv = window->priv;
    gboolean (* map_event) (GtkWidget *, GdkEventAny *) =
        GTK_WIDGET_CLASS (terminal_window_parent_class)->map_event;
    GtkAllocation widget_allocation;

    gtk_widget_get_allocation (widget, &widget_allocation);
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] map-event, size %d : %d at (%d, %d)\n",
                           widget,
                           widget_allocation.width, widget_allocation.height,
                           widget_allocation.x, widget_allocation.y);

    if (priv->clear_demands_attention)
    {
#ifdef GDK_WINDOWING_X11
        terminal_util_x11_clear_demands_attention (gtk_widget_get_window (widget));
#endif

        priv->clear_demands_attention = FALSE;
    }

    if (map_event)
        return map_event (widget, event);

    return FALSE;
}

static gboolean
terminal_window_state_event (GtkWidget            *widget,
                             GdkEventWindowState  *event)
{
    gboolean (* window_state_event) (GtkWidget *, GdkEventWindowState *event) =
        GTK_WIDGET_CLASS (terminal_window_parent_class)->window_state_event;

    if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN)
    {
        TerminalWindow *window = TERMINAL_WINDOW (widget);
        TerminalWindowPrivate *priv = window->priv;
        GtkAction *action;
        gboolean is_fullscreen;

        is_fullscreen = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
        action = gtk_action_group_get_action (priv->action_group, "ViewFullscreen");
        gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action), is_fullscreen);

        action = gtk_action_group_get_action (priv->action_group, "PopupLeaveFullscreen");
        gtk_action_set_visible (action, is_fullscreen);
        G_GNUC_END_IGNORE_DEPRECATIONS;
    }

    if (window_state_event)
        return window_state_event (widget, event);

    return FALSE;
}

#ifdef GDK_WINDOWING_X11
static void
terminal_window_window_manager_changed_cb (GdkScreen *screen,
        TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;
    gboolean supports_fs;

    supports_fs = gdk_x11_screen_supports_net_wm_hint (screen, gdk_atom_intern ("_NET_WM_STATE_FULLSCREEN", FALSE));

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "ViewFullscreen");
    gtk_action_set_sensitive (action, supports_fs);
    G_GNUC_END_IGNORE_DEPRECATIONS;
}
#endif

static void
terminal_window_screen_update (TerminalWindow *window,
                               GdkScreen *screen)
{
    TerminalApp *app;

#ifdef GDK_WINDOWING_X11
    if (screen && GDK_IS_X11_SCREEN (screen))
    {
        terminal_window_window_manager_changed_cb (screen, window);
        g_signal_connect (screen, "window-manager-changed",
                          G_CALLBACK (terminal_window_window_manager_changed_cb), window);
    }
#endif

    if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (screen), "GT::HasSettingsConnection")))
        return;

    g_object_set_data_full (G_OBJECT (screen), "GT::HasSettingsConnection",
                            GINT_TO_POINTER (TRUE),
                            (GDestroyNotify) app_setting_notify_destroy_cb);

    app = terminal_app_get ();
    app_setting_notify_cb (app, NULL, screen);
    g_signal_connect (app, "notify::" TERMINAL_APP_ENABLE_MNEMONICS,
                      G_CALLBACK (app_setting_notify_cb), screen);
    g_signal_connect (app, "notify::" TERMINAL_APP_ENABLE_MENU_BAR_ACCEL,
                      G_CALLBACK (app_setting_notify_cb), screen);
}

static void
terminal_window_screen_changed (GtkWidget *widget,
                                GdkScreen *previous_screen)
{
    TerminalWindow *window = TERMINAL_WINDOW (widget);
    void (* screen_changed) (GtkWidget *, GdkScreen *) =
        GTK_WIDGET_CLASS (terminal_window_parent_class)->screen_changed;
    GdkScreen *screen;

    if (screen_changed)
        screen_changed (widget, previous_screen);

    screen = gtk_widget_get_screen (widget);
    if (previous_screen == screen)
        return;

#ifdef GDK_WINDOWING_X11
    if (previous_screen && GDK_IS_X11_SCREEN (previous_screen))
    {
        g_signal_handlers_disconnect_by_func (previous_screen,
                                              G_CALLBACK (terminal_window_window_manager_changed_cb),
                                              window);
    }
#endif

    if (!screen)
        return;

    terminal_window_screen_update (window, screen);
}

static void
terminal_window_profile_list_changed_cb (TerminalApp *app,
        TerminalWindow *window)
{
    terminal_window_update_set_profile_menu (window);
    terminal_window_update_new_terminal_menus (window);
}

static void
terminal_window_encoding_list_changed_cb (TerminalApp *app,
        TerminalWindow *window)
{
    terminal_window_update_encoding_menu (window);
}

static void
terminal_window_init (TerminalWindow *window)
{
    const GtkActionEntry menu_entries[] =
    {
        /* Toplevel */
        { "File", NULL, N_("_File"), NULL, NULL, NULL },
        { "FileNewWindowProfiles", "utilities-terminal", N_("Open _Terminal"), NULL, NULL, NULL },
        { "FileNewTabProfiles", STOCK_NEW_TAB, N_("Open Ta_b"), NULL, NULL, NULL },
        { "Edit", NULL, N_("_Edit"), NULL, NULL, NULL },
        { "View", NULL, N_("_View"), NULL, NULL, NULL },
        { "Search", NULL, N_("_Search"), NULL, NULL, NULL },
        { "Terminal", NULL, N_("_Terminal"), NULL, NULL, NULL },
        { "Tabs", NULL, N_("Ta_bs"), NULL, NULL, NULL },
        { "Help", NULL, N_("_Help"), NULL, NULL, NULL },
        { "Popup", NULL, NULL, NULL, NULL, NULL },
        { "NotebookPopup", NULL, "", NULL, NULL, NULL },

        /* File menu */
        {
            "FileNewWindow", "utilities-terminal", N_("Open _Terminal"), "<shift><control>N",
            NULL,
            G_CALLBACK (file_new_window_callback)
        },
        {
            "FileNewTab", STOCK_NEW_TAB, N_("Open Ta_b"), "<shift><control>T",
            NULL,
            G_CALLBACK (file_new_tab_callback)
        },
        {
            "FileNewProfile", "document-open", N_("New _Profile…"), "",
            NULL,
            G_CALLBACK (file_new_profile_callback)
        },
        {
            "FileSaveContents", "document-save", N_("_Save Contents"), "",
            NULL,
            G_CALLBACK (file_save_contents_callback)
        },
        {
            "FileCloseTab", "window-close", N_("C_lose Tab"), "<shift><control>W",
            NULL,
            G_CALLBACK (file_close_tab_callback)
        },
        {
            "FileCloseWindow", "window-close", N_("_Close Window"), "<shift><control>Q",
            NULL,
            G_CALLBACK (file_close_window_callback)
        },

        /* Edit menu */
        {
            "EditCopy", "edit-copy", N_("_Copy"), "<shift><control>C",
            NULL,
            G_CALLBACK (edit_copy_callback)
        },
        {
            "EditPaste", "edit-paste", N_("_Paste"), "<shift><control>V",
            NULL,
            G_CALLBACK (edit_paste_callback)
        },
        {
            "EditPasteURIPaths", "edit-paste", N_("Paste _Filenames"), "",
            NULL,
            G_CALLBACK (edit_paste_callback)
        },
        {
            "EditSelectAll", "edit-select-all", N_("Select _All"), "<shift><control>A",
            NULL,
            G_CALLBACK (edit_select_all_callback)
        },
        {
            "EditProfiles", NULL, N_("P_rofiles…"), NULL,
            NULL,
            G_CALLBACK (edit_profiles_callback)
        },
        {
            "EditKeybindings", NULL, N_("_Keyboard Shortcuts…"), NULL,
            NULL,
            G_CALLBACK (edit_keybindings_callback)
        },
        {
            "EditCurrentProfile", NULL, N_("Pr_ofile Preferences"), NULL,
            NULL,
            G_CALLBACK (edit_current_profile_callback)
        },

        /* View menu */
        {
            "ViewZoomIn", "zoom-in", N_("Zoom _In"), "<control>plus",
            NULL,
            G_CALLBACK (view_zoom_in_callback)
        },
        {
            "ViewZoomOut", "zoom-out", N_("Zoom _Out"), "<control>minus",
            NULL,
            G_CALLBACK (view_zoom_out_callback)
        },
        {
            "ViewZoom100", "zoom-original", N_("_Normal Size"), "<control>0",
            NULL,
            G_CALLBACK (view_zoom_normal_callback)
        },

        /* Search menu */
        {
            "SearchFind", "edit-find", N_("_Find..."), "<shift><control>F",
            NULL,
            G_CALLBACK (search_find_callback)
        },
        {
            "SearchFindNext", NULL, N_("Find Ne_xt"), "<shift><control>H",
            NULL,
            G_CALLBACK (search_find_next_callback)
        },
        {
            "SearchFindPrevious", NULL, N_("Find Pre_vious"), "<shift><control>G",
            NULL,
            G_CALLBACK (search_find_prev_callback)
        },
        {
            "SearchClearHighlight", NULL, N_("_Clear Highlight"), "<shift><control>J",
            NULL,
            G_CALLBACK (search_clear_highlight_callback)
        },
#if 0
        {
            "SearchGoToLine", "go-jump", N_("Go to _Line..."), "<shift><control>I",
            NULL,
            G_CALLBACK (search_goto_line_callback)
        },
        {
            "SearchIncrementalSearch", "edit-find", N_("_Incremental Search..."), "<shift><control>K",
            NULL,
            G_CALLBACK (search_incremental_search_callback)
        },
#endif

        /* Terminal menu */
        { "TerminalProfiles", NULL, N_("Change _Profile"), NULL, NULL, NULL },
        {
            "ProfilePrevious", NULL, N_("_Previous Profile"), "<alt>Page_Up",
            NULL,
            G_CALLBACK (terminal_next_or_previous_profile_cb)
        },
        {
            "ProfileNext", NULL, N_("_Next Profile"), "<alt>Page_Down",
            NULL,
            G_CALLBACK (terminal_next_or_previous_profile_cb)
        },
        {
            "TerminalSetTitle", NULL, N_("_Set Title…"), NULL,
            NULL,
            G_CALLBACK (terminal_set_title_callback)
        },
        { "TerminalSetEncoding", NULL, N_("Set _Character Encoding"), NULL, NULL, NULL },
        {
            "TerminalReset", NULL, N_("_Reset"), NULL,
            NULL,
            G_CALLBACK (terminal_reset_callback)
        },
        {
            "TerminalResetClear", NULL, N_("Reset and C_lear"), NULL,
            NULL,
            G_CALLBACK (terminal_reset_clear_callback)
        },

        /* Terminal/Encodings menu */
        {
            "TerminalAddEncoding", NULL, N_("_Add or Remove…"), NULL,
            NULL,
            G_CALLBACK (terminal_add_encoding_callback)
        },

        /* Tabs menu */
        {
            "TabsPrevious", NULL, N_("_Previous Tab"), "<control>Page_Up",
            NULL,
            G_CALLBACK (tabs_next_or_previous_tab_cb)
        },
        {
            "TabsNext", NULL, N_("_Next Tab"), "<control>Page_Down",
            NULL,
            G_CALLBACK (tabs_next_or_previous_tab_cb)
        },
        {
            "TabsMoveLeft", NULL, N_("Move Tab _Left"), "<shift><control>Page_Up",
            NULL,
            G_CALLBACK (tabs_move_left_callback)
        },
        {
            "TabsMoveRight", NULL, N_("Move Tab _Right"), "<shift><control>Page_Down",
            NULL,
            G_CALLBACK (tabs_move_right_callback)
        },
        {
            "TabsDetach", NULL, N_("_Detach tab"), NULL,
            NULL,
            G_CALLBACK (tabs_detach_tab_callback)
        },

        /* Help menu */
        {
            "HelpContents", "help-browser", N_("_Contents"), "F1",
            NULL,
            G_CALLBACK (help_contents_callback)
        },
        {
            "HelpAbout", "help-about", N_("_About"), NULL,
            NULL,
            G_CALLBACK (help_about_callback)
        },

        /* Popup menu */
        {
            "PopupSendEmail", NULL, N_("_Send Mail To…"), NULL,
            NULL,
            G_CALLBACK (popup_open_url_callback)
        },
        {
            "PopupCopyEmailAddress", NULL, N_("_Copy E-mail Address"), NULL,
            NULL,
            G_CALLBACK (popup_copy_url_callback)
        },
        {
            "PopupCall", NULL, N_("C_all To…"), NULL,
            NULL,
            G_CALLBACK (popup_open_url_callback)
        },
        {
            "PopupCopyCallAddress", NULL, N_("_Copy Call Address"), NULL,
            NULL,
            G_CALLBACK (popup_copy_url_callback)
        },
        {
            "PopupOpenLink", NULL, N_("_Open Link"), NULL,
            NULL,
            G_CALLBACK (popup_open_url_callback)
        },
        {
            "PopupCopyLinkAddress", NULL, N_("_Copy Link Address"), NULL,
            NULL,
            G_CALLBACK (popup_copy_url_callback)
        },
        { "PopupTerminalProfiles", NULL, N_("P_rofiles"), NULL, NULL, NULL },
        {
            "PopupCopy", "edit-copy", N_("_Copy"), "",
            NULL,
            G_CALLBACK (edit_copy_callback)
        },
        {
            "PopupPaste", "edit-paste", N_("_Paste"), "",
            NULL,
            G_CALLBACK (edit_paste_callback)
        },
        {
            "PopupPasteURIPaths", "edit-paste", N_("Paste _Filenames"), "",
            NULL,
            G_CALLBACK (edit_paste_callback)
        },
        {
            "PopupNewTerminal", "utilities-terminal", N_("Open _Terminal"), NULL,
            NULL,
            G_CALLBACK (file_new_window_callback)
        },
        {
            "PopupNewTab", "tab-new", N_("Open Ta_b"), NULL,
            NULL,
            G_CALLBACK (file_new_tab_callback)
        },
        {
            "PopupCloseWindow", "window-close", N_("C_lose Window"), NULL,
            NULL,
            G_CALLBACK (file_close_window_callback)
        },
        {
            "PopupCloseTab", "window-close", N_("C_lose Tab"), NULL,
            NULL,
            G_CALLBACK (file_close_tab_callback)
        },
        {
            "PopupLeaveFullscreen", NULL, N_("L_eave Full Screen"), NULL,
            NULL,
            G_CALLBACK (popup_leave_fullscreen_callback)
        },
        { "PopupInputMethods", NULL, N_("_Input Methods"), NULL, NULL, NULL }
    };

    const GtkToggleActionEntry toggle_menu_entries[] =
    {
        /* View Menu */
        {
            "ViewMenubar", NULL, N_("Show _Menubar"), NULL,
            NULL,
            G_CALLBACK (view_menubar_toggled_callback),
            FALSE
        },
        {
            "ViewFullscreen", NULL, N_("_Full Screen"), NULL,
            NULL,
            G_CALLBACK (view_fullscreen_toggled_callback),
            FALSE
        }
    };
    TerminalWindowPrivate *priv;
    TerminalApp *app;
    GtkActionGroup *action_group;
    GtkAction *action;
    GtkUIManager *manager;
    GError *error;
    GtkWindowGroup *window_group;
    GtkAccelGroup *accel_group;
    GtkClipboard *clipboard;

    priv = window->priv = terminal_window_get_instance_private (window);

    g_signal_connect (G_OBJECT (window), "delete_event",
                      G_CALLBACK(terminal_window_delete_event),
                      NULL);
    g_signal_connect (G_OBJECT (window), "focus_in_event",
                      G_CALLBACK(terminal_window_focus_in_event),
                      NULL);

#ifdef MATE_ENABLE_DEBUG
    _TERMINAL_DEBUG_IF (TERMINAL_DEBUG_GEOMETRY)
    {
        g_signal_connect_after (window, "size-allocate", G_CALLBACK (terminal_window_size_allocate_cb), NULL);
    }
#endif

    GtkStyleContext *context;

    context = gtk_widget_get_style_context (GTK_WIDGET (window));
    gtk_style_context_add_class (context, "mate-terminal");

    gtk_window_set_title (GTK_WINDOW (window), _("Terminal"));

    priv->active_screen = NULL;
    priv->menubar_visible = FALSE;

    priv->main_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (window), priv->main_vbox);
    gtk_widget_show (priv->main_vbox);

    priv->notebook = gtk_notebook_new ();
    gtk_notebook_set_scrollable (GTK_NOTEBOOK (priv->notebook), TRUE);
    gtk_notebook_set_show_border (GTK_NOTEBOOK (priv->notebook), FALSE);
    gtk_notebook_set_show_tabs (GTK_NOTEBOOK (priv->notebook), FALSE);
    gtk_notebook_set_group_name (GTK_NOTEBOOK (priv->notebook), I_("mate-terminal-window"));
    g_signal_connect (priv->notebook, "button-press-event",
                      G_CALLBACK (notebook_button_press_cb), settings_global);
    g_signal_connect (window, "key-press-event",
                      G_CALLBACK (window_key_press_cb), settings_global);
    g_signal_connect (priv->notebook, "popup-menu",
                      G_CALLBACK (notebook_popup_menu_cb), window);
    g_signal_connect_after (priv->notebook, "switch-page",
                            G_CALLBACK (notebook_page_selected_callback), window);
    g_signal_connect_after (priv->notebook, "page-added",
                            G_CALLBACK (notebook_page_added_callback), window);
    g_signal_connect_after (priv->notebook, "page-removed",
                            G_CALLBACK (notebook_page_removed_callback), window);
    g_signal_connect_data (priv->notebook, "page-reordered",
                           G_CALLBACK (terminal_window_update_tabs_menu_sensitivity),
                           window, NULL, G_CONNECT_SWAPPED | G_CONNECT_AFTER);

    gtk_widget_add_events (priv->notebook, GDK_SCROLL_MASK);
    g_signal_connect (priv->notebook, "scroll-event",
                            G_CALLBACK (notebook_scroll_event_cb), window);

    g_signal_connect (priv->notebook, "create-window",
                    G_CALLBACK (handle_tab_droped_on_desktop), window);

    gtk_box_pack_end (GTK_BOX (priv->main_vbox), priv->notebook, TRUE, TRUE, 0);
    gtk_widget_show (priv->notebook);

    priv->old_char_width = -1;
    priv->old_char_height = -1;

    priv->old_chrome_width = -1;
    priv->old_chrome_height = -1;
    priv->old_padding_width = -1;
    priv->old_padding_height = -1;

    priv->old_geometry_widget = NULL;

    /* Create the UI manager */
    manager = priv->ui_manager = gtk_ui_manager_new ();

    accel_group = gtk_ui_manager_get_accel_group (manager);
    gtk_window_add_accel_group (GTK_WINDOW (window), accel_group);
    /* Workaround for bug #453193, bug #138609 and bug #559728 */
    g_signal_connect_after (accel_group, "accel-activate",
                            G_CALLBACK (terminal_window_accel_activate_cb), window);

    /* Create the actions */
    /* Note that this action group name is used in terminal-accels.c; do not change it */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    priv->action_group = action_group = gtk_action_group_new ("Main");
    gtk_action_group_set_translation_domain (action_group, NULL);
    gtk_action_group_add_actions (action_group, menu_entries,
                                  G_N_ELEMENTS (menu_entries), window);
    gtk_action_group_add_toggle_actions (action_group,
                                         toggle_menu_entries,
                                         G_N_ELEMENTS (toggle_menu_entries),
                                         window);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    gtk_ui_manager_insert_action_group (manager, action_group, 0);
    g_object_unref (action_group);

   clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), GDK_SELECTION_CLIPBOARD);
   g_signal_connect_swapped (clipboard, "owner-change",
                             G_CALLBACK (update_edit_menu), window);
   update_edit_menu (window);
    /* Idem for this action, since the window is not fullscreen. */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "PopupLeaveFullscreen");
    gtk_action_set_visible (action, FALSE);

#ifndef ENABLE_SAVE
    action = gtk_action_group_get_action (priv->action_group, "FileSaveContents");
    gtk_action_set_visible (action, FALSE);
#endif
    G_GNUC_END_IGNORE_DEPRECATIONS;

    /* Load the UI */
    error = NULL;
    priv->ui_id = gtk_ui_manager_add_ui_from_resource (manager,
                  TERMINAL_RESOURCES_PATH_PREFIX G_DIR_SEPARATOR_S "ui/terminal.xml",
                  &error);
    g_assert_no_error (error);

    priv->menubar = gtk_ui_manager_get_widget (manager, "/menubar");
    gtk_box_pack_start (GTK_BOX (priv->main_vbox),
                        priv->menubar,
                        FALSE, FALSE, 0);

    /* Add tabs menu */
    priv->tabs_menu = terminal_tabs_menu_new (window);

    app = terminal_app_get ();
    terminal_window_profile_list_changed_cb (app, window);
    g_signal_connect (app, "profile-list-changed",
                      G_CALLBACK (terminal_window_profile_list_changed_cb), window);

    terminal_window_encoding_list_changed_cb (app, window);
    g_signal_connect (app, "encoding-list-changed",
                      G_CALLBACK (terminal_window_encoding_list_changed_cb), window);

    terminal_window_set_menubar_visible (window, TRUE);
    priv->use_default_menubar_visibility = TRUE;

    terminal_window_update_size_to_menu (window);

    /* We have to explicitly call this, since screen-changed is NOT
     * emitted for the toplevel the first time!
     */
    terminal_window_screen_update (window, gtk_widget_get_screen (GTK_WIDGET (window)));

    window_group = gtk_window_group_new ();
    gtk_window_group_add_window (window_group, GTK_WINDOW (window));
    g_object_unref (window_group);

    terminal_util_set_unique_role (GTK_WINDOW (window), "mate-terminal-window");
}

static void
terminal_window_class_init (TerminalWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose = terminal_window_dispose;
    object_class->finalize = terminal_window_finalize;

    widget_class->show = terminal_window_show;
    widget_class->realize = terminal_window_realize;
    widget_class->map_event = terminal_window_map_event;
    widget_class->window_state_event = terminal_window_state_event;
    widget_class->screen_changed = terminal_window_screen_changed;
}

static void
terminal_window_dispose (GObject *object)
{
    TerminalWindow *window = TERMINAL_WINDOW (object);
    TerminalWindowPrivate *priv = window->priv;
    TerminalApp *app;
    GdkScreen *screen;
    GtkClipboard *clipboard;

    remove_popup_info (window);

    priv->disposed = TRUE;

    if (priv->tabs_menu)
    {
        g_object_unref (priv->tabs_menu);
        priv->tabs_menu = NULL;
    }

    if (priv->profiles_action_group != NULL)
        disconnect_profiles_from_actions_in_group (priv->profiles_action_group);
    if (priv->new_terminal_action_group != NULL)
        disconnect_profiles_from_actions_in_group (priv->new_terminal_action_group);

    app = terminal_app_get ();
    g_signal_handlers_disconnect_by_func (app,
                                          G_CALLBACK (terminal_window_profile_list_changed_cb),
                                          window);
    g_signal_handlers_disconnect_by_func (app,
                                          G_CALLBACK (terminal_window_encoding_list_changed_cb),
                                          window);
    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), GDK_SELECTION_CLIPBOARD);
    g_signal_handlers_disconnect_by_func (clipboard,
                                          G_CALLBACK (update_edit_menu),
                                          window);

#ifdef GDK_WINDOWING_X11
    screen = gtk_widget_get_screen (GTK_WIDGET (object));
    if (screen && GDK_IS_X11_SCREEN (screen))
    {
        g_signal_handlers_disconnect_by_func (screen,
                                              G_CALLBACK (terminal_window_window_manager_changed_cb),
                                              window);
    }
#endif

    G_OBJECT_CLASS (terminal_window_parent_class)->dispose (object);
}

static void
terminal_window_finalize (GObject *object)
{
    TerminalWindow *window = TERMINAL_WINDOW (object);
    TerminalWindowPrivate *priv = window->priv;

    g_object_unref (priv->ui_manager);

    if (priv->confirm_close_dialog)
        gtk_dialog_response (GTK_DIALOG (priv->confirm_close_dialog),
                             GTK_RESPONSE_DELETE_EVENT);

    if (priv->search_find_dialog)
        gtk_dialog_response (GTK_DIALOG (priv->search_find_dialog),
                             GTK_RESPONSE_DELETE_EVENT);

    G_OBJECT_CLASS (terminal_window_parent_class)->finalize (object);
}

static gboolean
terminal_window_delete_event (GtkWidget *widget,
                              GdkEvent *event,
                              gpointer data)
{
    return confirm_close_window_or_tab (TERMINAL_WINDOW (widget), NULL);
}

static gboolean
terminal_window_focus_in_event (GtkWidget *widget,
                                GdkEventFocus *event,
                                gpointer data)
{
  TerminalWindow *window = TERMINAL_WINDOW (widget);
  TerminalWindowPrivate *priv = window->priv;

  if (event->in)
    priv->focus_time = g_get_real_time () / G_USEC_PER_SEC;

  return FALSE;
}

static void
terminal_window_show (GtkWidget *widget)
{
    TerminalWindow *window = TERMINAL_WINDOW (widget);
    GtkAllocation widget_allocation;

    gtk_widget_get_allocation (widget, &widget_allocation);

    TerminalWindowPrivate *priv = window->priv;

    if (priv->active_screen != NULL)
    {
        terminal_window_update_copy_selection (priv->active_screen, window);
#if 0
        /* At this point, we have our GdkScreen, and hence the right
         * font size, so we can go ahead and size the window. */
        terminal_window_update_size (window, priv->active_screen, FALSE);
#endif
    }

    terminal_window_update_geometry (window);

    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] show, size %d : %d at (%d, %d)\n",
                           widget,
                           widget_allocation.width, widget_allocation.height,
                           widget_allocation.x, widget_allocation.y);

    GTK_WIDGET_CLASS (terminal_window_parent_class)->show (widget);
}

TerminalWindow*
terminal_window_new (void)
{
    return g_object_new (TERMINAL_TYPE_WINDOW, NULL);
}

/**
 * terminal_window_set_is_restored:
 * @window:
 *
 * Marks the window as restored from session.
 */
void
terminal_window_set_is_restored (TerminalWindow *window)
{
    g_return_if_fail (TERMINAL_IS_WINDOW (window));
    g_return_if_fail (!gtk_widget_get_mapped (GTK_WIDGET (window)));

    window->priv->clear_demands_attention = TRUE;
}

static void
profile_set_callback (TerminalScreen *screen,
                      TerminalProfile *old_profile,
                      TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (!gtk_widget_get_realized (GTK_WIDGET (window)))
        return;

    if (screen != priv->active_screen)
        return;

    terminal_window_update_set_profile_menu_active_profile (window);
}

static void
sync_screen_title (TerminalScreen *screen,
                   GParamSpec *psepc,
                   TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (screen != priv->active_screen)
        return;

    gtk_window_set_title (GTK_WINDOW (window), terminal_screen_get_title (screen));
}

static void
sync_screen_icon_title (TerminalScreen *screen,
                        GParamSpec *psepc,
                        TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (!gtk_widget_get_realized (GTK_WIDGET (window)))
        return;

    if (screen != priv->active_screen)
        return;

    if (!terminal_screen_get_icon_title_set (screen))
        return;

    gdk_window_set_icon_name (gtk_widget_get_window (GTK_WIDGET (window)), terminal_screen_get_icon_title (screen));
}

static void
sync_screen_icon_title_set (TerminalScreen *screen,
                            GParamSpec *psepc,
                            TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (!gtk_widget_get_realized (GTK_WIDGET (window)))
        return;

    if (screen != priv->active_screen)
        return;

    if (terminal_screen_get_icon_title_set (screen))
        return;

    /* Need to reset the icon name */
    gdk_window_set_icon_name (gtk_widget_get_window (GTK_WIDGET (window)), NULL);

    /* Re-setting the right title will be done by the notify::title handler which comes after this one */
}

/* Notebook callbacks */

static void
close_button_clicked_cb (GtkWidget *tab_label,
                         GtkWidget *screen_container)
{
    GtkWidget *toplevel;
    TerminalWindow *window;
    TerminalScreen *screen;

    toplevel = gtk_widget_get_toplevel (screen_container);
    if (!gtk_widget_is_toplevel (toplevel))
        return;

    if (!TERMINAL_IS_WINDOW (toplevel))
        return;

    window = TERMINAL_WINDOW (toplevel);

    screen = terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (screen_container));
    if (confirm_close_window_or_tab (window, screen))
        return;

    terminal_window_remove_screen (window, screen);
}

void
terminal_window_add_screen (TerminalWindow *window,
                            TerminalScreen *screen,
                            int            position)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *old_window;
    GtkWidget *screen_container, *tab_label;

    old_window = gtk_widget_get_toplevel (GTK_WIDGET (screen));
    if (gtk_widget_is_toplevel (old_window) &&
            TERMINAL_IS_WINDOW (old_window) &&
            TERMINAL_WINDOW (old_window)== window)
        return;

    if (TERMINAL_IS_WINDOW (old_window))
        terminal_window_remove_screen (TERMINAL_WINDOW (old_window), screen);

    screen_container = terminal_screen_container_new (screen);
    gtk_widget_show (screen_container);

    update_tab_visibility (window, +1);

    tab_label = terminal_tab_label_new (screen);
    g_signal_connect (tab_label, "close-button-clicked",
                      G_CALLBACK (close_button_clicked_cb), screen_container);

    gtk_notebook_insert_page (GTK_NOTEBOOK (priv->notebook),
                              screen_container,
                              tab_label,
                              position);
    gtk_container_child_set (GTK_CONTAINER (priv->notebook),
                             screen_container,
                             "tab-expand", TRUE,
                             "tab-fill", TRUE,
                             NULL);
    gtk_notebook_set_tab_reorderable (GTK_NOTEBOOK (priv->notebook),
                                      screen_container,
                                      TRUE);
    gtk_notebook_set_tab_detachable (GTK_NOTEBOOK (priv->notebook),
                                     screen_container,
                                     TRUE);
}

void
terminal_window_remove_screen (TerminalWindow *window,
                               TerminalScreen *screen)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreenContainer *screen_container;

    g_return_if_fail (gtk_widget_get_toplevel (GTK_WIDGET (screen)) == GTK_WIDGET (window));

    update_tab_visibility (window, -1);

    screen_container = terminal_screen_container_get_from_screen (screen);
    if (detach_tab)
    {
        gtk_notebook_detach_tab (GTK_NOTEBOOK (priv->notebook),
                                 GTK_WIDGET (screen_container));
        detach_tab = FALSE;
    }
    else
        gtk_container_remove (GTK_CONTAINER (priv->notebook),
                              GTK_WIDGET (screen_container));
}

void
terminal_window_move_screen (TerminalWindow *source_window,
                             TerminalWindow *dest_window,
                             TerminalScreen *screen,
                             int dest_position)
{
    TerminalScreenContainer *screen_container;

    g_return_if_fail (TERMINAL_IS_WINDOW (source_window));
    g_return_if_fail (TERMINAL_IS_WINDOW (dest_window));
    g_return_if_fail (TERMINAL_IS_SCREEN (screen));
    g_return_if_fail (gtk_widget_get_toplevel (GTK_WIDGET (screen)) == GTK_WIDGET (source_window));
    g_return_if_fail (dest_position >= -1);

    screen_container = terminal_screen_container_get_from_screen (screen);
    g_assert (TERMINAL_IS_SCREEN_CONTAINER (screen_container));

    /* We have to ref the screen container as well as the screen,
     * because otherwise removing the screen container from the source
     * window's notebook will cause the container and its containing
     * screen to be gtk_widget_destroy()ed!
     */
    g_object_ref_sink (screen_container);
    g_object_ref_sink (screen);

    detach_tab = TRUE;

    terminal_window_remove_screen (source_window, screen);

    /* Now we can safely remove the screen from the container and let the container die */
    gtk_container_remove (GTK_CONTAINER (gtk_widget_get_parent (GTK_WIDGET (screen))), GTK_WIDGET (screen));
    g_object_unref (screen_container);

    terminal_window_add_screen (dest_window, screen, dest_position);
    gtk_notebook_set_current_page (GTK_NOTEBOOK (dest_window->priv->notebook), dest_position);
    g_object_unref (screen);
}

GList*
terminal_window_list_screen_containers (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    /* We are trusting that GtkNotebook will return pages in order */
    return gtk_container_get_children (GTK_CONTAINER (priv->notebook));
}

void
terminal_window_set_menubar_visible (TerminalWindow *window,
                                     gboolean        setting)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkAction *action;

    /* it's been set now, so don't override when adding a screen.
     * this side effect must happen before we short-circuit below.
     */
    priv->use_default_menubar_visibility = FALSE;

    if (setting == priv->menubar_visible)
        return;

    priv->menubar_visible = (setting != FALSE);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "ViewMenubar");
    gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action), setting);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    g_object_set (priv->menubar, "visible", setting, NULL);

    /* FIXMEchpe: use gtk_widget_get_realized instead? */
    if (priv->active_screen)
    {
        _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                               "[window %p] setting size after toggling menubar visibility\n",
                               window);

        terminal_window_update_size (window, priv->active_screen, TRUE);
    }
}

gboolean
terminal_window_get_menubar_visible (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    return priv->menubar_visible;
}

GtkWidget *
terminal_window_get_notebook (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    g_return_val_if_fail (TERMINAL_IS_WINDOW (window), NULL);

    return GTK_WIDGET (priv->notebook);
}

void
terminal_window_update_size (TerminalWindow *window,
                          TerminalScreen *screen,
                          gboolean        even_if_mapped)
{
    terminal_window_update_size_set_geometry (window, screen,
                                              even_if_mapped, NULL);
}

gboolean
terminal_window_update_size_set_geometry (TerminalWindow *window,
                                          TerminalScreen *screen,
                                          gboolean        even_if_mapped,
                                          gchar          *geometry_string)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *widget;
    GtkWidget *app;
    gboolean result;
    int geom_result;
    gint force_pos_x = 0, force_pos_y = 0;
    unsigned int force_grid_width = 0, force_grid_height = 0;
    int grid_width, grid_height;
    gint pixel_width, pixel_height;
    GdkWindow *gdk_window;
    GdkGravity pos_gravity;

    gdk_window = gtk_widget_get_window (GTK_WIDGET (window));
    result = TRUE;

    if (gdk_window != NULL &&
        (gdk_window_get_state (gdk_window) &
         (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_TILED)))
    {
        /* Don't adjust the size of maximized or tiled (snapped, half-maximized)
         * windows: if we do, there will be ugly gaps of up to 1 character cell
         * around otherwise tiled windows. */
        return result;
    }

    /* be sure our geometry is up-to-date */
    terminal_window_update_geometry (window);

    if (GTK_IS_WIDGET (screen))
        widget = GTK_WIDGET (screen);
    else
        widget = GTK_WIDGET (window);

    app = gtk_widget_get_toplevel (widget);
    g_assert (app != NULL);

    terminal_screen_get_size (screen, &grid_width, &grid_height);
    if (geometry_string != NULL)
    {
        geom_result = terminal_window_XParseGeometry (geometry_string,
                                                      &force_pos_x,
                                                      &force_pos_y,
                                                      &force_grid_width,
                                                      &force_grid_height);
        if (geom_result == NoValue)
            result = FALSE;
    }
    else
        geom_result = NoValue;

    if ((geom_result & WidthValue) != 0)
        grid_width = force_grid_width;
    if ((geom_result & HeightValue) != 0)
        grid_height = force_grid_height;

    /* the "old" struct members were updated by update_geometry */
    pixel_width = priv->old_chrome_width + grid_width * priv->old_char_width;
    pixel_height = priv->old_chrome_height + grid_height * priv->old_char_height;

    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] size is %dx%d cells of %dx%d px\n",
                           window, grid_width, grid_height,
                           priv->old_char_width, priv->old_char_height);

    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] %dx%d + %dx%d = %dx%d\n",
                           window, grid_width * priv->old_char_width,
                           grid_height * priv->old_char_height,
                           priv->old_chrome_width, priv->old_chrome_height,
                           pixel_width, pixel_height);

    pos_gravity = GDK_GRAVITY_NORTH_WEST;
    if ((geom_result & XNegative) != 0 && (geom_result & YNegative) != 0)
        pos_gravity = GDK_GRAVITY_SOUTH_EAST;
    else if ((geom_result & XNegative) != 0)
        pos_gravity = GDK_GRAVITY_NORTH_EAST;
    else if ((geom_result & YNegative) != 0)
        pos_gravity = GDK_GRAVITY_SOUTH_WEST;

    if ((geom_result & XValue) == 0)
        force_pos_x = 0;
    if ((geom_result & YValue) == 0)
        force_pos_y = 0;

    if (pos_gravity == GDK_GRAVITY_SOUTH_EAST ||
        pos_gravity == GDK_GRAVITY_NORTH_EAST)
        force_pos_x = WidthOfScreen (gdk_x11_screen_get_xscreen (gtk_widget_get_screen (app))) -
                      pixel_width + force_pos_x;
    if (pos_gravity == GDK_GRAVITY_SOUTH_WEST ||
        pos_gravity == GDK_GRAVITY_SOUTH_EAST)
        force_pos_y = HeightOfScreen (gdk_x11_screen_get_xscreen (gtk_widget_get_screen (app))) -
                      pixel_height + force_pos_y;

    /* we don't let you put a window offscreen; maybe some people would
     * prefer to be able to, but it's kind of a bogus thing to do.
     */
    if (force_pos_x < 0)
        force_pos_x = 0;
    if (force_pos_y < 0)
        force_pos_y = 0;

    if (even_if_mapped && gtk_widget_get_mapped (app))
        gtk_window_resize (GTK_WINDOW (app), pixel_width, pixel_height);
    else
        gtk_window_set_default_size (GTK_WINDOW (app), pixel_width, pixel_height);

    if ((geom_result & XValue) != 0 || (geom_result & YValue) != 0)
    {
        gtk_window_set_gravity (GTK_WINDOW (app), pos_gravity);
        gtk_window_move (GTK_WINDOW (app), force_pos_x, force_pos_y);
    }

    return result;
}

void
terminal_window_switch_screen (TerminalWindow *window,
                               TerminalScreen *screen)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreenContainer *screen_container;
    int page_num;

    screen_container = terminal_screen_container_get_from_screen (screen);
    g_assert (TERMINAL_IS_SCREEN_CONTAINER (screen_container));
    page_num = gtk_notebook_page_num (GTK_NOTEBOOK (priv->notebook),
                                      GTK_WIDGET (screen_container));
    gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook), page_num);
}

TerminalScreen*
terminal_window_get_active (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    return priv->active_screen;
}

static gboolean
notebook_button_press_cb (GtkWidget *widget,
                          GdkEventButton *event,
                          GSettings *settings)
{
    TerminalWindow *window = TERMINAL_WINDOW (gtk_widget_get_toplevel (widget));
    TerminalWindowPrivate *priv = window->priv;
    GtkNotebook *notebook = GTK_NOTEBOOK (widget);
    GtkWidget *tab;
    GtkWidget *menu;
    GtkAction *action;
    int tab_clicked;
    int page_num;
    int before_pages;
    int later_pages;

    if ((event->type == GDK_BUTTON_PRESS && event->button == 2) &&
            (g_settings_get_boolean (settings, "middle-click-closes-tabs")))
    {
        tab_clicked = find_tab_num_at_pos (notebook,
                                           (int)event->x_root,
                                           (int)event->y_root);
        if (tab_clicked >= 0)
        {
            before_pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook));
            page_num = gtk_notebook_get_current_page (notebook);
            gtk_notebook_set_current_page (notebook, tab_clicked);
            TerminalScreen *active_screen = priv->active_screen;

                if (!(confirm_close_window_or_tab (window, active_screen)))
                {
                    update_tab_visibility (window, -1);
                    gtk_notebook_remove_page(notebook, tab_clicked);
                }

                later_pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook));

                if (before_pages > later_pages) {
                    if (tab_clicked > page_num)
                        gtk_notebook_set_current_page (notebook, page_num);
                    else if (tab_clicked < page_num)
                        gtk_notebook_set_current_page (notebook, page_num - 1);
                }
                else
                    gtk_notebook_set_current_page (notebook, page_num);

        }
    }

    if (event->type != GDK_BUTTON_PRESS ||
            event->button != 3 ||
            (event->state & gtk_accelerator_get_default_mod_mask ()) != 0)
        return FALSE;

    tab_clicked = find_tab_num_at_pos (notebook,
                                       (int)event->x_root,
                                       (int)event->y_root);
    if (tab_clicked < 0)
        return FALSE;

    /* switch to the page the mouse is over */
    gtk_notebook_set_current_page (notebook, tab_clicked);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "NotebookPopup");
    gtk_action_activate (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    menu = gtk_ui_manager_get_widget (priv->ui_manager, "/NotebookPopup");
    if (gtk_menu_get_attach_widget (GTK_MENU (menu)))
      gtk_menu_detach (GTK_MENU (menu));
    tab = gtk_notebook_get_nth_page (notebook, tab_clicked);
    gtk_menu_attach_to_widget (GTK_MENU (menu), tab, NULL);
    gtk_menu_popup_at_pointer (GTK_MENU (menu), NULL);

    return TRUE;
}

static gboolean
window_key_press_cb (GtkWidget *widget,
                     GdkEventKey *event,
                     GSettings *settings)
{
    if (g_settings_get_boolean (settings, "ctrl-tab-switch-tabs") &&
        event->state & GDK_CONTROL_MASK)
    {
        TerminalWindow *window = TERMINAL_WINDOW (widget);
        TerminalWindowPrivate *priv = window->priv;
        GtkNotebook *notebook = GTK_NOTEBOOK (priv->notebook);

        int pages = gtk_notebook_get_n_pages (notebook);
        int page_num = gtk_notebook_get_current_page (notebook);

        if (event->keyval == GDK_KEY_ISO_Left_Tab)
        {
            if (page_num != 0)
                gtk_notebook_prev_page (notebook);
            else
                gtk_notebook_set_current_page (notebook, (pages - 1));
            return TRUE;
        }

        if (event->keyval == GDK_KEY_Tab)
        {
            if (page_num != (pages -1))
                gtk_notebook_next_page (notebook);
            else
                gtk_notebook_set_current_page (notebook, 0);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
notebook_popup_menu_cb (GtkWidget *widget,
                        TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkNotebook *notebook = GTK_NOTEBOOK (priv->notebook);
    GtkWidget *focus_widget, *tab, *tab_label, *menu;
    GtkAction *action;
    int page_num;

    focus_widget = gtk_window_get_focus (GTK_WINDOW (window));
    /* Only respond if the notebook is the actual focus */
    if (focus_widget != priv->notebook)
        return FALSE;

    page_num = gtk_notebook_get_current_page (notebook);
    tab = gtk_notebook_get_nth_page (notebook, page_num);
    tab_label = gtk_notebook_get_tab_label (notebook, tab);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (priv->action_group, "NotebookPopup");
    gtk_action_activate (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    menu = gtk_ui_manager_get_widget (priv->ui_manager, "/NotebookPopup");
    if (gtk_menu_get_attach_widget (GTK_MENU (menu)))
      gtk_menu_detach (GTK_MENU (menu));
    gtk_menu_attach_to_widget (GTK_MENU (menu), tab_label, NULL);
    gtk_menu_popup_at_widget (GTK_MENU (menu),
                              tab_label,
                              GDK_GRAVITY_SOUTH_WEST,
                              GDK_GRAVITY_NORTH_WEST,
                              NULL);
    gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);

    return TRUE;
}

static void
notebook_page_selected_callback (GtkWidget       *notebook,
                                 GtkWidget       *page_widget,
                                 guint            page_num,
                                 TerminalWindow  *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *widget;
    TerminalScreen *screen;
    int old_grid_width, old_grid_height;

    _terminal_debug_print (TERMINAL_DEBUG_MDI,
                           "[window %p] MDI: page-selected %d\n",
                           window, page_num);

    if (priv->disposed)
        return;

    screen = terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (page_widget));
    widget = GTK_WIDGET (screen);
    g_assert (screen != NULL);

    _terminal_debug_print (TERMINAL_DEBUG_MDI,
                           "[window %p] MDI: setting active tab to screen %p (old active screen %p)\n",
                           window, screen, priv->active_screen);

    if (priv->active_screen == screen)
        return;

    if (priv->active_screen != NULL)
    {
        terminal_screen_get_size (priv->active_screen, &old_grid_width, &old_grid_height);

        /* This is so that we maintain the same grid */
        vte_terminal_set_size (VTE_TERMINAL (screen), old_grid_width, old_grid_height);
    }

    /* Workaround to remove gtknotebook's feature of computing its size based on
     * all pages. When the widget is hidden, its size will not be taken into
     * account.
     */
    if (priv->active_screen)
        gtk_widget_hide (GTK_WIDGET (priv->active_screen)); /* FIXME */

    /* Make sure that the widget is no longer hidden due to the workaround */
    gtk_widget_show (widget);

    priv->active_screen = screen;

    /* Override menubar setting if it wasn't restored from session */
    if (priv->use_default_menubar_visibility)
    {
        gboolean setting =
            terminal_profile_get_property_boolean (terminal_screen_get_profile (screen), TERMINAL_PROFILE_DEFAULT_SHOW_MENUBAR);

        terminal_window_set_menubar_visible (window, setting);
    }

    sync_screen_icon_title_set (screen, NULL, window);
    sync_screen_icon_title (screen, NULL, window);
    sync_screen_title (screen, NULL, window);

    /* set size of window to current grid size */
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] setting size after flipping notebook pages\n",
                           window);
    terminal_window_update_size (window, screen, TRUE);

    terminal_window_update_tabs_menu_sensitivity (window);
    terminal_window_update_encoding_menu_active_encoding (window);
    terminal_window_update_set_profile_menu_active_profile (window);
    terminal_window_update_copy_sensitivity (screen, window);
    terminal_window_update_zoom_sensitivity (window);
    terminal_window_update_search_sensitivity (screen, window);
}

static void
notebook_page_added_callback (GtkWidget       *notebook,
                              GtkWidget       *container,
                              guint            page_num,
                              TerminalWindow  *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreen *screen;
    int pages;

    screen = terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (container));

    _terminal_debug_print (TERMINAL_DEBUG_MDI,
                           "[window %p] MDI: screen %p inserted\n",
                           window, screen);

    g_signal_connect (G_OBJECT (screen),
                      "profile-set",
                      G_CALLBACK (profile_set_callback),
                      window);

    /* FIXME: only connect on the active screen, not all screens! */
    g_signal_connect (screen, "notify::title",
                      G_CALLBACK (sync_screen_title), window);
    g_signal_connect (screen, "notify::icon-title",
                      G_CALLBACK (sync_screen_icon_title), window);
    g_signal_connect (screen, "notify::icon-title-set",
                      G_CALLBACK (sync_screen_icon_title_set), window);
    g_signal_connect (screen, "selection-changed",
                      G_CALLBACK (terminal_window_update_copy_sensitivity), window);

    g_signal_connect (screen, "show-popup-menu",
                      G_CALLBACK (screen_show_popup_menu_callback), window);
    g_signal_connect (screen, "match-clicked",
                      G_CALLBACK (screen_match_clicked_cb), window);
    g_signal_connect (screen, "resize-window",
                      G_CALLBACK (screen_resize_window_cb), window);

    g_signal_connect (screen, "close-screen",
                      G_CALLBACK (screen_close_cb), window);

    update_tab_visibility (window, 0);
    terminal_window_update_tabs_menu_sensitivity (window);
    terminal_window_update_search_sensitivity (screen, window);

#if 0
    /* FIXMEchpe: wtf is this doing? */

    /* If we have an active screen, match its size and zoom */
    if (priv->active_screen)
    {
        int current_width, current_height;
        double scale;

        terminal_screen_get_size (priv->active_screen, &current_width, &current_height);
        vte_terminal_set_size (VTE_TERMINAL (screen), current_width, current_height);

        scale = terminal_screen_get_font_scale (priv->active_screen);
        terminal_screen_set_font_scale (screen, scale);
    }
#endif

    if (priv->present_on_insert)
    {
        gtk_window_present_with_time (GTK_WINDOW (window), gtk_get_current_event_time ());
        priv->present_on_insert = FALSE;
    }
    pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook));
    if (pages == 2) terminal_window_update_size (window, priv->active_screen, TRUE);
}

static void
notebook_page_removed_callback (GtkWidget       *notebook,
                                GtkWidget       *container,
                                guint            page_num,
                                TerminalWindow  *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreen *screen;
    int pages;

    if (priv->disposed)
        return;

    screen = terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (container));

    _terminal_debug_print (TERMINAL_DEBUG_MDI,
                           "[window %p] MDI: screen %p removed\n",
                           window, screen);

    g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                          G_CALLBACK (profile_set_callback),
                                          window);

    g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                          G_CALLBACK (sync_screen_title),
                                          window);

    g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                          G_CALLBACK (sync_screen_icon_title),
                                          window);

    g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                          G_CALLBACK (sync_screen_icon_title_set),
                                          window);

    g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                          G_CALLBACK (terminal_window_update_copy_sensitivity),
                                          window);

    g_signal_handlers_disconnect_by_func (screen,
                                          G_CALLBACK (screen_show_popup_menu_callback),
                                          window);

    g_signal_handlers_disconnect_by_func (screen,
                                          G_CALLBACK (screen_match_clicked_cb),
                                          window);
    g_signal_handlers_disconnect_by_func (screen,
                                          G_CALLBACK (screen_resize_window_cb),
                                          window);

    g_signal_handlers_disconnect_by_func (screen,
                                          G_CALLBACK (screen_close_cb),
                                          window);

    terminal_window_update_tabs_menu_sensitivity (window);
    update_tab_visibility (window, 0);
    terminal_window_update_search_sensitivity (screen, window);

    pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook));
    if (pages == 1)
    {
        terminal_window_update_size (window, priv->active_screen, TRUE);
    }
    else if (pages == 0)
    {
        gtk_widget_destroy (GTK_WIDGET (window));
    }
}

void
terminal_window_update_copy_selection (TerminalScreen *screen,
        TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    priv->copy_selection =
        terminal_profile_get_property_boolean (terminal_screen_get_profile (screen),
            TERMINAL_PROFILE_COPY_SELECTION);
}

static gboolean
notebook_scroll_event_cb (GtkWidget      *widget,
                          GdkEventScroll *event,
                          TerminalWindow *window)
{
  GtkNotebook *notebook = GTK_NOTEBOOK (widget);
  GtkWidget *child, *event_widget, *action_widget;

  child = gtk_notebook_get_nth_page (notebook, gtk_notebook_get_current_page (notebook));
  if (child == NULL)
    return FALSE;

  event_widget = gtk_get_event_widget ((GdkEvent *) event);

  /* Ignore scroll events from the content of the page */
  if (event_widget == NULL ||
      event_widget == child ||
      gtk_widget_is_ancestor (event_widget, child))
    return FALSE;

  /* And also from the action widgets */
  action_widget = gtk_notebook_get_action_widget (notebook, GTK_PACK_START);
  if (event_widget == action_widget ||
      (action_widget != NULL && gtk_widget_is_ancestor (event_widget, action_widget)))
    return FALSE;
  action_widget = gtk_notebook_get_action_widget (notebook, GTK_PACK_END);
  if (event_widget == action_widget ||
      (action_widget != NULL && gtk_widget_is_ancestor (event_widget, action_widget)))
    return FALSE;

  switch (event->direction) {
    case GDK_SCROLL_RIGHT:
    case GDK_SCROLL_DOWN:
      gtk_notebook_next_page (notebook);
      break;
    case GDK_SCROLL_LEFT:
    case GDK_SCROLL_UP:
      gtk_notebook_prev_page (notebook);
      break;
    case GDK_SCROLL_SMOOTH:
      switch (gtk_notebook_get_tab_pos (notebook)) {
        case GTK_POS_LEFT:
        case GTK_POS_RIGHT:
          if (event->delta_y > 0)
            gtk_notebook_next_page (notebook);
          else if (event->delta_y < 0)
            gtk_notebook_prev_page (notebook);
          break;
        case GTK_POS_TOP:
        case GTK_POS_BOTTOM:
          if (event->delta_x > 0)
            gtk_notebook_next_page (notebook);
          else if (event->delta_x < 0)
            gtk_notebook_prev_page (notebook);
          break;
      }
      break;
    }

  return TRUE;
}

void
terminal_window_update_geometry (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *widget;
    GdkGeometry hints;
    GtkBorder padding;
    GtkRequisition toplevel_request, vbox_request, widget_request;
    int grid_width, grid_height;
    int char_width, char_height;
    int chrome_width, chrome_height;

    if (priv->active_screen == NULL)
        return;

    widget = GTK_WIDGET (priv->active_screen);

    /* We set geometry hints from the active term; best thing
     * I can think of to do. Other option would be to try to
     * get some kind of union of all hints from all terms in the
     * window, but that doesn't make too much sense.
     */
    terminal_screen_get_cell_size (priv->active_screen, &char_width, &char_height);

    terminal_screen_get_size (priv->active_screen, &grid_width, &grid_height);
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "%dx%d cells of %dx%d px = %dx%d px\n",
                           grid_width, grid_height, char_width, char_height,
                           char_width * grid_width, char_height * grid_height);

    gtk_style_context_get_padding(gtk_widget_get_style_context (widget),
                                  gtk_widget_get_state_flags (widget),
                                  &padding);

    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "padding = %dx%d px\n",
                           padding.left + padding.right,
                           padding.top + padding.bottom);

    gtk_widget_get_preferred_size (priv->main_vbox, NULL, &vbox_request);
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "content area requests %dx%d px\n",
                           vbox_request.width, vbox_request.height);

    gtk_widget_get_preferred_size (GTK_WIDGET (window), NULL, &toplevel_request);
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "window requests %dx%d px\n",
                           toplevel_request.width, toplevel_request.height);

    chrome_width = vbox_request.width - (char_width * grid_width);
    chrome_height = vbox_request.height - (char_height * grid_height);
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "chrome: %dx%d px\n",
                           chrome_width, chrome_height);

    gtk_widget_get_preferred_size (widget, NULL, &widget_request);
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "terminal widget requests %dx%d px\n",
                           widget_request.width, widget_request.height);

    if (char_width != priv->old_char_width ||
             char_height != priv->old_char_height ||
             padding.left + padding.right != priv->old_padding_width ||
             padding.top + padding.bottom != priv->old_padding_height ||
             chrome_width != priv->old_chrome_width ||
             chrome_height != priv->old_chrome_height ||
             widget != GTK_WIDGET (priv->old_geometry_widget))
    {
        hints.base_width = chrome_width;
        hints.base_height = chrome_height;

#define MIN_WIDTH_CHARS 4
#define MIN_HEIGHT_CHARS 1

        hints.width_inc = char_width;
        hints.height_inc = char_height;

        /* min size is min size of the whole window, remember. */
        hints.min_width = hints.base_width + hints.width_inc * MIN_WIDTH_CHARS;
        hints.min_height = hints.base_height + hints.height_inc * MIN_HEIGHT_CHARS;

        gtk_window_set_geometry_hints (GTK_WINDOW (window),
                                       NULL,
                                       &hints,
                                       GDK_HINT_RESIZE_INC |
                                       GDK_HINT_MIN_SIZE |
                                       GDK_HINT_BASE_SIZE);

        _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                               "[window %p] hints: base %dx%d min %dx%d inc %d %d\n",
                               window,
                               hints.base_width,
                               hints.base_height,
                               hints.min_width,
                               hints.min_height,
                               hints.width_inc,
                               hints.height_inc);

        priv->old_geometry_widget = widget;
    }
    else
    {
        _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                               "[window %p] hints: increment unchanged, not setting\n",
                               window);
    }

    /* We need these for the size calculation in terminal_window_update_size(),
     * so we set them unconditionally. */
    priv->old_char_width = char_width;
    priv->old_char_height = char_height;
    priv->old_chrome_width = chrome_width;
    priv->old_chrome_height = chrome_height;
    priv->old_padding_width = padding.left + padding.right;
    priv->old_padding_height = padding.top + padding.bottom;
}

static void
file_new_window_callback (GtkAction *action,
                          TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalApp *app;
    TerminalWindow *new_window;
    TerminalProfile *profile;
    char *new_working_directory;

    app = terminal_app_get ();

    profile = g_object_get_data (G_OBJECT (action), PROFILE_DATA_KEY);
    if (!profile)
        profile = terminal_screen_get_profile (priv->active_screen);
    if (!profile)
        profile = terminal_app_get_profile_for_new_term (app);
    if (!profile)
        return;

    if (_terminal_profile_get_forgotten (profile))
        return;

    new_window = terminal_app_new_window (app, gtk_widget_get_screen (GTK_WIDGET (window)));

    new_working_directory = terminal_screen_get_current_dir_with_fallback (priv->active_screen);
    terminal_app_new_terminal (app, new_window, profile,
                               NULL, NULL,
                               new_working_directory,
                               terminal_screen_get_initial_environment (priv->active_screen),
                               1.0);
    g_free (new_working_directory);

    gtk_window_present (GTK_WINDOW (new_window));
}

static void
file_new_tab_callback (GtkAction *action,
                       TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalApp *app;
    TerminalProfile *profile;
    char *new_working_directory;

    app = terminal_app_get ();
    profile = g_object_get_data (G_OBJECT (action), PROFILE_DATA_KEY);
    if (!profile)
        profile = terminal_screen_get_profile (priv->active_screen);
    if (!profile)
        profile = terminal_app_get_profile_for_new_term (app);
    if (!profile)
        return;

    if (_terminal_profile_get_forgotten (profile))
        return;

    new_working_directory = terminal_screen_get_current_dir_with_fallback (priv->active_screen);
    terminal_app_new_terminal (app, window, profile,
                               NULL, NULL,
                               new_working_directory,
                               terminal_screen_get_initial_environment (priv->active_screen),
                               1.0);
    g_free (new_working_directory);
}

static void
confirm_close_response_cb (GtkWidget *dialog,
                           int response,
                           TerminalWindow *window)
{
    TerminalScreen *screen;

    screen = g_object_get_data (G_OBJECT (dialog), "close-screen");

    gtk_widget_destroy (dialog);

    if (response != GTK_RESPONSE_ACCEPT)
        return;

    if (screen)
        terminal_window_remove_screen (window, screen);
    else
        gtk_widget_destroy (GTK_WIDGET (window));
}

/* Returns: TRUE if closing needs to wait until user confirmation;
 * FALSE if the terminal or window can close immediately.
 */
static gboolean
confirm_close_window_or_tab (TerminalWindow *window,
                             TerminalScreen *screen)
{
    GtkBuilder *builder;
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *dialog;
    gboolean has_processes;
    int n_tabs;
    char *confirm_msg;

    if (!g_settings_get_boolean (settings_global, "confirm-window-close"))
        return FALSE;

    if (screen)
    {
        has_processes = terminal_screen_has_foreground_process (screen);
        n_tabs = 1;
    }
    else
    {
        GList *tabs, *t;

        tabs = terminal_window_list_screen_containers (window);
        n_tabs = g_list_length (tabs);

        for (t = tabs; t != NULL; t = t->next)
        {
            TerminalScreen *terminal_screen;

            terminal_screen = terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (t->data));
            has_processes = terminal_screen_has_foreground_process (terminal_screen);
            if (has_processes)
                break;
        }
        g_list_free (tabs);
    }

    if (has_processes)
    {
        if (n_tabs > 1)
            confirm_msg = _("There are still processes running in some terminals in this window.\n"
                            "Closing the window will kill all of them.");
        else
            confirm_msg = _("There is still a process running in this terminal.\n"
                            "Closing the terminal will kill it.");
    } else if (n_tabs > 1)
            confirm_msg = _("There are multiple tabs open in this window.");
    else
        return FALSE;

    builder = gtk_builder_new_from_resource (TERMINAL_RESOURCES_PATH_PREFIX G_DIR_SEPARATOR_S "ui/confirm-close-dialog.ui");
    priv->confirm_close_dialog = dialog = GTK_WIDGET (gtk_builder_get_object (builder, "confirm_close_dialog"));
    if (n_tabs > 1) {
        gtk_label_set_text (GTK_LABEL (gtk_builder_get_object (builder, "question_text")), _("Close this window?"));
        gtk_button_set_label (GTK_BUTTON (gtk_builder_get_object (builder, "button_close")), _("C_lose Window"));
    } else {
        gtk_label_set_text (GTK_LABEL (gtk_builder_get_object (builder, "question_text")), _("Close this terminal?"));
        gtk_button_set_label (GTK_BUTTON (gtk_builder_get_object (builder, "button_close")), _("C_lose Terminal"));
    }
    gtk_label_set_text (GTK_LABEL (gtk_builder_get_object (builder, "description_text")), confirm_msg);
    g_object_unref (builder);

    g_object_set_data (G_OBJECT (dialog), "close-screen", screen);

    g_signal_connect (dialog, "destroy",
                      G_CALLBACK (gtk_widget_destroyed), &priv->confirm_close_dialog);
    g_signal_connect (dialog, "response",
                      G_CALLBACK (confirm_close_response_cb), window);

    gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (window));
    gtk_window_set_title (GTK_WINDOW (dialog), "");
    gtk_window_present (GTK_WINDOW (dialog));

    return TRUE;
}

static void
file_close_window_callback (GtkAction *action,
                            TerminalWindow *window)
{
    if (confirm_close_window_or_tab (window, NULL))
        return;

    gtk_widget_destroy (GTK_WIDGET (window));
}

#ifdef ENABLE_SAVE
static void
save_contents_dialog_on_response (GtkDialog *dialog, gint response_id, gpointer terminal)
{
    GtkWindow *parent;
    gchar *filename_uri = NULL;
    GFile *file;
    GOutputStream *stream;
    GError *error = NULL;

    if (response_id != GTK_RESPONSE_ACCEPT)
    {
        gtk_widget_destroy (GTK_WIDGET (dialog));
        return;
    }

    parent = (GtkWindow*) gtk_widget_get_ancestor (GTK_WIDGET (terminal), GTK_TYPE_WINDOW);
    filename_uri = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog));

    gtk_widget_destroy (GTK_WIDGET (dialog));

    if (filename_uri == NULL)
        return;

    file = g_file_new_for_uri (filename_uri);
    stream = G_OUTPUT_STREAM (g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error));

    if (stream)
    {
        /* FIXME
         * Should be replaced with the async version when vte implements that.
         */
        vte_terminal_write_contents_sync (terminal, stream,
                                          VTE_WRITE_DEFAULT,
                                          NULL, &error);
        g_object_unref (stream);
    }

    if (error)
    {
        terminal_util_show_error_dialog (parent, NULL, error,
                                         "%s", _("Could not save contents"));
        g_error_free (error);
    }

    g_object_unref(file);
    g_free(filename_uri);
}
#endif /* ENABLE_SAVE */

static void
file_save_contents_callback (GtkAction *action,
                             TerminalWindow *window)
{
#ifdef ENABLE_SAVE
    GtkWidget *dialog = NULL;
    TerminalWindowPrivate *priv = window->priv;
    VteTerminal *terminal;

    if (!priv->active_screen)
        return;

    terminal = VTE_TERMINAL (priv->active_screen);
    g_return_if_fail (VTE_IS_TERMINAL (terminal));

    dialog = gtk_file_chooser_dialog_new (_("Save as..."),
                                          GTK_WINDOW(window),
                                          GTK_FILE_CHOOSER_ACTION_SAVE,
                                          "gtk-cancel", GTK_RESPONSE_CANCEL,
                                          "gtk-save", GTK_RESPONSE_ACCEPT,
                                          NULL);

    gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);
    /* XXX where should we save to? */
    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP));

    gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW(window));
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);

    g_signal_connect (dialog, "response", G_CALLBACK (save_contents_dialog_on_response), terminal);
    g_signal_connect (dialog, "delete_event", G_CALLBACK (terminal_util_dialog_response_on_delete), NULL);

    gtk_window_present (GTK_WINDOW (dialog));
#endif /* ENABLE_SAVE */
}

static void
file_close_tab_callback (GtkAction *action,
                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalScreen *active_screen = priv->active_screen;

    if (!active_screen)
        return;

    if (confirm_close_window_or_tab (window, active_screen))
        return;

    terminal_window_remove_screen (window, active_screen);
}

static void
edit_copy_callback (GtkAction *action,
                    TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (!priv->active_screen)
        return;

#if VTE_CHECK_VERSION (0, 50, 0)
    vte_terminal_copy_clipboard_format (VTE_TERMINAL (priv->active_screen), VTE_FORMAT_TEXT);
#else
    vte_terminal_copy_clipboard (VTE_TERMINAL (priv->active_screen));
#endif
}

typedef struct
{
    TerminalScreen *screen;
    gboolean uris_as_paths;
} PasteData;

static void
clipboard_uris_received_cb (GtkClipboard *clipboard,
                            /* const */ char **uris,
                            PasteData *data)
{
    char *text;
    gsize len;

    if (!uris)
    {
        g_object_unref (data->screen);
        g_slice_free (PasteData, data);
        return;
    }

    /* This potentially modifies the strings in |uris| but that's ok */
    if (data->uris_as_paths)
        terminal_util_transform_uris_to_quoted_fuse_paths (uris);

    text = terminal_util_concat_uris (uris, &len);
    vte_terminal_feed_child (VTE_TERMINAL (data->screen), text, len);
    g_free (text);

    g_object_unref (data->screen);
    g_slice_free (PasteData, data);
}

static void
clipboard_targets_received_cb (GtkClipboard *clipboard,
                               GdkAtom *targets,
                               int n_targets,
                               PasteData *data)
{
    if (!targets)
    {
        g_object_unref (data->screen);
        g_slice_free (PasteData, data);
        return;
    }

    if (gtk_targets_include_uri (targets, n_targets))
    {
        gtk_clipboard_request_uris (clipboard,
                                    (GtkClipboardURIReceivedFunc) clipboard_uris_received_cb,
                                    data);
        return;
    }
    else /* if (gtk_targets_include_text (targets, n_targets)) */
    {
        vte_terminal_paste_clipboard (VTE_TERMINAL (data->screen));
    }

    g_object_unref (data->screen);
    g_slice_free (PasteData, data);
}

static void
edit_paste_callback (GtkAction *action,
                     TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkClipboard *clipboard;
    PasteData *data;
    const char *name;

    if (!priv->active_screen)
        return;

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), GDK_SELECTION_CLIPBOARD);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    name = gtk_action_get_name (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    data = g_slice_new (PasteData);
    data->screen = g_object_ref (priv->active_screen);
    data->uris_as_paths = (name == I_("EditPasteURIPaths") || name == I_("PopupPasteURIPaths"));

    gtk_clipboard_request_targets (clipboard,
                                   (GtkClipboardTargetsReceivedFunc) clipboard_targets_received_cb,
                                   data);
}

static void
edit_select_all_callback (GtkAction *action,
                          TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (!priv->active_screen)
        return;

    vte_terminal_select_all (VTE_TERMINAL (priv->active_screen));
}

static void
edit_keybindings_callback (GtkAction *action,
                           TerminalWindow *window)
{
    terminal_app_edit_keybindings (terminal_app_get (),
                                   GTK_WINDOW (window));
}

static void
edit_current_profile_callback (GtkAction *action,
                               TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    terminal_app_edit_profile (terminal_app_get (),
                               terminal_screen_get_profile (priv->active_screen),
                               GTK_WINDOW (window),
                               NULL);
}

static void
file_new_profile_callback (GtkAction *action,
                           TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    terminal_app_new_profile (terminal_app_get (),
                              terminal_screen_get_profile (priv->active_screen),
                              GTK_WINDOW (window));
}

static void
edit_profiles_callback (GtkAction *action,
                        TerminalWindow *window)
{
    terminal_app_manage_profiles (terminal_app_get (),
                                  GTK_WINDOW (window));
}

static void
view_menubar_toggled_callback (GtkToggleAction *action,
                               TerminalWindow *window)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    terminal_window_set_menubar_visible (window, gtk_toggle_action_get_active (action));
    G_GNUC_END_IGNORE_DEPRECATIONS;
}

static void
view_fullscreen_toggled_callback (GtkToggleAction *action,
                                  TerminalWindow *window)
{
    gboolean toggle_action_check;

    g_return_if_fail (gtk_widget_get_realized (GTK_WIDGET (window)));

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    toggle_action_check = gtk_toggle_action_get_active (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    if (toggle_action_check)
        gtk_window_fullscreen (GTK_WINDOW (window));
    else
        gtk_window_unfullscreen (GTK_WINDOW (window));
}

static const double zoom_factors[] =
{
    TERMINAL_SCALE_MINIMUM,
    TERMINAL_SCALE_XXXXX_SMALL,
    TERMINAL_SCALE_XXXX_SMALL,
    TERMINAL_SCALE_XXX_SMALL,
    PANGO_SCALE_XX_SMALL,
    PANGO_SCALE_X_SMALL,
    PANGO_SCALE_SMALL,
    PANGO_SCALE_MEDIUM,
    PANGO_SCALE_LARGE,
    PANGO_SCALE_X_LARGE,
    PANGO_SCALE_XX_LARGE,
    TERMINAL_SCALE_XXX_LARGE,
    TERMINAL_SCALE_XXXX_LARGE,
    TERMINAL_SCALE_XXXXX_LARGE,
    TERMINAL_SCALE_MAXIMUM
};

static gboolean
find_larger_zoom_factor (double  current,
                         double *found)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (zoom_factors); ++i)
    {
        /* Find a font that's larger than this one */
        if ((zoom_factors[i] - current) > 1e-6)
        {
            *found = zoom_factors[i];
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
find_smaller_zoom_factor (double  current,
                          double *found)
{
    int i;

    i = (int) G_N_ELEMENTS (zoom_factors) - 1;
    while (i >= 0)
    {
        /* Find a font that's smaller than this one */
        if ((current - zoom_factors[i]) > 1e-6)
        {
            *found = zoom_factors[i];
            return TRUE;
        }

        --i;
    }

    return FALSE;
}

static void
view_zoom_in_callback (GtkAction *action,
                       TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    double current;

    if (priv->active_screen == NULL)
        return;

    current = terminal_screen_get_font_scale (priv->active_screen);
    if (!find_larger_zoom_factor (current, &current))
        return;

    terminal_screen_set_font_scale (priv->active_screen, current);
    terminal_window_update_zoom_sensitivity (window);
}

static void
view_zoom_out_callback (GtkAction *action,
                        TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    double current;

    if (priv->active_screen == NULL)
        return;

    current = terminal_screen_get_font_scale (priv->active_screen);
    if (!find_smaller_zoom_factor (current, &current))
        return;

    terminal_screen_set_font_scale (priv->active_screen, current);
    terminal_window_update_zoom_sensitivity (window);
}

static void
view_zoom_normal_callback (GtkAction *action,
                           TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (priv->active_screen == NULL)
        return;

    terminal_screen_set_font_scale (priv->active_screen, PANGO_SCALE_MEDIUM);
    terminal_window_update_zoom_sensitivity (window);
}

static void
search_find_response_callback (GtkWidget *dialog,
                               int        response,
                               gpointer   user_data)
{
    TerminalWindow *window = TERMINAL_WINDOW (user_data);
    TerminalWindowPrivate *priv = window->priv;
    TerminalSearchFlags flags;
    VteRegex *regex;

    if (response != GTK_RESPONSE_ACCEPT)
        return;

    if (G_UNLIKELY (!priv->active_screen))
        return;

    regex = terminal_search_dialog_get_regex (dialog);
    g_return_if_fail (regex != NULL);

    flags = terminal_search_dialog_get_search_flags (dialog);

    vte_terminal_search_set_regex (VTE_TERMINAL (priv->active_screen), regex, 0);
    vte_terminal_search_set_wrap_around (VTE_TERMINAL (priv->active_screen),
                                         (flags & TERMINAL_SEARCH_FLAG_WRAP_AROUND));

    if (flags & TERMINAL_SEARCH_FLAG_BACKWARDS)
        vte_terminal_search_find_previous (VTE_TERMINAL (priv->active_screen));
    else
        vte_terminal_search_find_next (VTE_TERMINAL (priv->active_screen));

    terminal_window_update_search_sensitivity (priv->active_screen, window);
}

static gboolean
search_dialog_delete_event_cb (GtkWidget   *widget,
                               GdkEventAny *event,
                               gpointer     user_data)
{
    /* prevent destruction */
    return TRUE;
}

static void
search_find_callback (GtkAction *action,
                      TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (!priv->search_find_dialog)
    {
        GtkWidget *dialog;

        dialog = priv->search_find_dialog = terminal_search_dialog_new (GTK_WINDOW (window));

        g_signal_connect (dialog, "destroy",
                          G_CALLBACK (gtk_widget_destroyed), &priv->search_find_dialog);
        g_signal_connect (dialog, "response",
                          G_CALLBACK (search_find_response_callback), window);
        g_signal_connect (dialog, "delete-event",
                          G_CALLBACK (search_dialog_delete_event_cb), NULL);
    }

    terminal_search_dialog_present (priv->search_find_dialog);
}

static void
search_find_next_callback (GtkAction *action,
                           TerminalWindow *window)
{
    if (G_UNLIKELY (!window->priv->active_screen))
        return;

    vte_terminal_search_find_next (VTE_TERMINAL (window->priv->active_screen));
}

static void
search_find_prev_callback (GtkAction *action,
                           TerminalWindow *window)
{
    if (G_UNLIKELY (!window->priv->active_screen))
        return;

    vte_terminal_search_find_previous (VTE_TERMINAL (window->priv->active_screen));
}

static void
search_clear_highlight_callback (GtkAction *action,
                                 TerminalWindow *window)
{
    if (G_UNLIKELY (!window->priv->active_screen))
        return;

    vte_terminal_search_set_regex (VTE_TERMINAL (window->priv->active_screen), NULL, 0);
}

static void
terminal_next_or_previous_profile_cb (GtkAction *action,
                              TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalProfile *active_profile, *new_profile = NULL;
    GList *profiles, *p;

    const char *name;
    guint backwards = 0;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    name = gtk_action_get_name (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    if (strcmp (name, "ProfilePrevious") == 0)
    {
        backwards = 1;
    }

    profiles = terminal_app_get_profile_list (terminal_app_get ());
    if (profiles == NULL)
        return;

    if (priv->active_screen)
        active_profile = terminal_screen_get_profile (priv->active_screen);
    else
        return;

    for (p = profiles; p != NULL; p = p->next)
    {
        TerminalProfile *profile = (TerminalProfile *) p->data;
        if (profile == active_profile)
        {
            if (backwards) {
                p = p->prev;
                if (p == NULL)
                    p = g_list_last (profiles);
                new_profile = p->data;
                break;
            }
            else
            {
                p = p->next;
                if (p == NULL)
                    p = g_list_first (profiles);
                new_profile = p->data;
                break;
            }
        }
    }

    if (new_profile)
        terminal_screen_set_profile (priv->active_screen, new_profile);

    g_list_free (profiles);
}

static void
terminal_set_title_dialog_response_cb (GtkWidget *dialog,
                                       int response,
                                       TerminalScreen *screen)
{
    if (response == GTK_RESPONSE_OK)
    {
        GtkEntry *entry;
        const char *text;

        entry = GTK_ENTRY (g_object_get_data (G_OBJECT (dialog), "title-entry"));
        text = gtk_entry_get_text (entry);
        terminal_screen_set_user_title (screen, text);
    }

    gtk_widget_destroy (dialog);
}

static void
terminal_set_title_callback (GtkAction *action,
                             TerminalWindow *window)
{
    GtkBuilder *builder;
    TerminalWindowPrivate *priv = window->priv;
    GtkWidget *dialog, *entry;

    if (priv->active_screen == NULL)
        return;

    builder = gtk_builder_new_from_resource (TERMINAL_RESOURCES_PATH_PREFIX G_DIR_SEPARATOR_S "ui/set-title-dialog.ui");
    dialog = GTK_WIDGET (gtk_builder_get_object (builder, "dialog"));
    entry = GTK_WIDGET (gtk_builder_get_object (builder, "title_entry"));
    g_object_unref (builder);

    gtk_widget_grab_focus (entry);
    gtk_entry_set_text (GTK_ENTRY (entry), terminal_screen_get_raw_title (priv->active_screen));
    gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
    g_object_set_data (G_OBJECT (dialog), "title-entry", entry);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (terminal_set_title_dialog_response_cb), priv->active_screen);
    g_signal_connect (dialog, "delete-event",
                      G_CALLBACK (terminal_util_dialog_response_on_delete), NULL);

    gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (window));

    gtk_window_present (GTK_WINDOW (dialog));
}

static void
terminal_add_encoding_callback (GtkAction *action,
                                TerminalWindow *window)
{
    terminal_app_edit_encodings (terminal_app_get (),
                                 GTK_WINDOW (window));
}

static void
terminal_reset_callback (GtkAction *action,
                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (priv->active_screen == NULL)
        return;

    vte_terminal_reset (VTE_TERMINAL (priv->active_screen), TRUE, FALSE);
}

static void
terminal_reset_clear_callback (GtkAction *action,
                               TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    if (priv->active_screen == NULL)
        return;

    vte_terminal_reset (VTE_TERMINAL (priv->active_screen), TRUE, TRUE);
}

static void
tabs_next_or_previous_tab_cb (GtkAction *action,
                              TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkNotebookClass *klass;
    const char *name;
    guint keyval = 0;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    name = gtk_action_get_name (action);
    G_GNUC_END_IGNORE_DEPRECATIONS;
    if (strcmp (name, "TabsNext") == 0)
    {
        keyval = GDK_KEY_Page_Down;
    }
    else if (strcmp (name, "TabsPrevious") == 0)
    {
        keyval = GDK_KEY_Page_Up;
    }

    klass = GTK_NOTEBOOK_GET_CLASS (GTK_NOTEBOOK (priv->notebook));
    gtk_binding_set_activate (gtk_binding_set_by_class (klass),
                              keyval,
                              GDK_CONTROL_MASK,
                              G_OBJECT (priv->notebook));
}

static void
tabs_move_left_callback (GtkAction *action,
                         TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkNotebook *notebook = GTK_NOTEBOOK (priv->notebook);
    gint page_num,last_page;
    GtkWidget *page;

    page_num = gtk_notebook_get_current_page (notebook);
    last_page = gtk_notebook_get_n_pages (notebook) - 1;
    page = gtk_notebook_get_nth_page (notebook, page_num);

    gtk_notebook_reorder_child (notebook, page, page_num == 0 ? last_page : page_num - 1);
}

static void
tabs_move_right_callback (GtkAction *action,
                          TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    GtkNotebook *notebook = GTK_NOTEBOOK (priv->notebook);
    gint page_num,last_page;
    GtkWidget *page;

    page_num = gtk_notebook_get_current_page (notebook);
    last_page = gtk_notebook_get_n_pages (notebook) - 1;
    page = gtk_notebook_get_nth_page (notebook, page_num);

    gtk_notebook_reorder_child (notebook, page, page_num == last_page ? 0 : page_num + 1);
}

static void
tabs_detach_tab_callback (GtkAction *action,
                          TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;
    TerminalApp *app;
    TerminalWindow *new_window;
    TerminalScreen *screen;

    app = terminal_app_get ();

    screen = priv->active_screen;

    new_window = terminal_app_new_window (app, gtk_widget_get_screen (GTK_WIDGET (window)));

    terminal_window_move_screen (window, new_window, screen, -1);

    /* FIXME: this seems wrong if tabs are shown in the window */
    terminal_window_update_size (new_window, screen, FALSE);

    gtk_window_present_with_time (GTK_WINDOW (new_window), gtk_get_current_event_time ());
}

static void
help_contents_callback (GtkAction *action,
                        TerminalWindow *window)
{
    terminal_util_show_help (NULL, GTK_WINDOW (window));
}

#define ABOUT_GROUP "About"
#define EMAILIFY(string) (g_strdelimit ((string), "%", '@'))

static void
help_about_callback (GtkAction *action,
                     TerminalWindow *window)
{
    char *licence_text;
    GBytes *bytes;
    const guint8 *data;
    GKeyFile *key_file;
    GError *error = NULL;
    char **authors, **contributors, **artists, **documenters, **array_strv;
    gchar *comments = NULL;
    gsize data_len, n_authors = 0, n_contributors = 0, n_artists = 0, n_documenters = 0 , i;
    GPtrArray *array;

    bytes = g_resources_lookup_data (TERMINAL_RESOURCES_PATH_PREFIX G_DIR_SEPARATOR_S "ui/terminal.about",
                                     G_RESOURCE_LOOKUP_FLAGS_NONE,
                                     &error);
    g_assert_no_error (error);

    data = g_bytes_get_data (bytes, &data_len);
    key_file = g_key_file_new ();
    g_key_file_load_from_data (key_file, (const char *) data, data_len, 0, &error);
    g_assert_no_error (error);

    authors = g_key_file_get_string_list (key_file, ABOUT_GROUP, "Authors", &n_authors, NULL);
    contributors = g_key_file_get_string_list (key_file, ABOUT_GROUP, "Contributors", &n_contributors, NULL);
    artists = g_key_file_get_string_list (key_file, ABOUT_GROUP, "Artists", &n_artists, NULL);
    documenters = g_key_file_get_string_list (key_file, ABOUT_GROUP, "Documenters", &n_documenters, NULL);
    g_key_file_free (key_file);
    g_bytes_unref (bytes);

    array = g_ptr_array_new ();

    for (i = 0; i < n_authors; ++i)
        g_ptr_array_add (array, EMAILIFY (authors[i]));
    g_free (authors); /* strings are now owned by the array */

    if (n_contributors > 0)
    {
        g_ptr_array_add (array, g_strdup (""));
        g_ptr_array_add (array, g_strdup (_("Contributors:")));
        for (i = 0; i < n_contributors; ++i)
            g_ptr_array_add (array, EMAILIFY (contributors[i]));
    }
    g_free (contributors); /* strings are now owned by the array */

    g_ptr_array_add (array, NULL);
    array_strv = (char **) g_ptr_array_free (array, FALSE);

    for (i = 0; i < n_artists; ++i)
        artists[i] = EMAILIFY (artists[i]);
    for (i = 0; i < n_documenters; ++i)
        documenters[i] = EMAILIFY (documenters[i]);

    licence_text = terminal_util_get_licence_text ();

    comments = g_strdup_printf (_("MATE Terminal is a terminal emulator for the MATE Desktop Environment.\nPowered by Virtual TErminal %d.%d.%d"),
                                vte_get_major_version (), vte_get_minor_version (), vte_get_micro_version ());

    gtk_show_about_dialog (GTK_WINDOW (window),
                           "program-name", _("MATE Terminal"),
                           "version", VERSION,
                           "title", _("About MATE Terminal"),
                           "copyright", _("Copyright \xc2\xa9 2002–2004 Havoc Pennington\n"
                                          "Copyright \xc2\xa9 2003–2004, 2007 Mariano Suárez-Alvarez\n"
                                          "Copyright \xc2\xa9 2006 Guilherme de S. Pastore\n"
                                          "Copyright \xc2\xa9 2007–2010 Christian Persch\n"
                                          "Copyright \xc2\xa9 2011 Perberos\n"
                                          "Copyright \xc2\xa9 2012-2021 MATE developers"),
                           "comments", comments,
                           "authors", array_strv,
                           "artists", artists,
                           "documenters", documenters,
                           "license", licence_text,
                           "wrap-license", TRUE,
                           "translator-credits", _("translator-credits"),
                           "logo-icon-name", MATE_TERMINAL_ICON_NAME,
                           "website", PACKAGE_URL,
                           NULL);

    g_free (comments);
    g_strfreev (array_strv);
    g_strfreev (artists);
    g_strfreev (documenters);
    g_free (licence_text);
}

GtkUIManager *
terminal_window_get_ui_manager (TerminalWindow *window)
{
    TerminalWindowPrivate *priv = window->priv;

    return priv->ui_manager;
}

void
terminal_window_save_state (TerminalWindow *window,
                            GKeyFile *key_file,
                            const char *group)
{
    TerminalWindowPrivate *priv = window->priv;
    GList *tabs, *lt;
    TerminalScreen *active_screen;
    GdkWindowState state;
    GPtrArray *tab_names_array;
    char **tab_names;
    gsize len;

    //XXXif (priv->menub)//XXX
    g_key_file_set_boolean (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_MENUBAR_VISIBLE,
                            priv->menubar_visible);

    g_key_file_set_string (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_ROLE,
                           gtk_window_get_role (GTK_WINDOW (window)));

    state = gdk_window_get_state (gtk_widget_get_window (GTK_WIDGET (window)));
    if (state & GDK_WINDOW_STATE_MAXIMIZED)
        g_key_file_set_boolean (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_MAXIMIZED, TRUE);
    if (state & GDK_WINDOW_STATE_FULLSCREEN)
        g_key_file_set_boolean (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_FULLSCREEN, TRUE);

    active_screen = terminal_window_get_active (window);
    tabs = terminal_window_list_screen_containers (window);

    tab_names_array = g_ptr_array_sized_new (g_list_length (tabs) + 1);

    for (lt = tabs; lt != NULL; lt = lt->next)
    {
        TerminalScreen *screen;
        char *tab_group;

        screen = terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (lt->data));

        tab_group = g_strdup_printf ("Terminal%p", screen);
        g_ptr_array_add (tab_names_array, tab_group);

        terminal_screen_save_config (screen, key_file, tab_group);

        if (screen == active_screen)
        {
            int w, h, x, y;
            char *geometry;

            g_key_file_set_string (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_ACTIVE_TAB, tab_group);

            /* FIXME saving the geometry is not great :-/ */
            terminal_screen_get_size (screen, &w, &h);
            gtk_window_get_position (GTK_WINDOW (window), &x, &y);
            geometry = g_strdup_printf ("%dx%d+%d+%d", w, h, x, y);
            g_key_file_set_string (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_GEOMETRY, geometry);
            g_free (geometry);
        }
    }

    g_list_free (tabs);

    len = tab_names_array->len;
    g_ptr_array_add (tab_names_array, NULL);
    tab_names = (char **) g_ptr_array_free (tab_names_array, FALSE);
    g_key_file_set_string_list (key_file, group, TERMINAL_CONFIG_WINDOW_PROP_TABS, (const char * const *) tab_names, len);
    g_strfreev (tab_names);
}

TerminalWindow *
terminal_window_get_latest_focused (TerminalWindow *window1,
                                    TerminalWindow *window2)
{
  TerminalWindowPrivate *priv1 = NULL;
  TerminalWindowPrivate *priv2 = NULL;

  if (!window1)
    return window2;

  if (!window2)
    return window1;

  priv1 = window1->priv;
  priv2 = window2->priv;

  if (priv2->focus_time > priv1->focus_time)
    return window2;

  return window1;
}

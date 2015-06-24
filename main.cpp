#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <xmmsclient/xmmsclient.h>
#include <xmmsclient/xmmsclient-glib.h>

// query album art from xmms2
// playlist_clear

struct XugApp
{
	GtkApplication parent;
};

struct XugAppClass
{
	GtkApplicationClass parent_class;
};

struct XugAppWindow
{
	GtkApplicationWindow parent;
};

struct XugAppWindowPrivate
{
	GtkWidget *stack;
	GtkWidget *play;
	GtkWidget *stop;
	GtkWidget *track;
	GtkWidget *progress;
	GtkWidget *search;
	GtkWidget *searchbar;
};

struct XugAppWindowClass
{
	GtkApplicationWindowClass parent_class;
};


struct XmmsResult {
	XmmsResult(xmmsc_result_t* result) : _result(result) {}

	~XmmsResult() {
		xmmsc_result_unref(_result);
	}

	void wait() {
		xmmsc_result_wait(_result);
	}

	xmmsv_t* value() {
		return xmmsc_result_get_value(_result);
	}

	void notifier_set(xmmsc_result_notifier_t func, void *user_data) {
		xmmsc_result_notifier_set(_result, func, user_data);
	}

	xmmsc_result_t* _result;
};


static bool check_value(xmmsv_t* value)
{
	const char* err_buf;
	if (xmmsv_is_error (value) && xmmsv_get_error (value, &err_buf)) {
		fprintf(stderr, "Error: %s\n", err_buf);
		return false;
	}
	return true;
}


static int get_int(xmmsv_t* value, int orelse=0)
{
	if (!check_value(value))
		return orelse;
	int intval;
	if (!xmmsv_get_int(value, &intval)) {
		fprintf(stderr, "Error: value not int\n");
		return orelse;
	}
	return intval;
}


struct PlaybackStatus
{
	int id;
	int status;
	int playtime;
	int position;
	int duration;

	gchar* title;
	gchar* artist;
};


XugApp* xug_app_new();
XugAppWindow* xug_app_window_new(XugApp* app);

#define XUG_APP_TYPE (xug_app_get_type())
#define XUG_APP(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), XUG_APP_TYPE, XugApp))
#define XUG_APP_WINDOW_TYPE (xug_app_window_get_type())
#define XUG_APP_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), XUG_APP_WINDOW_TYPE, XugAppWindow))

GType   xug_app_get_type();
XugApp* xug_app_new();
GType   xug_app_window_get_type();
XugAppWindow* xug_app_window_new(XugApp* app);


G_DEFINE_TYPE(XugApp, xug_app, GTK_TYPE_APPLICATION);
G_DEFINE_TYPE_WITH_PRIVATE(XugAppWindow, xug_app_window, GTK_TYPE_APPLICATION_WINDOW);

static int cb_current_id(xmmsv_t* value, void* userdata);
static int cb_status(xmmsv_t* value, void* userdata);
static int cb_playtime(xmmsv_t* value, void* userdata);
static void quit_activated(GSimpleAction* action, GVariant* parameter, gpointer app);
static void search_text_changed(GtkEntry* entry, XugAppWindow* win);
static void visible_child_changed(GObject* stack, GParamSpec* pspec);

static void play_clicked(GtkButton* button, XugAppWindow* win);
static void choose_view_clicked(GtkButton* button, XugAppWindow* win);


static xmmsc_connection_t* g_xmms_sync;
static xmmsc_connection_t* g_xmms_async;
static PlaybackStatus g_playback_status;


static GActionEntry app_entries[] =
{
	{ "quit", quit_activated, 0, 0, 0 }
};


inline XugAppWindowPrivate* privates(XugAppWindow* win) {
	return (XugAppWindowPrivate*)xug_app_window_get_instance_private(win);
}


static void xug_app_init(XugApp* app)
{
}


static void xug_app_activate(GApplication* app)
{
	auto win = xug_app_window_new(XUG_APP(app));

	xmmsc_mainloop_gmain_init(g_xmms_async);

	gtk_window_present(GTK_WINDOW(win));
}


static void xug_app_startup(GApplication* app)
{
	const gchar* quit_accels[2] = { "<Ctrl>Q", 0 };
	G_APPLICATION_CLASS(xug_app_parent_class)->startup(app);
	g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
	auto builder = gtk_builder_new_from_resource("/gs/kri/xug/app-menu.ui");
	auto app_menu = G_MENU_MODEL(gtk_builder_get_object(builder, "appmenu"));
	gtk_application_set_app_menu(GTK_APPLICATION(app), app_menu);
	g_object_unref(builder);
}


static void xug_app_class_init(XugAppClass* cls)
{
	G_APPLICATION_CLASS(cls)->activate = xug_app_activate;
	G_APPLICATION_CLASS(cls)->startup = xug_app_startup;
}


XugApp* xug_app_new()
{
	return (XugApp*)g_object_new(XUG_APP_TYPE, "application-id", "gs.kri.xug", "flags", 0, nullptr);
}

static void
enrich_mediainfo (xmmsv_t *val)
{
	if (!xmmsv_dict_has_key (val, "title") && xmmsv_dict_has_key (val, "url")) {
		/* First decode the URL encoding */
		xmmsv_t *tmp, *v, *urlv;
		gchar *url = NULL;
		const gchar *filename = NULL;
		const unsigned char *burl;
		unsigned int blen;

		xmmsv_dict_get (val, "url", &v);

		tmp = xmmsv_decode_url (v);
		if (tmp && xmmsv_get_bin (tmp, &burl, &blen)) {
			url = (gchar*)g_malloc (blen + 1);
			memcpy (url, burl, blen);
			url[blen] = 0;
			xmmsv_unref (tmp);
			filename = strrchr (url, '/');
			if (!filename || !filename[1]) {
				filename = url;
			} else {
				filename = filename + 1;
			}
		}

		/* Let's see if the result is valid utf-8. This must be done
		 * since we don't know the charset of the binary string */
		if (filename && g_utf8_validate (filename, -1, NULL)) {
			/* If it's valid utf-8 we don't have any problem just
			 * printing it to the screen
			 */
			urlv = xmmsv_new_string (filename);
		} else if (filename) {
			/* Not valid utf-8 :-( We make a valid guess here that
			 * the string when it was encoded with URL it was in the
			 * same charset as we have on the terminal now.
			 *
			 * THIS MIGHT BE WRONG since different clients can have
			 * different charsets and DIFFERENT computers most likely
			 * have it.
			 */
			gchar *tmp2 = g_locale_to_utf8 (filename, -1, NULL, NULL, NULL);
			urlv = xmmsv_new_string (tmp2);
			g_free (tmp2);
		} else {
			/* Decoding the URL failed for some reason. That's not good. */
			urlv = xmmsv_new_string ("?");
		}

		xmmsv_dict_set (val, "title", urlv);
		xmmsv_unref (urlv);
		g_free (url);
	}
}

static void update_ui(XugAppWindowPrivate* priv)
{
	if (g_playback_status.artist && g_playback_status.title) {
		gchar* s = g_strdup_printf("%s - %s", g_playback_status.artist, g_playback_status.title);
		gtk_label_set_text(GTK_LABEL(priv->track), s);
		g_free(s);
	} else if (g_playback_status.title) {
		gtk_label_set_text(GTK_LABEL(priv->track), g_playback_status.title);
	} else {
		gtk_label_set_text(GTK_LABEL(priv->track), "Not playing");
	}
	gtk_level_bar_set_value(GTK_LEVEL_BAR(priv->progress), g_playback_status.playtime);

	if (g_playback_status.status == XMMS_PLAYBACK_STATUS_PLAY) {
		gtk_button_set_image(GTK_BUTTON(priv->play), gtk_image_new_from_icon_name("media-playback-pause", GTK_ICON_SIZE_BUTTON));
	} else {
		gtk_button_set_image(GTK_BUTTON(priv->play), gtk_image_new_from_icon_name("media-playback-start", GTK_ICON_SIZE_BUTTON));
	}

	gtk_widget_set_sensitive(priv->play, TRUE);

	gtk_widget_set_sensitive(priv->progress, g_playback_status.status != XMMS_PLAYBACK_STATUS_STOP);

}


static void update_medialib_info(XugAppWindowPrivate* priv, bool complete=false)
{
	int id = g_playback_status.id;
	if (id < 0)
		return;

	g_print("xmms medialib info: %d\n", id);

	XmmsResult result(xmmsc_medialib_get_info(g_xmms_sync, id));
	result.wait();
	if (!check_value(result.value())) {
		// nothing is playing
		g_playback_status.playtime = 0;
		g_playback_status.duration = 100;
		g_playback_status.title = 0;
		g_playback_status.artist = 0;
		update_ui(priv);
		return;
	}

	auto info = xmmsv_propdict_to_dict(result.value(), 0);
	enrich_mediainfo(info);
	int val;
	const char* sval;
	if (xmmsv_dict_entry_get_int(info, "playback_status", &val)) {
		g_playback_status.status = val;
	} else {
		if (complete) {
			XmmsResult r2(xmmsc_playback_status(g_xmms_sync));
			r2.wait();
			if (check_value(r2.value())) {
				g_playback_status.status = get_int(r2.value());
			}
		}
	}
	if (xmmsv_dict_entry_get_int(info, "playtime", &val)) {
		g_playback_status.playtime = val;
	} else {
		if (complete) {
			XmmsResult r2(xmmsc_playback_playtime(g_xmms_sync));
			r2.wait();
			if (check_value(r2.value())) {
				g_playback_status.playtime = get_int(r2.value());
			}
		}
	}
	if (xmmsv_dict_entry_get_int(info, "position", &val)) {
		g_playback_status.position = val;
	}
	if (xmmsv_dict_entry_get_int(info, "duration", &val)) {
		g_playback_status.duration = val;
	}

	if (xmmsv_dict_entry_get_string(info, "title", &sval)) {
		g_playback_status.title = g_strdup(sval);
	}

	if (xmmsv_dict_entry_get_string(info, "artist", &sval)) {
		g_playback_status.artist = g_strdup(sval);
	}

	update_ui(priv);
}

static void init_xmms_status(XugAppWindowPrivate* priv)
{
	const char* err_buf;
	// find out what is currently playing (if anything)
	g_print("xmms status\n");
	int id = -1;
	{
		XmmsResult result(xmmsc_playback_current_id(g_xmms_sync));
		result.wait();
		id = get_int(result.value(), -1);
	}
	if (id < 0)
		return;
	g_playback_status.id = id;

	update_medialib_info(priv, true);

        //info = xmms1.xmms.medialib_get_info(now)
        //status = xmms1.xmms.playback_status()
        //playtime = xmms1.xmms.playback_playtime()

}


static void xug_app_window_init(XugAppWindow* win)
{
	auto priv = privates(win);

	gtk_widget_init_template(GTK_WIDGET(win));

	g_object_bind_property(priv->search, "active", priv->searchbar, "search-mode-enabled", G_BINDING_BIDIRECTIONAL);

	init_xmms_status(priv);

	XmmsResult(xmmsc_broadcast_playback_status(g_xmms_async)).notifier_set(cb_status, win);
	XmmsResult(xmmsc_broadcast_playback_current_id(g_xmms_async)).notifier_set(cb_current_id, win);
	XmmsResult(xmmsc_signal_playback_playtime(g_xmms_async)).notifier_set(cb_playtime, win);

	gtk_widget_set_sensitive(priv->search, TRUE);

}

static void xug_app_window_dispose(GObject* object)
{
	//auto win = XUG_APP_WINDOW(object);
	//auto priv = privates(win);
	//g_clear_object(&priv->settings);
	G_OBJECT_CLASS(xug_app_window_parent_class)->dispose(object);
}


static void xug_app_window_class_init(XugAppWindowClass* cls)
{
	G_OBJECT_CLASS(cls)->dispose = xug_app_window_dispose;
	gtk_widget_class_set_template_from_resource(GTK_WIDGET_CLASS(cls), "/gs/kri/xug/window.ui");
	gtk_widget_class_bind_template_child_private(GTK_WIDGET_CLASS(cls), XugAppWindow, stack);
	gtk_widget_class_bind_template_child_private(GTK_WIDGET_CLASS(cls), XugAppWindow, search);
	gtk_widget_class_bind_template_child_private(GTK_WIDGET_CLASS(cls), XugAppWindow, searchbar);
	gtk_widget_class_bind_template_child_private(GTK_WIDGET_CLASS(cls), XugAppWindow, play);
	gtk_widget_class_bind_template_child_private(GTK_WIDGET_CLASS(cls), XugAppWindow, track);
	gtk_widget_class_bind_template_child_private(GTK_WIDGET_CLASS(cls), XugAppWindow, progress);

	gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(cls), search_text_changed);
	gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(cls), visible_child_changed);

	gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(cls), choose_view_clicked);
	gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(cls), play_clicked);

}


XugAppWindow* xug_app_window_new(XugApp* app)
{
	return (XugAppWindow*)g_object_new(XUG_APP_WINDOW_TYPE, "application", app, NULL);
}


static int cb_current_id(xmmsv_t* value, void* userdata)
{
	auto win = (XugAppWindow*)userdata;
	auto priv = privates(win);

	int id = get_int(value, -1);
	if (id < 0)
		return FALSE;

	g_print("event(current_id) %d\n", id);

	g_playback_status.id = id;
	// TODO: update status, playtime, position, duration

	update_medialib_info(priv, false);

	return TRUE;
}


static int cb_status(xmmsv_t* value, void* userdata)
{
	auto win = (XugAppWindow*)userdata;
	auto priv = privates(win);

	int status = get_int(value, -1);
	if (status < 0)
		return FALSE;

	g_print("event(status): %d\n", status);

	g_playback_status.status = status;

	update_ui(priv);

	return TRUE;
}


static int cb_playtime(xmmsv_t* value, void* userdata)
{
	auto win = (XugAppWindow*)userdata;
	auto priv = privates(win);

	int time = get_int(value, -1);
	if (time < 0)
		return FALSE;

	//g_print("playtime %d\n", time);

	g_playback_status.playtime = time;

	if (g_playback_status.duration > 0) {

		double progress = (double)time / (double)g_playback_status.duration;

		gtk_level_bar_set_value(GTK_LEVEL_BAR(priv->progress), progress);
	}

	return TRUE;
}


static void quit_activated(GSimpleAction* action, GVariant* parameter, gpointer app)
{
	g_application_quit(G_APPLICATION(app));
}


static void search_text_changed(GtkEntry* entry, XugAppWindow* win)
{
	auto text = gtk_entry_get_text(entry);
	if (text[0] == '\0')
		return;

	g_print("text: %s\n", text);

	//auto priv = privates(win);
	//auto tab = gtk_stack_get_visible_child(GTK_STACK(priv->stack));
}


static void visible_child_changed(GObject* stack, GParamSpec* pspec)
{
	if (gtk_widget_in_destruction(GTK_WIDGET(stack)))
		return;

	auto win = XUG_APP_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET (stack)));
	auto priv = privates(win);
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(priv->searchbar), FALSE);
}


static void play_clicked(GtkButton* button, XugAppWindow* win)
{
	if (g_playback_status.status == XMMS_PLAYBACK_STATUS_PLAY) {
		XmmsResult result(xmmsc_playback_pause(g_xmms_sync));
		result.wait();
		g_playback_status.status = XMMS_PLAYBACK_STATUS_PAUSE;
	} else {
		XmmsResult result(xmmsc_playback_start(g_xmms_sync));
		result.wait();
		g_playback_status.status = XMMS_PLAYBACK_STATUS_PLAY;
	}
}


static void choose_view_clicked(GtkButton* button, XugAppWindow* win)
{
}


int main(int argc, char* argv[])
{
	g_xmms_sync = xmmsc_init("xugsync");
	if (!g_xmms_sync) {
		fprintf(stderr, "xmms2 connection failed (out of memory?)\n");
		return 1;
	}

	if (!xmmsc_connect(g_xmms_sync, getenv("XMMS_PATH"))) {

		// start the xmms2 server
		if (system ("xmms2-launcher") != 0) {
			fprintf(stderr, "connection failed: %s\n",
				xmmsc_get_last_error(g_xmms_sync));
			return 1;
		}

		if (!xmmsc_connect(g_xmms_sync, getenv("XMMS_PATH"))) {
			fprintf(stderr, "connection failed: %s\n",
				xmmsc_get_last_error(g_xmms_sync));
			return 1;
		}
	}

	g_xmms_async = xmmsc_init("xugasync");
	if (!g_xmms_async) {
		fprintf(stderr, "xmms2 connection failed (out of memory?)\n");
		return 1;
	}

	if (!xmmsc_connect(g_xmms_async, getenv("XMMS_PATH"))) {
		fprintf(stderr, "connection failed: %s\n",
			xmmsc_get_last_error(g_xmms_async));
		return 1;
	}

	return g_application_run(G_APPLICATION(xug_app_new()), argc, argv);
}

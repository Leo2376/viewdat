/*
 * viewdat-gui.c - GTK3 + Cairo GUI viewer for Ngspice .dat files.
 *
 * Usage: viewdat-gui [file.dat]
 *
 * Features: open .dat files, plot all signals vs time, left-side signal
 * browser with per-signal visibility toggles, per-signal selector,
 * reference-voltage line, time-only zoom in/out/reset,
 * bottom horizontal scrollbar for scrolling in time, SVG export.
 *
 * Zoom applies ONLY to the time (X) axis. The Y (value) axis always
 * fits the full data range. When zoomed in (zoom > 1), the bottom
 * scrollbar lets you pan through time.
 */
#include <cairo-svg.h>
#include <cairo.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "datreader.h"

static DatFile g_dat = { NULL, 0, 0, 0 };
static gboolean g_loaded = FALSE;
static char *g_path = NULL;

static GtkWidget *g_area;
static GtkWidget *g_hscroll;
static GtkAdjustment *g_hadj;
static GtkWidget *g_combo;
static GtkWidget *g_vref_entry;
static GtkWidget *g_status;

static double g_zoom = 1.0;      /* time-axis zoom, 1.0 = fit all */
static double g_view_t0 = 0.0;   /* left edge of visible time window */
static gboolean g_updating_scroll = FALSE;

static int g_show = -1;
static gboolean g_vref_on = FALSE;
static double g_vref = 0.0;

/* Left-side signal browser state: per-signal visibility flags. */
static gboolean *g_vis = NULL;
static int g_nvis = 0;
static GtkListStore *g_sig_store = NULL;
static gboolean g_updating_sig = FALSE;

enum {
    SIG_COL_VISIBLE = 0,
    SIG_COL_SWATCH,
    SIG_COL_NAME,
    SIG_COL_INDEX,
    SIG_N_COLS
};

static const double PALETTE[][3] = {
    {0.12, 0.47, 0.71}, {1.00, 0.50, 0.05}, {0.17, 0.63, 0.17},
    {0.84, 0.15, 0.16}, {0.58, 0.40, 0.74}, {0.55, 0.34, 0.29},
    {0.89, 0.47, 0.76}, {0.50, 0.50, 0.50},
};

/* Full data time range. Returns FALSE when there is no data. */
static gboolean data_time_range(double *tmin, double *tmax)
{
    if (!g_loaded || g_dat.nrows == 0)
        return FALSE;
    double vmin, vmax;
    dat_range(&g_dat, tmin, tmax, &vmin, &vmax);
    if (!( *tmax > *tmin )) {
        /* Degenerate: fabricate a unit span so zoom/scroll math works. */
        *tmax = *tmin + 1.0;
    }
    return TRUE;
}

/* Width of the visible time window for the current zoom. */
static double visible_width(double tmin, double tmax)
{
    double span = tmax - tmin;
    if (!(span > 0.0))
        span = 1.0;
    if (g_zoom < 1.0)
        g_zoom = 1.0;
    return span / g_zoom;
}

static double clamp_t0(double t0, double tmin, double tmax, double vis)
{
    if (!(vis < (tmax - tmin)))
        return tmin;
    if (!(t0 >= tmin))
        return tmin;
    if (t0 > tmax - vis)
        return tmax - vis;
    return t0;
}

/* Current visible time window + full (padded) value range.
 * Y is NEVER zoomed: it always shows the whole signal height. */
static void get_view(double *vtmin, double *vtmax,
                     double *vvmin, double *vvmax)
{
    double tmin, tmax, vmin, vmax;
    dat_range(&g_dat, &tmin, &tmax, &vmin, &vmax);
    if (!(tmax > tmin)) { tmax = tmin + 1.0; }
    if (!(vmax > vmin)) {
        double c = (vmax + vmin) / 2.0;
        if (!(vmax > vmin)) { vmin = c - 0.5; vmax = c + 0.5; }
    }

    double vis = visible_width(tmin, tmax);
    g_view_t0 = clamp_t0(g_view_t0, tmin, tmax, vis);
    *vtmin = g_view_t0;
    *vtmax = g_view_t0 + vis;

    /* Small Y padding so traces at min/max are not clipped by the frame. */
    double pad = (vmax - vmin) * 0.03;
    *vvmin = vmin - pad;
    *vvmax = vmax + pad;
    if (!(*vvmax > *vvmin)) {
        *vvmin = vmin - 0.5;
        *vvmax = vmax + 0.5;
    }
}

static void update_scrollbar(void)
{
    if (!g_hadj)
        return;
    double tmin, tmax;
    if (!data_time_range(&tmin, &tmax)) {
        g_updating_scroll = TRUE;
        gtk_adjustment_configure(g_hadj, 0, 0, 100, 1, 10, 10);
        gtk_widget_set_sensitive(g_hscroll, FALSE);
        g_updating_scroll = FALSE;
        return;
    }
    double vis = visible_width(tmin, tmax);
    g_view_t0 = clamp_t0(g_view_t0, tmin, tmax, vis);

    g_updating_scroll = TRUE;
    /* Adjustment in time units: value = left edge, page = visible width. */
    double step = vis / 10.0;
    if (!(step > 0.0))
        step = (tmax - tmin) / 100.0;
    if (!(step > 0.0))
        step = 1.0;
    gtk_adjustment_configure(g_hadj, g_view_t0,
                             tmin, tmax, step, vis * 0.9, vis);
    /* Only scrollable when zoomed in. */
    gtk_widget_set_sensitive(g_hscroll, vis < (tmax - tmin) - 1e-12);
    g_updating_scroll = FALSE;
}

static void render_plot(cairo_t *cr, double W, double H)
{
    if (!g_loaded || g_dat.nrows == 0) {
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, 30, 50);
        cairo_show_text(cr, "No file loaded - click Open to choose a .dat file.");
        return;
    }

    double vtmin, vtmax, vvmin, vvmax;
    get_view(&vtmin, &vtmax, &vvmin, &vvmax);

    int ML = 70, MR = 15, MT = 12, MB = 40;
    double pw = (W - ML - MR), ph = H - MT - MB;
    if (pw < 10) pw = 10;
    if (ph < 10) ph = 10;

    /* Background. */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* Grid. */
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 10; i++) {
        double x = ML + pw * i / 10.0;
        cairo_move_to(cr, x, MT);
        cairo_line_to(cr, x, MT + ph);
        cairo_stroke(cr);
    }
    for (int i = 0; i <= 8; i++) {
        double y = MT + ph * i / 8.0;
        cairo_move_to(cr, ML, y);
        cairo_line_to(cr, ML + pw, y);
        cairo_stroke(cr);
    }

    /* Frame. */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, ML, MT, pw, ph);
    cairo_stroke(cr);

    /* Clip traces to the plot frame so scrolled-out data doesn't paint
     * over margins/labels. */
    cairo_save(cr);
    cairo_rectangle(cr, ML, MT, pw, ph);
    cairo_clip(cr);

    int ns = dat_nsignals(&g_dat);
    if (ns <= 0)
        ns = 0;
    int stride = (g_dat.nrows > 4000) ? g_dat.nrows / 4000 : 1;

    double tspan = vtmax - vtmin;
    double vspan = vvmax - vvmin;
    if (!(tspan > 0.0)) tspan = 1.0;
    if (!(vspan > 0.0)) vspan = 1.0;

    for (int s = 0; s < ns; s++) {
        /* Single-select combo (g_show) and browser checkboxes combine:
         * a trace draws only if it passes both filters. */
        if (g_show >= 0 && s != g_show)
            continue;
        if (g_vis && s < g_nvis && !g_vis[s])
            continue;
        const double *c = PALETTE[s % 8];
        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
        cairo_set_line_width(cr, 1.4);
        gboolean pen = FALSE;
        for (int r = 0; r < g_dat.nrows; r += stride) {
            double t = dat_time(&g_dat, s, r);
            double v = dat_value(&g_dat, s, r);
            if (isnan(t) || isnan(v)) {
                pen = FALSE;
                continue;
            }
            if (t < vtmin || t > vtmax) {
                /* Outside the scrolled view: lift the pen so we don't
                 * draw a long diagonal across the window. */
                pen = FALSE;
                continue;
            }
            double x = ML + (t - vtmin) / tspan * pw;
            double y = MT + (vvmax - v) / vspan * ph;
            if (pen)
                cairo_line_to(cr, x, y);
            else {
                cairo_move_to(cr, x, y);
                pen = TRUE;
            }
        }
        cairo_stroke(cr);
    }

    /* Reference voltage line (Y is unzoomed, so it stays put). */
    if (g_vref_on && g_vref >= vvmin && g_vref <= vvmax) {
        double y = MT + (vvmax - g_vref) / vspan * ph;
        cairo_set_source_rgb(cr, 0.9, 0.1, 0.1);
        cairo_set_line_width(cr, 1.2);
        static const double dash[] = { 6.0, 4.0 };
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, ML, y);
        cairo_line_to(cr, ML + pw, y);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }

    cairo_restore(cr);

    /* Axis labels: X shows the *visible* time window, Y the full range. */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    char buf[160];
    double vmin, vmax, tmin, tmax;
    {
        double d_tmin, d_tmax, d_vmin, d_vmax;
        dat_range(&g_dat, &d_tmin, &d_tmax, &d_vmin, &d_vmax);
        vmin = d_vmin; vmax = d_vmax; tmin = d_tmin; tmax = d_tmax;
    }
    snprintf(buf, sizeof buf, "%g", vmax);
    cairo_move_to(cr, 4, MT + 10);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof buf, "%g", vmin);
    cairo_move_to(cr, 4, MT + ph);
    cairo_show_text(cr, buf);
    /* X ticks: label 6 ticks across the visible window. */
    for (int i = 0; i <= 5; i++) {
        double t = vtmin + tspan * i / 5.0;
        snprintf(buf, sizeof buf, "%g", t);
        cairo_move_to(cr, ML + pw * i / 5.0 - 10, MT + ph + 14);
        cairo_show_text(cr, buf);
    }
    snprintf(buf, sizeof buf, "view t: %g .. %g  (full %g .. %g)",
             vtmin, vtmax, tmin, tmax);
    cairo_move_to(cr, ML, H - 8);
    cairo_show_text(cr, buf);
}

static void on_scroll_changed(GtkAdjustment *adj, gpointer data)
{
    (void)data;
    if (g_updating_scroll)
        return;
    double tmin, tmax;
    if (!data_time_range(&tmin, &tmax))
        return;
    double vis = visible_width(tmin, tmax);
    g_view_t0 = clamp_t0(gtk_adjustment_get_value(adj), tmin, tmax, vis);
    gtk_widget_queue_draw(g_area);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)widget; (void)data;
    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    render_plot(cr, (double)W, (double)H);
    return FALSE;
}

static void update_status(void)
{
    char buf[512];
    if (!g_loaded) {
        gtk_label_set_text(GTK_LABEL(g_status), "No file loaded.");
        return;
    }
    double tmin, tmax, vmin, vmax;
    dat_range(&g_dat, &tmin, &tmax, &vmin, &vmax);
    double vtmin, vtmax, vvmin, vvmax;
    get_view(&vtmin, &vtmax, &vvmin, &vvmax);
    snprintf(buf, sizeof buf,
             "%s  |  rows: %d  signals: %d  (%s)  |  view t: %g..%g  v: %g..%g  |  time zoom: %.2fx",
             g_path ? g_path : "?",
             g_dat.nrows, dat_nsignals(&g_dat),
             g_dat.paired ? "paired t,v" : "plain",
             vtmin, vtmax, vmin, vmax, g_zoom);
    gtk_label_set_text(GTK_LABEL(g_status), buf);
}

static void sig_color_hex(int s, char *buf, size_t len)
{
    const double *c = PALETTE[s % 8];
    int r = (int)(c[0] * 255.0 + 0.5);
    int g = (int)(c[1] * 255.0 + 0.5);
    int b = (int)(c[2] * 255.0 + 0.5);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    snprintf(buf, len, "#%02X%02X%02X", r, g, b);
}

static void sig_ensure_size(int ns)
{
    if (ns <= 0) {
        free(g_vis);
        g_vis = NULL;
        g_nvis = 0;
        return;
    }
    if (ns != g_nvis) {
        free(g_vis);
        g_vis = malloc((size_t)ns * sizeof *g_vis);
        g_nvis = (g_vis != NULL) ? ns : 0;
    }
    for (int i = 0; i < g_nvis; i++)
        g_vis[i] = TRUE;
}

static void sig_refresh_store(void)
{
    if (!g_sig_store)
        return;
    g_updating_sig = TRUE;
    gtk_list_store_clear(g_sig_store);
    int ns = dat_nsignals(&g_dat);
    if (g_loaded && ns > 0) {
        for (int s = 0; s < ns; s++) {
            char name[32], hex[16];
            snprintf(name, sizeof name, "Signal %d", s);
            sig_color_hex(s, hex, sizeof hex);
            GtkTreeIter it;
            gtk_list_store_append(g_sig_store, &it);
            gtk_list_store_set(g_sig_store, &it,
                               SIG_COL_VISIBLE, (g_vis && s < g_nvis) ? g_vis[s] : TRUE,
                               SIG_COL_SWATCH, hex,
                               SIG_COL_NAME, name,
                               SIG_COL_INDEX, s,
                               -1);
        }
    }
    g_updating_sig = FALSE;
}

/* Push visibility flags into the tree model without rebuilding rows. */
static void sig_push_to_store(void)
{
    if (!g_sig_store)
        return;
    g_updating_sig = TRUE;
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_sig_store), &it);
    while (ok) {
        int idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(g_sig_store), &it, SIG_COL_INDEX, &idx, -1);
        if (idx >= 0 && idx < g_nvis)
            gtk_list_store_set(g_sig_store, &it, SIG_COL_VISIBLE, g_vis[idx], -1);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_sig_store), &it);
    }
    g_updating_sig = FALSE;
}

static gboolean load_file(const char *path)
{
    DatFile nd = { NULL, 0, 0, 0 };
    if (dat_load(path, &nd) != 0)
        return FALSE;

    dat_free(&g_dat);
    g_dat = nd;
    g_loaded = TRUE;
    free(g_path);
    g_path = strdup(path);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_combo));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_combo), "All signals");
    int ns = dat_nsignals(&g_dat);
    for (int s = 0; s < ns; s++) {
        char buf[32];
        snprintf(buf, sizeof buf, "Signal %d", s);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_combo), buf);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), 0);
    g_show = -1;
    g_zoom = 1.0;

    sig_ensure_size(ns);
    sig_refresh_store();

    double tmin, tmax;
    if (data_time_range(&tmin, &tmax))
        g_view_t0 = tmin;
    else
        g_view_t0 = 0.0;

    update_scrollbar();
    update_status();
    gtk_widget_queue_draw(g_area);
    return TRUE;
}

static void show_error(GtkWindow *parent, const char *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR,
                                            GTK_BUTTONS_CLOSE, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_open(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWindow *parent = GTK_WINDOW(data);
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open .dat file", parent, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Ngspice data (*.dat)");
    gtk_file_filter_add_pattern(filter, "*.dat");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (!load_file(path))
            show_error(parent, "Could not read that file (see terminal for details).");
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_combo_changed(GtkComboBox *box, gpointer data)
{
    (void)data;
    if (g_updating_sig)
        return;
    int active = gtk_combo_box_get_active(box);
    g_show = (active == 0) ? -1 : active - 1;
    /* Combo acts as a quick filter on top of "all checked": reselecting
     * from the combo re-enables every checkbox so the two stay in sync. */
    for (int s = 0; s < g_nvis; s++)
        g_vis[s] = TRUE;
    sig_push_to_store();
    gtk_widget_queue_draw(g_area);
}

static void sig_sync_combo(void)
{
    if (!g_combo)
        return;
    g_updating_sig = TRUE;
    int ns = (g_vis != NULL) ? g_nvis : dat_nsignals(&g_dat);
    int nvis = 0, only = -1;
    for (int s = 0; s < ns; s++) {
        if (!g_vis || s >= g_nvis || g_vis[s]) {
            nvis++;
            only = s;
        }
    }
    if (nvis == ns)
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), 0);
    else if (nvis == 1 && only >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), only + 1);
    else
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), -1);
    /* Keep single-select filter consistent: custom subsets mean "no
     * combo filter", a single visible signal means "that signal". */
    if (nvis == ns)
        g_show = -1;
    else if (nvis == 1)
        g_show = only;
    else if (nvis > 1)
        g_show = -1;
    g_updating_sig = FALSE;
}

static void on_sig_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell; (void)data;
    if (g_updating_sig || !g_sig_store)
        return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_sig_store), &it, path)) {
        int idx = -1;
        gboolean vis = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(g_sig_store), &it,
                           SIG_COL_INDEX, &idx,
                           SIG_COL_VISIBLE, &vis, -1);
        if (idx >= 0 && idx < g_nvis) {
            g_vis[idx] = !vis;
            gtk_list_store_set(g_sig_store, &it, SIG_COL_VISIBLE, g_vis[idx], -1);
            sig_sync_combo();
            update_status();
            gtk_widget_queue_draw(g_area);
        }
    }
    gtk_tree_path_free(path);
}

static void on_sig_all(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    for (int s = 0; s < g_nvis; s++)
        g_vis[s] = TRUE;
    sig_push_to_store();
    sig_sync_combo();
    update_status();
    gtk_widget_queue_draw(g_area);
}

static void on_sig_none(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    for (int s = 0; s < g_nvis; s++)
        g_vis[s] = FALSE;
    sig_push_to_store();
    sig_sync_combo();
    update_status();
    gtk_widget_queue_draw(g_area);
}

static void on_vref_changed(GtkEntry *entry, gpointer data)
{
    (void)data;
    const char *txt = gtk_entry_get_text(entry);
    char *end = NULL;
    while (*txt == ' ' || *txt == '\t') txt++;
    if (*txt == '\0') {
        g_vref_on = FALSE;
    } else {
        double v = strtod(txt, &end);
        if (end != txt) {
            g_vref = v;
            g_vref_on = TRUE;
        } else {
            g_vref_on = FALSE;
        }
    }
    gtk_widget_queue_draw(g_area);
}

static void on_zoom(GtkButton *btn, gpointer data)
{
    (void)btn;
    const char *action = (const char *)data;
    double tmin, tmax;
    if (!data_time_range(&tmin, &tmax)) {
        g_zoom = 1.0;
        return;
    }
    /* Keep the current centre stable across zoom steps. */
    double old_vis = visible_width(tmin, tmax);
    double center = g_view_t0 + old_vis / 2.0;
    if (!(g_zoom > 1.0)) {
        /* At fit-all, centre is the data centre. */
        center = (tmin + tmax) / 2.0;
        old_vis = tmax - tmin;
        (void)old_vis;
    }

    if (strcmp(action, "in") == 0)
        g_zoom *= 1.25;
    else if (strcmp(action, "out") == 0)
        g_zoom /= 1.25;
    else {
        g_zoom = 1.0;
        g_view_t0 = tmin;
        update_scrollbar();
        update_status();
        gtk_widget_queue_draw(g_area);
        return;
    }
    if (g_zoom < 1.0) g_zoom = 1.0;
    if (g_zoom > 100.0) g_zoom = 100.0;

    double vis = visible_width(tmin, tmax);
    g_view_t0 = clamp_t0(center - vis / 2.0, tmin, tmax, vis);
    update_scrollbar();
    update_status();
    gtk_widget_queue_draw(g_area);
}

static void on_export(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    if (!g_loaded) {
        GtkWindow *parent = GTK_WINDOW(gtk_widget_get_toplevel(g_area));
        GtkWidget *dlg = gtk_message_dialog_new(parent,
                                                 GTK_DIALOG_MODAL,
                                                 GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                 "Nothing to export - open a file first.");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_toplevel(g_area));
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Export SVG", parent, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "waveform.svg");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        double w = gtk_widget_get_allocated_width(g_area);
        double h = gtk_widget_get_allocated_height(g_area);
        if (!(w > 0)) w = 900;
        if (!(h > 0)) h = 450;
        cairo_surface_t *surf = cairo_svg_surface_create(path, w, h);
        cairo_t *cr = cairo_create(surf);
        render_plot(cr, w, h);
        cairo_destroy(cr);
        cairo_surface_finish(surf);
        cairo_surface_destroy(surf);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "viewdat - Ngspice waveform viewer");
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 560);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Toolbar. */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 4);

    GtkWidget *open_btn = gtk_button_new_with_label("Open");
    gtk_box_pack_start(GTK_BOX(bar), open_btn, FALSE, FALSE, 0);

    g_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_combo), "All signals");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), 0);
    gtk_box_pack_start(GTK_BOX(bar), g_combo, FALSE, FALSE, 0);

    GtkWidget *vref_lab = gtk_label_new("Vref:");
    gtk_box_pack_start(GTK_BOX(bar), vref_lab, FALSE, FALSE, 0);
    g_vref_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(g_vref_entry), 8);
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_vref_entry), "e.g. 0.5");
    gtk_box_pack_start(GTK_BOX(bar), g_vref_entry, FALSE, FALSE, 0);

    GtkWidget *zin = gtk_button_new_with_label("Zoom +");
    GtkWidget *zout = gtk_button_new_with_label("Zoom -");
    GtkWidget *zreset = gtk_button_new_with_label("Reset");
    gtk_box_pack_start(GTK_BOX(bar), zin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), zout, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), zreset, FALSE, FALSE, 0);

    GtkWidget *exp = gtk_button_new_with_label("Export SVG");
    gtk_box_pack_start(GTK_BOX(bar), exp, FALSE, FALSE, 0);

    /* Main content: left signal browser + right plot area. */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), hpaned, TRUE, TRUE, 0);

    /* Left: signal browser for the currently opened waveform. */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(left_vbox, 200, -1);
    gtk_paned_pack1(GTK_PANED(hpaned), left_vbox, FALSE, FALSE);

    GtkWidget *sig_title = gtk_label_new("Signals");
    gtk_label_set_xalign(GTK_LABEL(sig_title), 0.0);
    gtk_box_pack_start(GTK_BOX(left_vbox), sig_title, FALSE, FALSE, 2);

    GtkWidget *sig_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(left_vbox), sig_btns, FALSE, FALSE, 0);
    GtkWidget *all_btn = gtk_button_new_with_label("All");
    GtkWidget *none_btn = gtk_button_new_with_label("None");
    gtk_box_pack_start(GTK_BOX(sig_btns), all_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sig_btns), none_btn, TRUE, TRUE, 0);

    g_sig_store = gtk_list_store_new(SIG_N_COLS,
                                     G_TYPE_BOOLEAN,
                                     G_TYPE_STRING,
                                     G_TYPE_STRING,
                                     G_TYPE_INT);
    GtkWidget *sig_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_sig_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(sig_view), FALSE);
    g_object_unref(g_sig_store);

    GtkCellRenderer *tog = gtk_cell_renderer_toggle_new();
    g_signal_connect(tog, "toggled", G_CALLBACK(on_sig_toggled), NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sig_view), -1, "Show",
                                                tog, "active", SIG_COL_VISIBLE, NULL);

    GtkCellRenderer *sw = gtk_cell_renderer_text_new();
    g_object_set(sw, "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sig_view), -1, "",
                                                sw, "text", SIG_COL_SWATCH,
                                                "foreground", SIG_COL_SWATCH, NULL);

    GtkCellRenderer *nm = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sig_view), -1, "Signal",
                                                nm, "text", SIG_COL_NAME, NULL);

    GtkWidget *sig_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sig_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sig_scroll), sig_view);
    gtk_box_pack_start(GTK_BOX(left_vbox), sig_scroll, TRUE, TRUE, 0);

    /* Right: plot + time scrollbar. */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(hpaned), right_vbox, TRUE, FALSE);

    /* Plot area. */
    g_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g_area, TRUE);
    gtk_widget_set_vexpand(g_area, TRUE);
    gtk_box_pack_start(GTK_BOX(right_vbox), g_area, TRUE, TRUE, 0);
    g_signal_connect(g_area, "draw", G_CALLBACK(on_draw), NULL);

    /* Bottom horizontal scrollbar for time scrolling when zoomed.
     * Adjustment is in time units: value = left edge, page = visible width. */
    g_hadj = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
    g_hscroll = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, g_hadj);
    gtk_widget_set_sensitive(g_hscroll, FALSE);
    gtk_box_pack_start(GTK_BOX(right_vbox), g_hscroll, FALSE, FALSE, 0);
    g_signal_connect(g_hadj, "value-changed", G_CALLBACK(on_scroll_changed), NULL);

    g_status = gtk_label_new("No file loaded.");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 4);

    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open), win);
    g_signal_connect(g_combo, "changed", G_CALLBACK(on_combo_changed), NULL);
    g_signal_connect(g_vref_entry, "changed", G_CALLBACK(on_vref_changed), NULL);
    g_signal_connect(zin, "clicked", G_CALLBACK(on_zoom), (gpointer) "in");
    g_signal_connect(zout, "clicked", G_CALLBACK(on_zoom), (gpointer) "out");
    g_signal_connect(zreset, "clicked", G_CALLBACK(on_zoom), (gpointer) "reset");
    g_signal_connect(exp, "clicked", G_CALLBACK(on_export), NULL);
    g_signal_connect(all_btn, "clicked", G_CALLBACK(on_sig_all), NULL);
    g_signal_connect(none_btn, "clicked", G_CALLBACK(on_sig_none), NULL);

    gtk_widget_show_all(win);

    if (argc > 1) {
        if (!load_file(argv[1])) {
            char msg[512];
            snprintf(msg, sizeof msg, "Could not read '%s'.", argv[1]);
            show_error(GTK_WINDOW(win), msg);
        }
    } else {
        update_scrollbar();
    }

    gtk_main();

    if (g_path) free(g_path);
    free(g_vis);
    dat_free(&g_dat);

    return 0;
}

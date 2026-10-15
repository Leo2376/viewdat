/*
 * viewdat-gui.c - GTK3 + Cairo GUI viewer for Ngspice .dat files.
 *
 * Usage: viewdat-gui [file.dat]
 *
 * Features: open .dat files, plot all signals vs time, per-signal
 * selector, reference-voltage line, zoom in/out/reset, SVG export.
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
static GtkWidget *g_combo;
static GtkWidget *g_vref_entry;
static GtkWidget *g_status;

static double g_zoom = 1.0;
static int g_show = -1; /* -1 = all signals */
static gboolean g_vref_on = FALSE;
static double g_vref = 0.0;
static double g_scroll_offset = 0.0; /* horizontal scroll offset in pixels */

static const double PALETTE[][3] = {
    {0.12, 0.47, 0.71}, {1.00, 0.50, 0.05}, {0.17, 0.63, 0.17},
    {0.84, 0.15, 0.16}, {0.58, 0.40, 0.74}, {0.55, 0.34, 0.29},
    {0.89, 0.47, 0.76}, {0.50, 0.50, 0.50},
};

/* Draw the waveform on cr (size W x H). Shared by screen + SVG export. */
static void render_plot(cairo_t *cr, int W, int H,
                        double scroll_offset, double zoom,
                        double tmin, double tmax,
                        double vmin, double vmax, double vscale)
{
    const int ML = 70, MR = 15, MT = 12, MB = 40;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    if (!g_loaded || g_dat.nrows == 0) {
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, 30, 50);
        cairo_show_text(cr, "No file loaded - click Open to choose a .dat file.");
        return;
    }

    (void)scroll_offset;
    (void)zoom;
    (void)vscale;
    dat_range(&g_dat, &tmin, &tmax, &vmin, &vmax);
    if (!(tmax > tmin)) {
        tmax = tmin + 1.0;
    }
    if (!(vmax > vmin)) {
        vmax = vmin + 1.0;
    }

    /* Zoom around the data centre. */
    double tc = (tmin + tmax) / 2.0, vc = (vmin + vmax) / 2.0;
    double th = (tmax - tmin) / 2.0 / g_zoom;
    double vh = (vmax - vmin) / 2.0 / g_zoom;
    if (th <= 0)
        th = 1.0;
    if (vh <= 0)
        vh = 1.0;
    /* Keep a 5% margin. */
    th *= 1.05;
    vh *= 1.05;
    double vtmin = tc - th, vtmax = tc + th;
    double vvmin = vc - vh, vvmax = vc + vh;

    int pw = W - ML - MR, ph = H - MT - MB;

    /* Grid. */
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 10; i++) {
        double x = ML + (double)pw * i / 10.0;
        cairo_move_to(cr, x, MT);
        cairo_line_to(cr, x, MT + ph);
        cairo_stroke(cr);
    }
    for (int i = 0; i <= 8; i++) {
        double y = MT + (double)ph * i / 8.0;
        cairo_move_to(cr, ML, y);
        cairo_line_to(cr, ML + pw, y);
        cairo_stroke(cr);
    }

    /* Frame. */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, ML, MT, pw, ph);
    cairo_stroke(cr);

    int ns = dat_nsignals(&g_dat);
    int s0 = (g_show >= 0) ? g_show : 0;
    int s1 = (g_show >= 0) ? g_show : ns - 1;
    int stride = g_dat.nrows > 4000 ? g_dat.nrows / 4000 : 1;

    for (int s = s0; s <= s1; s++) {
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
            double x = ML + (t - vtmin) / (vtmax - vtmin) * pw;
            double y = MT + (vvmax - v) / (vvmax - vvmin) * ph;
            if (pen)
                cairo_line_to(cr, x, y);
            else {
                cairo_move_to(cr, x, y);
                pen = TRUE;
            }
        }
        cairo_stroke(cr);
    }

    /* Reference voltage line. */
    if (g_vref_on && g_vref >= vvmin && g_vref <= vvmax) {
        double y = MT + (vvmax - g_vref) / (vvmax - vvmin) * ph;
        static const double dash[] = { 6.0, 4.0 };
        cairo_set_source_rgb(cr, 0.9, 0.1, 0.1);
        cairo_set_line_width(cr, 1.2);
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, ML, y);
        cairo_line_to(cr, ML + pw, y);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }

    /* Axis labels. */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    char buf[128];
    snprintf(buf, sizeof buf, "%g", vvmax);
    cairo_move_to(cr, 4, MT + 10);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof buf, "%g", vvmin);
    cairo_move_to(cr, 4, MT + ph);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof buf, "t: %g .. %g", vtmin, vtmax);
    cairo_move_to(cr, ML, H - 12);
    cairo_show_text(cr, buf);

    /* Legend for single-signal mode. */
    if (g_show >= 0) {
        snprintf(buf, sizeof buf, "sig%d", g_show);
        const double *c = PALETTE[g_show % 8];
        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
        cairo_move_to(cr, ML + 8, MT + 14);
        cairo_show_text(cr, buf);
    }
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;
    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    double tmin, tmax, vmin, vmax;
    dat_range(&g_dat, &tmin, &tmax, &vmin, &vmax);
    render_plot(cr, W, H, g_scroll_offset, g_zoom, tmin, tmax, vmin, vmax, 1.0);
    return FALSE;
}

static void update_status(void)
{
    char buf[256];
    if (!g_loaded) {
        gtk_label_set_text(GTK_LABEL(g_status), "No file loaded.");
        return;
    }
    double tmin, tmax, vmin, vmax;
    dat_range(&g_dat, &tmin, &tmax, &vmin, &vmax);
    snprintf(buf, sizeof buf,
             "%s  |  rows: %d  signals: %d  (%s)  |  t: %g..%g  v: %g..%g  |  zoom: %.2fx",
             g_path ? g_path : "?",
             g_dat.nrows, dat_nsignals(&g_dat),
             g_dat.paired ? "paired t,v" : "plain",
             tmin, tmax, vmin, vmax, g_zoom);
    gtk_label_set_text(GTK_LABEL(g_status), buf);
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

    /* Rebuild the signal selector. */
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
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Ngspice data (*.dat)");
    gtk_file_filter_add_pattern(f, "*.dat");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
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
    int active = gtk_combo_box_get_active(box);
    g_show = active - 1; /* row 0 = All */
    gtk_widget_queue_draw(g_area);
}

static void on_vref_changed(GtkEntry *entry, gpointer data)
{
    (void)data;
    const char *txt = gtk_entry_get_text(entry);
    char *end = NULL;
    while (*txt == ' ' || *txt == '\t')
        txt++;
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
    const char *which = (const char *)data;
    if (strcmp(which, "in") == 0)
        g_zoom *= 1.25;
    else if (strcmp(which, "out") == 0)
        g_zoom /= 1.25;
    else
        g_zoom = 1.0;
    if (g_zoom < 0.1)
        g_zoom = 0.1;
    if (g_zoom > 100.0)
        g_zoom = 100.0;
    update_status();
    gtk_widget_queue_draw(g_area);
}

static void on_export_svg(void)
{
    if (!g_loaded) {
        gtk_message_dialog_new(GTK_WINDOW(g_area), 0,
                                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                "Nothing to export - open a file first.");
        return;
    }
    GtkWidget * dlg = gtk_file_chooser_dialog_new(
        "Export SVG", GTK_WINDOW(g_area), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "waveform.svg");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        cairo_surface_t *surf = cairo_svg_surface_create(path, 800, 400);
        cairo_t *cr = cairo_create(surf);
        double tmin, tmax, vmin, vmax;
        dat_range(&g_dat, &tmin, &tmax, &vmin, &vmax);
        render_plot(cr, 800, 400, g_scroll_offset, g_zoom, tmin, tmax, vmin, vmax, 1.0);
        cairo_destroy(cr);
        cairo_surface_finish(surf);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_export(GtkButton *btn, gpointer data)
{
    (void)btn;
    on_export_svg();
}

int main(int argc, char *argv[])
{
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

    g_hscroll = gtk_hscrollbar_new(NULL);
    gtk_box_pack_start(GTK_BOX(bar), g_hscroll, TRUE, TRUE, 2);

    GtkWidget *exp = gtk_button_new_with_label("Export SVG");
    gtk_box_pack_start(GTK_BOX(bar), exp, FALSE, FALSE, 0);

    /* Plot area. */
    g_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_area, 800, 450);
    gtk_widget_set_hexpand(g_area, TRUE);
    gtk_widget_set_vexpand(g_area, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), g_area, TRUE, TRUE, 0);

    /* Status line. */
    g_status = gtk_label_new("No file loaded.");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 4);

    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open), win);
    g_signal_connect(g_combo, "changed", G_CALLBACK(on_combo_changed), NULL);
    g_signal_connect(g_vref_entry, "changed", G_CALLBACK(on_vref_changed), NULL);
    g_signal_connect(zin, "clicked", G_CALLBACK(on_zoom), (gpointer) "in");
    g_signal_connect(zout, "clicked", G_CALLBACK(on_zoom), (gpointer) "out");
    g_signal_connect(zreset, "clicked", G_CALLBACK(on_zoom), (gpointer) "reset");
    g_signal_connect(exp, "clicked", G_CALLBACK(on_export), win);
    g_signal_connect(g_area, "draw", G_CALLBACK(on_draw), NULL);

    gtk_widget_show_all(win);

    if (argc > 1) {
        if (!load_file(argv[1])) {
            char msg[512];
            snprintf(msg, sizeof msg, "Could not read '%s'.", argv[1]);
            show_error(GTK_WINDOW(win), msg);
        }
    }

    gtk_main();

    dat_free(&g_dat);
    free(g_path);
    return 0;
}

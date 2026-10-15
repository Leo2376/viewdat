/*
 * viewdat.c - Terminal viewer for Ngspice .dat files.
 *
 * Usage: viewdat <file.dat> [-c COL|all] [-n ROWS] [--csv OUT] [--svg OUT]
 *
 * Prints stats, an ASCII preview plot, and can export CSV or SVG.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datreader.h"

static void usage(const char *prog)
{
    printf("Usage: %s <file.dat> [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -c COL    Signal to plot (0-based, default 0; use 'all' to overlay)\n");
    printf("  -n ROWS   Rows to print in the listing (default 10, 0 = none)\n");
    printf("  --csv F   Export time,value columns to CSV file F\n");
    printf("  --svg F   Export an SVG plot to file F\n");
    printf("  -h        Show this help\n");
}

static int parse_signals(const DatFile *d, const char *arg,
                         int *sels, int maxsel)
{
    int ns = dat_nsignals(d);
    if (strcmp(arg, "all") == 0) {
        int n = ns < maxsel ? ns : maxsel;
        for (int i = 0; i < n; i++)
            sels[i] = i;
        return n;
    }
    int c = atoi(arg);
    if (c < 0 || c >= ns) {
        fprintf(stderr, "viewdat: signal %d out of range (0..%d)\n",
                c, ns - 1);
        return -1;
    }
    sels[0] = c;
    return 1;
}

/* Simple ASCII plot of the selected signals. */
static void ascii_plot(const DatFile *d, const int *sels, int nsel)
{
    const int W = 70, H = 18;
    double tmin, tmax, vmin, vmax;
    char grid[H][W + 1];
    const char glyphs[] = "*+#xo%";

    dat_range(d, &tmin, &tmax, &vmin, &vmax);
    if (!(vmax > vmin)) {
        vmax = vmin + 1.0;
    }

    for (int y = 0; y < H; y++) {
        memset(grid[y], ' ', (size_t)W);
        grid[y][W] = '\0';
    }

    int stride = d->nrows > 2000 ? d->nrows / 2000 : 1;
    for (int k = 0; k < nsel; k++) {
        char g = glyphs[k % (int)(sizeof glyphs - 1)];
        for (int r = 0; r < d->nrows; r += stride) {
            double t = dat_time(d, sels[k], r);
            double v = dat_value(d, sels[k], r);
            if (isnan(t) || isnan(v))
                continue;
            int x = (tmax > tmin) ? (int)((t - tmin) / (tmax - tmin) * (W - 1)) : 0;
            int y = (int)((vmax - v) / (vmax - vmin) * (H - 1));
            if (x < 0)
                x = 0;
            if (x >= W)
                x = W - 1;
            if (y < 0)
                y = 0;
            if (y >= H)
                y = H - 1;
            grid[y][x] = g;
        }
    }

    printf("+");
    for (int x = 0; x < W; x++)
        putchar('-');
    printf("+\n");
    for (int y = 0; y < H; y++)
        printf("|%s|\n", grid[y]);
    printf("+");
    for (int x = 0; x < W; x++)
        putchar('-');
    printf("+\n");
    printf("t: [%g .. %g]  v: [%g .. %g]\n", tmin, tmax, vmin, vmax);
}

static int export_csv(const DatFile *d, const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "viewdat: cannot write '%s'\n", path);
        return -1;
    }
    int ns = dat_nsignals(d);
    fprintf(fp, "time");
    for (int s = 0; s < ns; s++)
        fprintf(fp, ",sig%d", s);
    fprintf(fp, "\n");
    for (int r = 0; r < d->nrows; r++) {
        fprintf(fp, "%.12e", dat_time(d, 0, r));
        for (int s = 0; s < ns; s++)
            fprintf(fp, ",%.12e", dat_value(d, s, r));
        fprintf(fp, "\n");
    }
    fclose(fp);
    printf("wrote %s (%d rows, %d signals)\n", path, d->nrows, ns);
    return 0;
}

static int export_svg(const DatFile *d, const char *path)
{
    static const char *colors[] = {
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
        "#9467bd", "#8c564b", "#e377c2", "#7f7f7f"
    };
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "viewdat: cannot write '%s'\n", path);
        return -1;
    }
    const int W = 800, H = 400, ML = 70, MR = 20, MT = 20, MB = 45;
    double tmin, tmax, vmin, vmax;
    int ns = dat_nsignals(d);

    dat_range(d, &tmin, &tmax, &vmin, &vmax);
    if (!(tmax > tmin)) {
        tmax = tmin + 1.0;
    }
    if (!(vmax > vmin)) {
        vmax = vmin + 1.0;
    }

    fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                "width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            W, H, W, H);
    fprintf(fp, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"white\"/>\n",
            W, H);
    /* grid */
    fprintf(fp, "<g stroke=\"#dddddd\" stroke-width=\"1\">\n");
    for (int i = 0; i <= 10; i++) {
        int x = ML + (W - ML - MR) * i / 10;
        fprintf(fp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>\n",
                x, MT, x, H - MB);
    }
    for (int i = 0; i <= 8; i++) {
        int y = MT + (H - MT - MB) * i / 8;
        fprintf(fp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>\n",
                ML, y, W - MR, y);
    }
    fprintf(fp, "</g>\n");
    fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                "fill=\"none\" stroke=\"black\"/>\n",
            ML, MT, W - ML - MR, H - MT - MB);

    int stride = d->nrows > 4000 ? d->nrows / 4000 : 1;
    for (int s = 0; s < ns; s++) {
        fprintf(fp, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"1.2\" points=\"",
                colors[s % 8]);
        for (int r = 0; r < d->nrows; r += stride) {
            double t = dat_time(d, s, r);
            double v = dat_value(d, s, r);
            if (isnan(t) || isnan(v))
                continue;
            double x = ML + (t - tmin) / (tmax - tmin) * (W - ML - MR);
            double y = MT + (vmax - v) / (vmax - vmin) * (H - MT - MB);
            fprintf(fp, "%.1f,%.1f ", x, y);
        }
        fprintf(fp, "\"/>\n");
    }
    fprintf(fp, "<text x=\"%d\" y=\"%d\" font-size=\"12\" font-family=\"sans-serif\">"
                "t: %g .. %g</text>\n",
            ML, H - 10, tmin, tmax);
    fprintf(fp, "</svg>\n");
    fclose(fp);
    printf("wrote %s (%d rows, %d signals)\n", path, d->nrows, ns);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *file = NULL;
    const char *colarg = "0";
    const char *csvout = NULL;
    const char *svgout = NULL;
    int nlist = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            colarg = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nlist = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csvout = argv[++i];
        } else if (strcmp(argv[i], "--svg") == 0 && i + 1 < argc) {
            svgout = argv[++i];
        } else if (argv[i][0] != '-') {
            file = argv[i];
        } else {
            fprintf(stderr, "viewdat: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!file) {
        usage(argv[0]);
        return 1;
    }

    DatFile d;
    if (dat_load(file, &d) != 0)
        return 1;

    int ns = dat_nsignals(&d);
    double tmin, tmax, vmin, vmax;
    dat_range(&d, &tmin, &tmax, &vmin, &vmax);

    printf("file: %s\n", file);
    printf("rows: %d  raw cols: %d  layout: %s  signals: %d\n",
           d.nrows, d.ncols, d.paired ? "paired (t,v pairs)" : "plain (t,v1..vN)",
           ns);
    printf("time range:  %g .. %g\n", tmin, tmax);
    for (int s = 0; s < ns; s++) {
        double lo = INFINITY, hi = -INFINITY;
        for (int r = 0; r < d.nrows; r++) {
            double v = dat_value(&d, s, r);
            if (!isnan(v)) {
                if (v < lo)
                    lo = v;
                if (v > hi)
                    hi = v;
            }
        }
        printf("sig%-3d min=%12g max=%12g\n", s, lo, hi);
    }

    int sels[8];
    int nsel = parse_signals(&d, colarg, sels, 8);
    if (nsel < 0) {
        dat_free(&d);
        return 1;
    }
    printf("\n");
    ascii_plot(&d, sels, nsel);

    if (nlist > 0) {
        int n = nlist < d.nrows ? nlist : d.nrows;
        printf("\nfirst %d rows (time + signals):\n", n);
        for (int r = 0; r < n; r++) {
            printf("%4d t=%.6e", r, dat_time(&d, 0, r));
            for (int s = 0; s < ns && s < 6; s++)
                printf("  v%d=%.6g", s, dat_value(&d, s, r));
            printf("\n");
        }
    }

    int rc = 0;
    if (csvout && export_csv(&d, csvout) != 0)
        rc = 1;
    if (svgout && export_svg(&d, svgout) != 0)
        rc = 1;

    dat_free(&d);
    return rc;
}

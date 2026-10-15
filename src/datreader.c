/*
 * datreader.c - Read Ngspice .dat waveform files.
 */
#include "datreader.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse one line into tmp (resized as needed). Returns token count,
 * 0 for empty lines, -1 when the line holds non-numeric text. */
static int parse_line(char *line, double **tmp, int *tmpcap)
{
    int n = 0;
    char *p = line;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',' ||
               *p == '\r' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        if (*p == '#' || *p == '*')
            break; /* trailing comment */
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p)
            return -1; /* non-numeric text -> header line */
        if (n >= *tmpcap) {
            int ncap = *tmpcap ? (*tmpcap * 2) : 16;
            double *nt = realloc(*tmp, (size_t)ncap * sizeof *nt);
            if (!nt)
                return -1;
            *tmp = nt;
            *tmpcap = ncap;
        }
        (*tmp)[n++] = v;
        p = end;
    }
    return n;
}

int dat_load(const char *path, DatFile *d)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "viewdat: cannot open '%s'\n", path);
        return -1;
    }

    double *rows = NULL;
    size_t cap = 0, len = 0;
    int ncols = -1, nrows = 0;
    double *tmp = NULL;
    int tmpcap = 0;
    char *line = NULL;
    size_t llen = 0;
    ssize_t got;

    d->data = NULL;
    d->nrows = d->ncols = 0;
    d->paired = 0;

    while ((got = getline(&line, &llen, fp)) >= 0) {
        char *s = line;
        (void)got;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '\0' || *s == '\n' || *s == '\r' ||
            *s == '#' || *s == '*')
            continue;
        int n = parse_line(line, &tmp, &tmpcap);
        if (n <= 0)
            continue; /* empty or header text */
        if (ncols < 0)
            ncols = n;
        if (len + (size_t)ncols > cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            while (ncap < len + (size_t)ncols)
                ncap *= 2;
            double *nr = realloc(rows, ncap * sizeof *nr);
            if (!nr) {
                fprintf(stderr, "viewdat: out of memory\n");
                free(rows);
                free(tmp);
                free(line);
                fclose(fp);
                return -1;
            }
            rows = nr;
            cap = ncap;
        }
        for (int i = 0; i < ncols; i++)
            rows[len + i] = (i < n) ? tmp[i] : NAN;
        len += (size_t)ncols;
        nrows++;
    }

    free(tmp);
    free(line);
    fclose(fp);

    if (nrows == 0 || ncols < 2) {
        fprintf(stderr, "viewdat: no numeric data in '%s'\n", path);
        free(rows);
        return -1;
    }

    d->data = rows;
    d->nrows = nrows;
    d->ncols = ncols;
    d->paired = 0;

    /* Detect paired (time,value) layout: all even columns equal. */
    if (ncols >= 4 && (ncols % 2) == 0) {
        int k = nrows < 10 ? nrows : 10;
        int ok = 1;
        for (int r = 0; r < k && ok; r++) {
            double t0 = rows[(size_t)r * ncols];
            if (isnan(t0)) {
                ok = 0;
                break;
            }
            for (int c = 2; c < ncols; c += 2) {
                double t = rows[(size_t)r * ncols + c];
                if (isnan(t)) {
                    ok = 0;
                    break;
                }
                double scale = fabs(t0) > 0.0 ? fabs(t0) : 1.0;
                if (fabs(t - t0) / scale > 1e-6 && fabs(t - t0) > 1e-30) {
                    ok = 0;
                    break;
                }
            }
        }
        d->paired = ok;
    }

    return 0;
}

void dat_free(DatFile *d)
{
    if (d) {
        free(d->data);
        d->data = NULL;
        d->nrows = d->ncols = 0;
        d->paired = 0;
    }
}

int dat_nsignals(const DatFile *d)
{
    if (!d || d->ncols < 2)
        return 0;
    return d->paired ? d->ncols / 2 : d->ncols - 1;
}

double dat_time(const DatFile *d, int sig, int row)
{
    if (!d || !d->data)
        return NAN;
    if (d->paired)
        return d->data[(size_t)row * d->ncols + 2 * sig];
    return d->data[(size_t)row * d->ncols];
}

double dat_value(const DatFile *d, int sig, int row)
{
    if (!d || !d->data)
        return NAN;
    if (d->paired)
        return d->data[(size_t)row * d->ncols + 2 * sig + 1];
    return d->data[(size_t)row * d->ncols + 1 + sig];
}

void dat_range(const DatFile *d,
               double *tmin, double *tmax,
               double *vmin, double *vmax)
{
    double lo_t = INFINITY, hi_t = -INFINITY;
    double lo_v = INFINITY, hi_v = -INFINITY;
    int ns = dat_nsignals(d);

    for (int s = 0; s < ns; s++) {
        for (int r = 0; r < d->nrows; r++) {
            double t = dat_time(d, s, r);
            double v = dat_value(d, s, r);
            if (!isnan(t)) {
                if (t < lo_t)
                    lo_t = t;
                if (t > hi_t)
                    hi_t = t;
            }
            if (!isnan(v)) {
                if (v < lo_v)
                    lo_v = v;
                if (v > hi_v)
                    hi_v = v;
            }
        }
    }
    if (tmin)
        *tmin = lo_t;
    if (tmax)
        *tmax = hi_t;
    if (vmin)
        *vmin = lo_v;
    if (vmax)
        *vmax = hi_v;
}

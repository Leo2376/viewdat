/*
 * datreader.h - Read Ngspice .dat waveform files.
 *
 * Supported layout: whitespace- and/or comma-separated numeric columns,
 * with '#' or '*' comment lines skipped. Two layouts are recognised:
 *
 *   plain : col 0 = time, cols 1..N = signal values
 *   paired: (time0 val0 time1 val1 ...) - the wrdata style where every
 *           signal carries its own time column (all time columns equal)
 *
 * The loader auto-detects the layout (see DatFile.paired).
 */

#ifndef DATREADER_H
#define DATREADER_H

typedef struct {
    double *data;   /* row-major values, size nrows * ncols */
    int     nrows;  /* number of data rows */
    int     ncols;  /* number of numeric columns per row */
    int     paired; /* 1 = (time,value) pairs, 0 = col0 is time */
} DatFile;

/* Load a .dat file. Returns 0 on success, -1 on failure. */
int dat_load(const char *path, DatFile *d);

/* Release memory held by a DatFile. */
void dat_free(DatFile *d);

/* Number of signals (paired: ncols/2, plain: ncols-1). */
int dat_nsignals(const DatFile *d);

/* Time / value accessors (row in [0, nrows), sig in [0, nsignals)). */
double dat_time(const DatFile *d, int sig, int row);
double dat_value(const DatFile *d, int sig, int row);

/* Global ranges over all signals (NaN cells ignored). */
void dat_range(const DatFile *d,
               double *tmin, double *tmax,
               double *vmin, double *vmax);

#endif /* DATREADER_H */

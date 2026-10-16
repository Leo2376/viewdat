# ViewDat

**viewdat** is a small C project for inspecting Ngspice `.dat` waveform
files (the `wrdata`-style numeric output). It ships two tools that share
one parser:

| Tool | Binary | Interface |
|------|--------|-----------|
| Terminal viewer | `viewdat` | Stats, ASCII plot, CSV/SVG export |
| GUI viewer | `viewdat-gui` | GTK3 + Cairo plot with zoom, Vref line, SVG export |

There is no eye-diagram folding, no multi-file overlay, and no HTML/PDF
output — the old README described options that were never implemented.
What is documented below is what the code actually does.

## Scope

- Read a single `.dat` file per invocation.
- Report rows, columns, detected layout, per-signal min/max.
- Plot signals vs. time (ASCII in the terminal, interactive canvas in
  the GUI).
- Export the data (`--csv`) or a plot (`--svg`) for further processing.
- Gracefully reject files with no numeric data instead of crashing.

## File format

Lines are whitespace- and/or comma-separated numbers. Blank lines and
lines starting with `#` or `*` are skipped. Two layouts are recognised
and auto-detected:

- **plain**: `time value1 value2 ... valueN` (column 0 is the time base).
- **paired**: `time0 value0 time1 value1 ...` — the Ngspice `wrdata`
  style where every vector carries its own time column. Detected when
  all even columns agree; each pair becomes one signal plotted against
  its own time column.

Example (paired, from `examples/tx_T4_eye.dat`):

```
5.00000000e-14  8.41141250e-01  5.00000000e-14  3.75000000e-03 ...
5.13583100e-14  8.42249271e-01  5.13583100e-14  3.85187325e-03 ...
```

Note: `examples/example_waveform.dat` is documentation only (comments,
no numeric rows) and is correctly reported as having no data.

## Sources

```
src/
├── datreader.h    # Shared parser API (DatFile, dat_load/dat_free,
│                  # dat_nsignals, dat_time/dat_value, dat_range)
├── datreader.c    # Parser implementation (plain + paired detection)
├── viewdat.c      # Terminal viewer (main + stats/ASCII/CSV/SVG)
└── viewdat-gui.c  # GTK3 GUI viewer (main + Cairo rendering)
```

Both binaries link `datreader.c`. The GUI's on-screen drawing and its
SVG export share one `render_plot()` function, so what you see is what
gets exported.

## Building

Requirements:

- Terminal tool: `gcc`, libc, libm.
- GUI tool: additionally GTK+ 3 and cairo headers
  (`pkg-config --cflags --libs gtk+-3.0` must work; on Debian/Ubuntu:
  `libgtk-3-dev`).

```bash
make               # builds ./viewdat and ./viewdat-gui
make viewdat       # terminal viewer only
make viewdat-gui   # GUI viewer only
make clean         # remove binaries
sudo make install  # install both to /usr/local/bin (PREFIX/DESTDIR honoured)
```

The build is warning-clean under `gcc -Wall -Wextra -O2 -std=c99`.

## Usage — terminal (`viewdat`)

```
viewdat <file.dat> [-c COL|all] [-n ROWS] [--csv OUT] [--svg OUT]
```

| Option | Description |
|--------|-------------|
| `-c COL` | Signal to plot, 0-based (default `0`); `all` overlays up to 8 signals |
| `-n ROWS` | Data rows to list after the plot (default 10, `0` = none) |
| `--csv OUT` | Write `time,sig0,sig1,…` to `OUT` |
| `--svg OUT` | Write an 800×400 SVG plot to `OUT` |
| `-h` | Show help |

Examples:

```bash
./viewdat examples/tx_T4_eye.dat
./viewdat examples/rx_T4_eye.dat -c all -n 0
./viewdat examples/loopback_T4.dat --csv loop.csv --svg loop.svg -n 0
./viewdat examples/tx_prbs_simple_eye.dat --svg eye.svg -n 0
```

Sample output:

```
file: examples/rx_T4_eye.dat
rows: 598  raw cols: 8  layout: paired (t,v pairs)  signals: 4
time range:  5e-14 .. 2.2e-09
sig0   min=   0.0587217 max=    0.122867
sig1   min=         0.5 max=         0.7
...
```

## Usage — GUI (`viewdat-gui`)

```bash
./viewdat-gui [file.dat]
```

With no argument it starts empty and asks you to open a file. The
toolbar offers:

- **Open** — file chooser (`.dat` filter) to load another file.
- **Signal selector** — `All signals` overlay or a single `Signal N`.
- **Vref** — type a voltage (e.g. `0.5`) to draw a dashed red reference
  line; clear the field to hide it.
- **Zoom + / Zoom − / Reset** — zoom the time (X) axis only around the
  current centre (1×–100×). Signal height always fits the full value
  range.
- **Bottom horizontal scrollbar** — scroll through time when zoomed in;
  disabled at 1× (full view).
- **Export SVG** — save the current view as an 800×400 SVG file.

The status bar shows the file name, row/signal counts, detected layout,
data ranges, and the current zoom factor.

## Examples directory

`examples/` holds 17 sample traces (transient and eye-style runs,
`T4`/PRBS/bytelane/dcosim sets):

```
buffer_test.dat        bytelane_eye.dat       dcosim_debug.dat
dcosim_debug_1p375.dat dcosim_simple_load.dat example_waveform.dat (doc only)
loopback_T4.dat        rx_T4_eye.dat          rx_T4_fixed.dat
rx_T4_fullswing.dat    tx_dcosim_eye.dat      tx_debug.dat
tx_prbs7_eye.dat       tx_prbs_simple_eye.dat tx_pwl_debug.dat
tx_T4_eye.dat          tx_T4_ST.dat
```

Most are paired-layout files with 4–16 raw columns; `buffer_test.dat`
parses as plain layout. All except `example_waveform.dat` load in both
tools.

## Limitations

- One input file at a time; no signal arithmetic or filtering.
- No time-window selection, no eye-diagram folding, no HTML/PDF output.
- Very large files are downsampled for drawing (plots and ASCII preview)
  but CSV export always writes every row.

## License

MIT License.

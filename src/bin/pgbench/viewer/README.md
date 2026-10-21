# `pgbench` Telemetry & Reporting Viewer (`src/bin/pgbench/viewer`)

A zero-dependency web application (`pgbench_viewer.py`) built with [`bottle.py`](bottle.py), [`Water.css`](https://watercss.kognise.dev/) (`css/water.css`), modular `SimpleTemplate` (`stpl`) templates (`html/*.html.stpl`), and a 439-byte WebAssembly layout/projection kernel (`wasm/pgbench_chart.wasm`, with non-runtime build sources in `src/`). It renders historical benchmark runs, multi-run comparisons, live streaming telemetry, and interactive SQL explorations directly from a PostgreSQL `--log-db` telemetry database.

---

## 1. Architecture Overview

1. **Modular `html/`, `css/`, `wasm/` Runtime Subdirectories & `src/` Build Sources**:
   - Runtime assets are organized into `html/*.html.stpl` (Bottle `SimpleTemplate` layouts and partials), `css/water.css` + `css/viewer.css` (stylesheets), and `wasm/pgbench_chart.wasm` (the 439-byte compiled WebAssembly binary).
   - Non-runtime sources and build tooling (`src/pgbench_chart.wat`, `src/pgbench_chart.c`, and `src/build_wasm.py`) are kept cleanly separated in `src/`.
   - `PartsLoader` in `pgbench_viewer.py` reads templates, stylesheets, and the WASM binary on demand with `st_mtime` hot-reload caching (invalidating `SimpleTemplate` sub-template caches whenever any `.stpl` file under `html/` is modified).
2. **Single-JSON-from-DB Data Flow (Phase 1)**:
   - Every Phase 1 view (`single`, `compare` / `pairwise`, `multi` / `matrix`, and `live`) executes a single `SELECT <schema>.pgbench_viewer_payload_json(...)` query and receives one self-contained JSONB document containing the run selector catalog, selected run/comparison breakdowns (`pgbench_show_run()` and `pgbench_compare_runs()`), time-series points, and active live-stream telemetry.
   - `html/phase1_body.html.stpl` conditionally `% include`s only the view partials required for the active request (`run_selector.html.stpl`, `live_monitor.html.stpl`, `compare_pair.html.stpl`, `compare_matrix.html.stpl`, `single_run.html.stpl`, and `wasm_chart_box.html.stpl`).
3. **Real-Time Live View via Periodic `COPY` Batch Commits**:
   - When `pgbench` runs with `--log-db` in `stream` mode (`--log-db-mode=stream`), worker threads flush and `COMMIT` batches every **1–2 seconds** (or **64 KB**) on their `COPY ... FROM STDIN` connections, while the main thread inserts progress snapshots (`-P <sec>`) into `pgbench_progress_log`.
   - `/live` queries `<schema>.pgbench_live_progress_v` and committed stream batches in real time and polls `/api/payload?mode=live` to update KPI cards and WASM charts in place.
4. **Two-Stage Bottle Metatemplate SQL Data Explorer (Phase 2)**:
   - The `/explorer` tab wraps arbitrary read-only `SELECT` / `WITH` / `TABLE` / `VALUES` queries in a single PostgreSQL JSONB envelope (`{schema, generated_at, row_count, rows}`).
   - `generate_explorer_stpl_metatemplate()` inspects the returned column names and runtime JSON types (`run_link`, `badge`, `meter_pct`, `meter_ratio`, `metric_num`, `num`, `json`, `text`) and expands the Stage-1 metatemplate [`html/explorer_table_meta.html.stpl`](html/explorer_table_meta.html.stpl) (using custom delimiter syntax `<@ @> @ {| |}`) into a Stage-2 `bottle.SimpleTemplate` (`stpl`) string that is both inspectable in `<details>` and rendered to HTML.
5. **Minimalistic WebAssembly Diagram Kernel (`wasm/pgbench_chart.wasm`)**:
   - A 439-byte WebAssembly binary module assembled reproducibly via `python3 src/build_wasm.py` (with source in `src/pgbench_chart.wat` and `src/pgbench_chart.c`) exports `memory` (2 pages) and 4 `f64` array kernels:
     - `series_min(ptr, n) -> f64`
     - `series_max(ptr, n) -> f64`
     - `series_mean(ptr, n) -> f64`
     - `project_1d(in_ptr, n, min_v, max_v, out_start, out_span, out_ptr) -> void`
   - Expanded into pages via `html/wasm_chart_module.html.stpl` (with `/static/pgbench_chart.wasm` streaming fallback) and `html/wasm_chart_box.html.stpl`.

---

## 2. Directory Contents

| Path | Role | Description |
| :--- | :--- | :--- |
| `pgbench_viewer.py` | Runtime | Main `bottle.py` WSGI server, `PartsLoader`, `PgSingleJsonClient`, Stage-1 metatemplate driver (`generate_explorer_stpl_metatemplate`), and HTTP routes. |
| `bottle.py` | Runtime | Vendored single-file Python WSGI micro-framework (`bottle.py` v0.12.20). |
| `README.md` | Docs | This documentation file. |
| `html/layout.html.stpl` | Runtime | Master HTML5 page shell expanding `inline_css`, navigation bar, metadata bar, `% include(body_template)`, single-JSON `<script>`, and `% include('wasm_chart_module.html.stpl')`. |
| `html/phase1_body.html.stpl` | Runtime | Phase 1 dispatcher that extracts payload slices and conditionally `% include`s active Phase 1 view partials. |
| `html/run_selector.html.stpl` | Runtime | Section 1: Benchmark Run Selector toolbar and collapsible `pgbench_runs_v` catalog table. |
| `html/live_monitor.html.stpl` | Runtime | Section 2: Live Running Benchmark Monitor panel and `/api/payload?mode=live` auto-refresh poller. |
| `html/compare_pair.html.stpl` | Runtime | Section 3: Pairwise Run Comparison table (`pgbench_compare_runs(base_id, cand_id)`). |
| `html/compare_matrix.html.stpl` | Runtime | Section 4: Multi-Run Matrix Comparison table & WASM bar chart (`pgbench_compare_runs(bigint[])`). |
| `html/single_run.html.stpl` | Runtime | Section 5: Selected Run KPI cards, 3-chart WASM grid, `pgbench_show_run()` breakdown, and `pgbench_run_commands` profile. |
| `html/explorer_body.html.stpl` | Runtime | Phase 2: SQL Data Explorer form, presets bar, WASM chart partial, and Stage-2 `stpl` source inspector. |
| `html/explorer_table_meta.html.stpl` | Runtime | Phase 2: Stage-1 Bottle metatemplate (`<@ @> @ {| |}`) that synthesizes the Stage-2 `SimpleTemplate` table template per column descriptor. |
| `html/wasm_chart_box.html.stpl` | Runtime | Reusable WASM SVG chart container partial (`% include` target across Single-Run, Matrix, and Explorer views). |
| `html/wasm_chart_module.html.stpl` | Runtime | WASM module loader (`PGBENCH_WASM_B64` inline instantiation + `/static/pgbench_chart.wasm` streaming fallback) and SVG line/bar chart projectors. |
| `css/water.css` | Runtime | Classless semantic `Water.css` v2 core stylesheet (light/dark adaptive variables & element rules). |
| `css/viewer.css` | Runtime | `pgbench` viewer custom variables (`--bg-card`, `--border-subtle`, `--accent-*`), grid/card/panel rules, badges, meters, and `svg.wasm-svg` styles. |
| `wasm/pgbench_chart.wasm` | Runtime | 439-byte compiled WebAssembly binary (`sha256: 9fb72bbd451e5ea1...`). |
| `src/pgbench_chart.wat` | Build / Source | WebAssembly Text Format (WAT) source for `memory`, `series_min`, `series_max`, `series_mean`, and `project_1d`. |
| `src/pgbench_chart.c` | Build / Source | Reference C99 source (`clang --target=wasm32 -O3 -nostdlib`) for the 4 `f64` kernels. |
| `src/build_wasm.py` | Build / Source | Zero-dependency Python assembler (`--verify`, `--print-b64`) that reproducibly builds `wasm/pgbench_chart.wasm`. |

---

## 3. Quick Start

### Step 1: Record Benchmark Runs into PostgreSQL (`--log-db`)

```bash
# 1. Baseline run with 1-second progress & aggregate intervals
pgbench -i -s 10 "host=localhost port=5432 dbname=benchdb"

pgbench -c 8 -j 4 -T 30 -P 1 --aggregate-interval=1 -l \
  --log-db="host=localhost port=5432 dbname=telemetry" \
  --log-db-unlogged=stream \
  --log-db-index \
  --run-label="pg20-baseline" \
  "host=localhost port=5432 dbname=benchdb"

# 2. Candidate run for comparison
pgbench -c 8 -j 4 -T 30 -P 1 --aggregate-interval=1 -l \
  --log-db="host=localhost port=5432 dbname=telemetry" \
  --log-db-unlogged=stream \
  --log-db-index \
  --run-label="pg20-candidate" \
  "host=localhost port=5432 dbname=benchdb"
```

### Step 2: Launch the Telemetry Viewer

```bash
python3 src/bin/pgbench/viewer/pgbench_viewer.py \
  --log-db="host=localhost port=5432 dbname=telemetry" \
  --schema=public \
  --host=0.0.0.0 \
  --port=8094
```

Then open `http://localhost:8094` in your browser. To rebuild or verify `wasm/pgbench_chart.wasm` from source at any time:

```bash
python3 src/bin/pgbench/viewer/src/build_wasm.py --verify
```

---

## 4. Command-Line Options

| Option | Default | Description |
| :--- | :--- | :--- |
| `--log-db CONNINFO`, `--conninfo CONNINFO` | `$PGBENCH_LOG_DB` or `host=localhost port=54320 dbname=pgbench_logdb_test` | PostgreSQL connection string (`libpq` keyword/value format or `postgresql://` URI) pointing to the `--log-db` telemetry database. |
| `--schema SCHEMA` | `$PGBENCH_LOG_DB_SCHEMA` or `public` | Target PostgreSQL schema containing `pgbench_runs` and the `pgbench_*` views/SRFs. |
| `--host HOST` | `0.0.0.0` | HTTP bind address for the `bottle.py` WSGI server. |
| `--port PORT` | `8094` | HTTP listen port. |
| `--psql-bin PATH` | `psql` | Fallback `psql` binary path used when `psycopg` is not installed. |

---

## 5. Views & Endpoints

### Phase 1: Benchmark Views & Live Monitor

| Route | Mode / Parameters | Underlying SQL Interface | Description |
| :--- | :--- | :--- | :--- |
| `GET /` (`?mode=single&run_id=<id>`) | `mode=single` | `<schema>.pgbench_viewer_payload_json('single', ...)` | **Single-Run Breakdown**: KPI summary cards, WASM SVG TPS/latency/HDR charts, `pgbench_show_run(run_id)`, and `pgbench_run_commands`. |
| `GET /?mode=compare&base_id=<b_id>&cand_id=<c_id>` | `mode=compare` (`pairwise` alias) | `<schema>.pgbench_viewer_payload_json('compare', ...)` | **Pairwise Run Comparison**: Baseline vs. candidate diff (`pgbench_compare_runs(base_id, cand_id)`) with dual-run WASM TPS overlay and `IMPROVED`/`REGRESSED` badges. |
| `GET /?mode=multi&run_ids=1,2,3` | `mode=multi` (`matrix` alias) | `<schema>.pgbench_viewer_payload_json('multi', ...)` | **Multi-Run Matrix**: N-way comparison (`pgbench_compare_runs(bigint[])`) with WASM bar chart and relative deltas vs. base run. |
| `GET /live` | `mode=live` | `<schema>.pgbench_viewer_payload_json('live', ...)` | **Live Benchmark View**: Real-time dashboard of `status = 'running'` benchmarks with configurable 1s/2s/5s `/api/payload?mode=live` polling. |
| `GET /api/payload` | `?mode=...&run_id=...&base_id=...&cand_id=...&run_ids=...` | `<schema>.pgbench_viewer_payload_json(...)` | Returns the raw single-JSON payload directly from PostgreSQL (`application/json`). |

### Phase 2: SQL Data Explorer & Static Assets

| Route | Description |
| :--- | :--- |
| `GET /explorer`, `POST /explorer` | Interactive SQL Data Explorer with 6 curated SQL presets, custom read-only `SELECT`/`WITH` execution, 2-stage `explorer_table_meta.html.stpl` $\rightarrow$ `bottle.SimpleTemplate` synthesis, inspectable Stage-2 `stpl` source, and automatic WASM bar chart plotting. |
| `GET /api/explorer?sql=...` | Executes the read-only SQL query and returns the JSON result envelope (`{schema, generated_at, row_count, rows, columns, generated_stpl}`). |
| `GET /static/water.css` | Serves `css/water.css` (`text/css`). |
| `GET /static/viewer.css` | Serves `css/viewer.css` (`text/css`). |
| `GET /static/pgbench_chart.wasm` | Serves the 439-byte `wasm/pgbench_chart.wasm` module (`application/wasm`). |

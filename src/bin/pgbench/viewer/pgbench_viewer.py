#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pgbench_viewer.py — Lightweight Benchmark Telemetry & Live Viewer for pgbench (--log-db)

Architecture:
1. Modular Directory Layout (`html/`, `css/`, `wasm/`, and build-only `src/`):
   - `html/*.html.stpl`: Bottle `SimpleTemplate` layouts, Phase 1/Phase 2 views,
     Stage-1 metatemplate (`explorer_table_meta.html.stpl`), and WASM chart partials.
   - `css/{water.css,viewer.css}`: Classless semantic `Water.css` v2 and viewer layout styles.
   - `wasm/pgbench_chart.wasm`: Runtime 439-byte WebAssembly `f64` statistical & 1D SVG
     coordinate projection kernel.
   - `src/{pgbench_chart.wat,pgbench_chart.c,build_wasm.py}`: Non-runtime WebAssembly
     sources and zero-dependency assembler.
   - `PartsLoader` reads HTML templates, CSS stylesheets, and the WASM binary on demand
     with `st_mtime`-aware caching and automatic WASM assembly fallback.
2. Single-JSON-from-DB per request:
   - Phase 1 routes (`/`, `/live`, `/run/<id>`, `/api/payload`) execute a SINGLE PostgreSQL query:
       SELECT <schema>.pgbench_viewer_payload_json(mode, run_id, base_id, cand_id, run_ids, limit)
   - Phase 2 routes (`/explorer`, `/api/explorer`) execute a SINGLE PostgreSQL query wrapping
     the user's read-only SQL into a single JSONB envelope `{schema, generated_at, row_count, rows}`,
     then render `html/explorer_table_meta.html.stpl` (Stage-1 metatemplate) into a tailored
     Stage-2 `bottle.SimpleTemplate` (`stpl`) for the inferred column types.
"""

import argparse
import base64
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

# Ensure local viewer directory is on sys.path so bundled bottle.py is found
VIEWER_DIR = os.path.dirname(os.path.abspath(__file__))
if VIEWER_DIR not in sys.path:
    sys.path.insert(0, VIEWER_DIR)

import bottle
from bottle import Bottle, SimpleTemplate, request, response

# ---------------------------------------------------------------------------
# Curated SQL Presets for Phase 2 Data Explorer
# ---------------------------------------------------------------------------
EXPLORER_PRESETS = [
    {
        "id": "runs_overview",
        "title": "1. Runs Overview (pgbench_runs_v)",
        "sql": (
            "SELECT run_id, run_label, status, clients, threads, duration_sec,\n"
            "       tps, latency_avg_ms, latency_p50_ms, latency_p95_ms, latency_p99_ms,\n"
            "       recall_avg, validation_pct, sla_all_passed\n"
            "FROM pgbench_runs_v\n"
            "ORDER BY run_id DESC\n"
            "LIMIT 50"
        ),
    },
    {
        "id": "latest_run_show",
        "title": "2. Latest Run Full Breakdown (pgbench_show_run())",
        "sql": "SELECT section, scope, metric, value, unit, details FROM pgbench_show_run()",
    },
    {
        "id": "slowest_commands",
        "title": "3. Top Slowest SQL/Meta Commands Across Runs",
        "sql": (
            "SELECT c.run_id, r.run_label, c.script_idx, c.cmd_idx, c.cmd_type,\n"
            "       round(c.latency_avg_ms::numeric, 3) AS latency_avg_ms,\n"
            "       round(c.latency_max_ms::numeric, 3) AS latency_max_ms,\n"
            "       c.cnt, c.failed, c.retries, c.first_line\n"
            "FROM pgbench_run_commands c\n"
            "JOIN pgbench_runs r ON r.run_id = c.run_id\n"
            "ORDER BY c.latency_avg_ms DESC NULLS LAST\n"
            "LIMIT 40"
        ),
    },
    {
        "id": "tx_per_second_buckets",
        "title": "4. Per-Second Throughput & Tail Latency (pgbench_tx_log)",
        "sql": (
            "SELECT run_id,\n"
            "       (ts_usec / 1000000)::bigint - min((ts_usec / 1000000)::bigint) OVER (PARTITION BY run_id) AS elapsed_sec,\n"
            "       count(*) FILTER (WHERE status = 'ok') AS tps,\n"
            "       round((avg(latency_us) FILTER (WHERE status = 'ok') / 1000.0)::numeric, 3) AS latency_avg_ms,\n"
            "       round((percentile_cont(0.95) WITHIN GROUP (ORDER BY latency_us) FILTER (WHERE status = 'ok') / 1000.0)::numeric, 3) AS latency_p95_ms,\n"
            "       round((percentile_cont(0.99) WITHIN GROUP (ORDER BY latency_us) FILTER (WHERE status = 'ok') / 1000.0)::numeric, 3) AS latency_p99_ms,\n"
            "       round((avg(recall) FILTER (WHERE recall IS NOT NULL))::numeric, 4) AS recall_avg\n"
            "FROM pgbench_tx_log\n"
            "WHERE run_id = (SELECT max(run_id) FROM pgbench_tx_log)\n"
            "GROUP BY run_id, (ts_usec / 1000000)::bigint\n"
            "ORDER BY elapsed_sec ASC"
        ),
    },
    {
        "id": "recall_aging",
        "title": "5. Vector Recall@K & Index Aging Degradation",
        "sql": (
            "SELECT rc.run_id, r.run_label, rc.scope, rc.metric_name, rc.samples,\n"
            "       round(rc.avg_recall::numeric, 4) AS avg_recall,\n"
            "       round(rc.p5_recall::numeric, 4) AS p5_recall,\n"
            "       round(rc.p50_recall::numeric, 4) AS p50_recall,\n"
            "       round(rc.initial_q1::numeric, 4) AS initial_q1,\n"
            "       round(rc.mature_q4::numeric, 4) AS mature_q4,\n"
            "       round(rc.degradation_pct::numeric, 2) AS degradation_pct\n"
            "FROM pgbench_run_recalls rc\n"
            "JOIN pgbench_runs r ON r.run_id = rc.run_id\n"
            "ORDER BY rc.run_id DESC, rc.scope, rc.metric_name"
        ),
    },
    {
        "id": "live_progress_stream",
        "title": "6. Live Progress Stream (pgbench_live_progress_v)",
        "sql": (
            "SELECT run_id, run_label, status, elapsed_sec, tps,\n"
            "       latency_avg_ms, latency_stddev_ms, lag_avg_ms, failed_cnt\n"
            "FROM pgbench_live_progress_v\n"
            "ORDER BY run_id DESC, elapsed_sec ASC\n"
            "LIMIT 100"
        ),
    },
]


# ---------------------------------------------------------------------------
# On-Demand Runtime Asset Loader (`html/`, `css/`, `wasm/`)
# ---------------------------------------------------------------------------
class PartsLoader:
    """
    Reads runtime HTML templates (`html/*.html.stpl`), CSS stylesheets (`css/*.css`),
    and WebAssembly binaries (`wasm/pgbench_chart.wasm`) on demand with `st_mtime`
    hot-reload caching. Non-runtime WASM sources and assembler reside in `src/`.
    """

    def __init__(self, viewer_dir: str = VIEWER_DIR):
        self.viewer_dir = os.path.abspath(viewer_dir)
        self.parts_dir = self.viewer_dir
        self.html_dir = os.path.join(self.viewer_dir, "html")
        self.css_dir = os.path.join(self.viewer_dir, "css")
        self.wasm_dir = os.path.join(self.viewer_dir, "wasm")
        self.src_dir = os.path.join(self.viewer_dir, "src")
        self.lookup = [self.html_dir, self.css_dir, self.wasm_dir]

        for p in reversed(self.lookup):
            if p not in bottle.TEMPLATE_PATH:
                bottle.TEMPLATE_PATH.insert(0, p)

        self._file_cache = {}
        self._tpl_cache = {}
        self._html_dir_sig = None
        self._wasm_cache = None

    def _stat_sig(self, path: str):
        st = os.stat(path)
        return (st.st_mtime_ns, st.st_size)

    def _compute_html_dir_sig(self):
        entries = []
        if os.path.isdir(self.html_dir):
            for fn in sorted(os.listdir(self.html_dir)):
                if fn.endswith(".stpl") or fn.endswith(".html"):
                    fp = os.path.join(self.html_dir, fn)
                    if os.path.isfile(fp):
                        entries.append((fn, self._stat_sig(fp)))
        return tuple(entries)

    def read_text(self, subdir: str, filename: str) -> str:
        path = os.path.join(self.viewer_dir, subdir, filename)
        sig = self._stat_sig(path)
        cached = self._file_cache.get(path)
        if cached and cached[0] == sig:
            return cached[1]
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        self._file_cache[path] = (sig, text)
        return text

    def get_css(self, filename: str) -> str:
        return self.read_text("css", filename)

    def get_combined_css(self) -> str:
        return self.get_css("water.css") + "\n" + self.get_css("viewer.css")

    def get_wasm_bundle(self) -> dict:
        wasm_path = os.path.join(self.wasm_dir, "pgbench_chart.wasm")
        if not os.path.exists(wasm_path):
            builder_path = os.path.join(self.src_dir, "build_wasm.py")
            if not os.path.exists(builder_path):
                raise FileNotFoundError(
                    f"Missing runtime WASM binary {wasm_path} (and fallback builder {builder_path})"
                )
            spec = importlib.util.spec_from_file_location("build_wasm", builder_path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            wasm_bytes = mod.build_wasm_bytes()
            try:
                os.makedirs(self.wasm_dir, exist_ok=True)
                with open(wasm_path, "wb") as f:
                    f.write(wasm_bytes)
            except OSError:
                bundle = {
                    "wasm_bytes": wasm_bytes,
                    "wasm_b64": base64.b64encode(wasm_bytes).decode("ascii"),
                    "wasm_size_bytes": len(wasm_bytes),
                }
                self._wasm_cache = (("mem", len(wasm_bytes)), bundle)
                return bundle

        sig = self._stat_sig(wasm_path)
        if self._wasm_cache and self._wasm_cache[0] == sig:
            return self._wasm_cache[1]

        with open(wasm_path, "rb") as f:
            wasm_bytes = f.read()
        bundle = {
            "wasm_bytes": wasm_bytes,
            "wasm_b64": base64.b64encode(wasm_bytes).decode("ascii"),
            "wasm_size_bytes": len(wasm_bytes),
        }
        self._wasm_cache = (sig, bundle)
        return bundle

    def get_template(self, template_name: str, syntax=None) -> SimpleTemplate:
        # Invalidate cached SimpleTemplate instances and their internal `_include` sub-caches
        # whenever any file in `html/` changes on disk.
        dir_sig = self._compute_html_dir_sig()
        if dir_sig != self._html_dir_sig:
            self._tpl_cache.clear()
            self._html_dir_sig = dir_sig

        key = (template_name, syntax)
        if key not in self._tpl_cache:
            source = self.read_text("html", template_name)
            self._tpl_cache[key] = SimpleTemplate(
                source=source,
                lookup=self.lookup,
                syntax=syntax,
            )
        return self._tpl_cache[key]

    def render_page(self, body_template: str, **ctx) -> str:
        wasm_bundle = self.get_wasm_bundle()
        merged = {
            "body_template": body_template,
            "inline_css": self.get_combined_css(),
            "wasm_b64": wasm_bundle["wasm_b64"],
            "wasm_size_bytes": wasm_bundle["wasm_size_bytes"],
        }
        merged.update(ctx)
        if "payload_json_str" in merged and isinstance(merged["payload_json_str"], str):
            merged["payload_json_str"] = merged["payload_json_str"].replace("</", "<\\/")
        return self.get_template("layout.html.stpl").render(**merged)


# ---------------------------------------------------------------------------
# Database Single-JSON Query Executor
# ---------------------------------------------------------------------------
class PgSingleJsonClient:
    """Executes a single SQL query against PostgreSQL that returns one JSON document."""

    def __init__(self, conninfo: str, schema: str = "public", psql_bin: str = "psql"):
        self.conninfo = conninfo
        self.schema = schema or "public"
        self.psql_bin = psql_bin
        self._psycopg = None
        try:
            import psycopg  # type: ignore
            self._psycopg = psycopg
        except ImportError:
            self._psycopg = None

    def _quote_ident(self, ident: str) -> str:
        return '"' + ident.replace('"', '""') + '"'

    def fetch_single_json(
        self, sql_returning_single_json: str, params=None, read_only: bool = False
    ) -> dict:
        t0 = time.monotonic()
        q_schema = self._quote_ident(self.schema)
        ro_sql = (
            "SET default_transaction_read_only = on;\nSET TRANSACTION READ ONLY;\n"
            if read_only
            else ""
        )

        if self._psycopg is not None:
            with self._psycopg.connect(self.conninfo, autocommit=True) as conn:
                with conn.cursor() as cur:
                    cur.execute(f"SET search_path TO {q_schema}, pg_catalog;")
                    if read_only:
                        cur.execute("SET default_transaction_read_only = on;")
                    cur.execute(sql_returning_single_json, params)
                    row = cur.fetchone()
                    if not row or row[0] is None:
                        data = {}
                    elif isinstance(row[0], (dict, list)):
                        data = {"data": row[0]} if isinstance(row[0], list) else row[0]
                    else:
                        data = json.loads(str(row[0]))
        else:
            rendered_sql = self._interpolate_params_for_psql(sql_returning_single_json, params)
            full_sql = f"SET search_path TO {q_schema}, pg_catalog;\n{ro_sql}{rendered_sql};"
            cmd = [self.psql_bin, "-X", "-q", "-t", "-A", "-d", self.conninfo, "-c", full_sql]
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
            if proc.returncode != 0:
                raise RuntimeError(proc.stderr.strip() or f"psql exited with code {proc.returncode}")
            out = proc.stdout.strip()
            lines = [ln for ln in out.splitlines() if ln.strip()]
            raw_json = lines[-1] if lines else "{}"
            data = json.loads(raw_json)

        elapsed_ms = round((time.monotonic() - t0) * 1000.0, 2)
        if isinstance(data, dict):
            data["_query_elapsed_ms"] = elapsed_ms
        return data

    def _interpolate_params_for_psql(self, sql: str, params) -> str:
        if not params:
            return sql
        out = sql
        for val in params:
            if val is None:
                rep = "NULL"
            elif isinstance(val, bool):
                rep = "TRUE" if val else "FALSE"
            elif isinstance(val, (int, float)):
                rep = str(val)
            elif isinstance(val, list):
                items = ", ".join(str(int(x)) for x in val)
                rep = f"ARRAY[{items}]::bigint[]"
            else:
                escaped = str(val).replace("'", "''")
                rep = f"'{escaped}'"
            out = out.replace("%s", rep, 1)
        return out

    def fetch_viewer_payload(
        self,
        mode: str = "overview",
        run_id=None,
        base_id=None,
        cand_id=None,
        run_ids=None,
        limit: int = 50,
    ) -> dict:
        sql = (
            "SELECT pgbench_viewer_payload_json(\n"
            "    %s::text,\n"
            "    %s::bigint,\n"
            "    %s::bigint,\n"
            "    %s::bigint,\n"
            "    %s::bigint[],\n"
            "    %s::int\n"
            ")::text"
        )
        return self.fetch_single_json(sql, [mode, run_id, base_id, cand_id, run_ids, limit])

    def fetch_explorer_payload(self, user_sql: str, max_rows: int = 500) -> dict:
        cleaned = user_sql.strip().rstrip(";").strip()
        if not cleaned:
            raise ValueError("SQL query must not be empty.")
        if ";" in cleaned:
            raise ValueError("Multiple SQL statements (';') are not permitted in Data Explorer.")
        first_tok = re.split(r"\s+", cleaned, maxsplit=1)[0].upper()
        if first_tok not in ("SELECT", "WITH", "TABLE", "VALUES"):
            raise ValueError(
                f"Only read-only SELECT / WITH queries are permitted in Data Explorer (got {first_tok})."
            )

        envelope_sql = f"""
WITH _user_q AS (
    {cleaned}
),
_limited_q AS (
    SELECT * FROM _user_q LIMIT {int(max_rows)}
),
_rows_json AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(q)), '[]'::jsonb) AS rows,
           count(*)::int AS row_count
    FROM _limited_q q
)
SELECT jsonb_build_object(
    'schema', current_schema(),
    'generated_at', clock_timestamp(),
    'row_count', (SELECT row_count FROM _rows_json),
    'rows', (SELECT rows FROM _rows_json)
)::text
"""
        return self.fetch_single_json(envelope_sql, read_only=True)


# ---------------------------------------------------------------------------
# Phase 2: Python Metatemplate Generator (via html/explorer_table_meta.html.stpl)
# ---------------------------------------------------------------------------
def infer_column_Descriptors(rows: list) -> list:
    if not rows:
        return []
    col_names = list(rows[0].keys())
    descriptors = []
    for col in col_names:
        cl = col.lower()
        sample_vals = [r.get(col) for r in rows[:25] if r.get(col) is not None]
        py_type = "text"
        if sample_vals:
            v0 = sample_vals[0]
            if isinstance(v0, bool):
                py_type = "bool"
            elif isinstance(v0, int):
                py_type = "int"
            elif isinstance(v0, float):
                py_type = "float"
            elif isinstance(v0, (dict, list)):
                py_type = "json"
            elif isinstance(v0, str):
                try:
                    float(v0)
                    py_type = "float" if ("." in v0) else "int"
                except ValueError:
                    py_type = "text"

        if cl == "run_id":
            kind = "run_link"
        elif cl in ("status", "verdict", "sla_all_passed", "passed", "validation_ok"):
            kind = "badge"
        elif ("pct" in cl or "recall" in cl) and py_type in ("float", "int"):
            kind = "meter_pct" if "pct" in cl else "meter_ratio"
        elif py_type in ("float", "int") and any(
            k in cl for k in ("tps", "latency", "lag", "_ms", "_us", "_sec", "delta", "val", "cnt", "samples")
        ):
            kind = "metric_num"
        elif py_type in ("float", "int"):
            kind = "num"
        elif py_type == "json":
            kind = "json"
        else:
            kind = "text"

        descriptors.append(
            {
                "name": col,
                "py_type": py_type,
                "kind": kind,
                "is_numeric": py_type in ("int", "float"),
            }
        )
    return descriptors


def generate_explorer_stpl_metatemplate(
    col_descriptors: list, parts_loader: PartsLoader = None
) -> str:
    """
    Stage-1 Python Metatemplate Generator:
    Reads `html/explorer_table_meta.html.stpl` (using Stage-1 syntax `<@ @> @ {| |}`)
    and expands `col_descriptors` into a tailored Stage-2 `bottle.SimpleTemplate` (`stpl`)
    source string for rendering the SQL query result rows.
    """
    if not col_descriptors:
        return "<p><em>Query returned 0 rows.</em></p>"
    loader = parts_loader or PartsLoader()
    meta_tpl = loader.get_template("explorer_table_meta.html.stpl", syntax="<@ @> @ {| |}")
    return meta_tpl.render(col_descriptors=col_descriptors).strip()


# ---------------------------------------------------------------------------
# Bottle Web Application Factory
# ---------------------------------------------------------------------------
def create_viewer_app(db_client: PgSingleJsonClient, parts_loader: PartsLoader = None) -> Bottle:
    app = Bottle()
    loader = parts_loader or PartsLoader()

    def _parse_int(val):
        if val is None or str(val).strip() == "":
            return None
        try:
            return int(val)
        except ValueError:
            return None

    def _parse_int_list(val):
        if not val:
            return None
        out = []
        for part in str(val).split(","):
            part = part.strip()
            if part:
                try:
                    out.append(int(part))
                except ValueError:
                    pass
        return out if out else None

    def _normalize_mode(raw_mode: str) -> str:
        m = (raw_mode or "single").strip().lower()
        if m == "pairwise":
            return "compare"
        if m == "matrix":
            return "multi"
        return m

    @app.route("/static/water.css")
    def serve_water_css():
        response.content_type = "text/css; charset=utf-8"
        return loader.get_css("water.css")

    @app.route("/static/viewer.css")
    def serve_viewer_css():
        response.content_type = "text/css; charset=utf-8"
        return loader.get_css("viewer.css")

    @app.route("/static/pgbench_chart.wasm")
    def serve_wasm():
        response.content_type = "application/wasm"
        return loader.get_wasm_bundle()["wasm_bytes"]

    @app.route("/api/payload")
    def api_payload():
        mode = _normalize_mode(request.query.get("mode"))
        run_id = _parse_int(request.query.get("run_id"))
        base_id = _parse_int(request.query.get("base_id"))
        cand_id = _parse_int(request.query.get("cand_id"))
        run_ids = _parse_int_list(request.query.get("run_ids"))
        limit = _parse_int(request.query.get("limit")) or 50

        payload = db_client.fetch_viewer_payload(
            mode=mode,
            run_id=run_id,
            base_id=base_id,
            cand_id=cand_id,
            run_ids=run_ids,
            limit=limit,
        )
        response.content_type = "application/json; charset=utf-8"
        return json.dumps(payload, default=str)

    @app.route("/api/explorer")
    def api_explorer():
        preset_id = request.query.get("preset") or ""
        user_sql = request.query.get("sql") or ""
        if not user_sql:
            chosen = next((p for p in EXPLORER_PRESETS if p["id"] == preset_id), EXPLORER_PRESETS[0])
            user_sql = chosen["sql"]
        response.content_type = "application/json; charset=utf-8"
        try:
            payload = db_client.fetch_explorer_payload(user_sql)
            rows = payload.get("rows") or []
            col_descriptors = infer_column_Descriptors(rows)
            payload["columns"] = [cd["name"] for cd in col_descriptors]
            payload["generated_stpl"] = generate_explorer_stpl_metatemplate(col_descriptors, loader)
            return json.dumps(payload, default=str)
        except Exception as exc:
            response.status = 400
            return json.dumps(
                {"error": str(exc), "schema": db_client.schema, "row_count": 0, "rows": []},
                default=str,
            )

    @app.route("/")
    @app.route("/live")
    @app.route("/run/<path_run_id:int>")
    def index_view(path_run_id=None):
        is_live_route = request.path == "/live"
        mode = "live" if is_live_route else _normalize_mode(request.query.get("mode"))
        run_id = path_run_id or _parse_int(request.query.get("run_id"))
        base_id = _parse_int(request.query.get("base_id"))
        cand_id = _parse_int(request.query.get("cand_id"))
        run_ids_str = request.query.get("run_ids") or ""
        run_ids = _parse_int_list(run_ids_str)

        payload = db_client.fetch_viewer_payload(
            mode=mode,
            run_id=run_id,
            base_id=base_id,
            cand_id=cand_id,
            run_ids=run_ids,
            limit=50,
        )
        runs = payload.get("runs") or []
        if mode == "compare" and (base_id is None or cand_id is None) and len(runs) >= 2:
            base_id = runs[1]["run_id"]
            cand_id = runs[0]["run_id"]
            payload = db_client.fetch_viewer_payload(
                mode=mode,
                run_id=run_id,
                base_id=base_id,
                cand_id=cand_id,
                run_ids=run_ids,
                limit=50,
            )
        elif mode == "multi" and not run_ids and len(runs) >= 2:
            run_ids = [r["run_id"] for r in reversed(runs[:4])]
            run_ids_str = ",".join(str(x) for x in run_ids)
            payload = db_client.fetch_viewer_payload(
                mode=mode,
                run_id=run_id,
                base_id=base_id,
                cand_id=cand_id,
                run_ids=run_ids,
                limit=50,
            )

        nav_tab = "live" if mode == "live" else ("compare" if mode in ("compare", "multi") else "overview")
        return loader.render_page(
            "phase1_body.html.stpl",
            page_title="Live Benchmark View" if mode == "live" else "Benchmark Runs & Comparison",
            nav_tab=nav_tab,
            mode=mode,
            target_run_id=payload.get("target_run_id"),
            base_id=base_id,
            cand_id=cand_id,
            run_ids_str=run_ids_str,
            active_runs=payload.get("active_runs") or [],
            payload=payload,
            payload_json_str=json.dumps(payload, default=str),
        )

    @app.route("/explorer", method=["GET", "POST"])
    def explorer_view():
        preset_id = request.params.get("preset") or ""
        user_sql = request.params.get("sql") or ""
        if not user_sql:
            chosen = next((p for p in EXPLORER_PRESETS if p["id"] == preset_id), EXPLORER_PRESETS[0])
            preset_id = chosen["id"]
            user_sql = chosen["sql"]

        error_msg = None
        payload = {"schema": db_client.schema, "row_count": 0, "rows": []}
        generated_stpl = ""
        rendered_metatemplate_html = ""

        try:
            payload = db_client.fetch_explorer_payload(user_sql)
            rows = payload.get("rows") or []
            col_descriptors = infer_column_Descriptors(rows)
            generated_stpl = generate_explorer_stpl_metatemplate(col_descriptors, loader)
            rendered_metatemplate_html = SimpleTemplate(generated_stpl, lookup=loader.lookup).render(
                rows=rows, json=json
            )
        except Exception as exc:
            error_msg = str(exc)

        return loader.render_page(
            "explorer_body.html.stpl",
            page_title="SQL Data Explorer (Metatemplates)",
            nav_tab="explorer",
            mode="explorer",
            target_run_id=None,
            active_runs=[],
            presets=EXPLORER_PRESETS,
            preset_id=preset_id,
            user_sql=user_sql,
            error_msg=error_msg,
            payload=payload,
            payload_json_str=json.dumps(payload, default=str),
            generated_stpl=generated_stpl,
            rendered_metatemplate_html=rendered_metatemplate_html,
        )

    return app


def main():
    from socketserver import ThreadingMixIn
    from wsgiref.simple_server import WSGIRequestHandler, WSGIServer, make_server

    class ThreadingWSGIServer(ThreadingMixIn, WSGIServer):
        daemon_threads = True

    parser = argparse.ArgumentParser(
        description="pgbench Telemetry & Live Benchmark Viewer (bottle.py + Water.css + WASM)"
    )
    parser.add_argument(
        "--log-db",
        "--conninfo",
        dest="log_db",
        default=os.environ.get("PGBENCH_LOG_DB", "host=localhost port=54320 dbname=pgbench_logdb_test"),
        help="PostgreSQL connection string for the pgbench telemetry database",
    )
    parser.add_argument(
        "--schema",
        default=os.environ.get("PGBENCH_LOG_DB_SCHEMA", "public"),
        help="Database schema containing pgbench_runs and telemetry tables (default: public)",
    )
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8094, help="HTTP listen port (default: 8094)")
    parser.add_argument("--psql-bin", default="psql", help="Fallback psql binary path")
    args = parser.parse_args()

    loader = PartsLoader()
    loader.get_wasm_bundle()
    client = PgSingleJsonClient(args.log_db, schema=args.schema, psql_bin=args.psql_bin)
    app = create_viewer_app(client, parts_loader=loader)
    print(
        f"Starting pgbench Telemetry Viewer on http://{args.host}:{args.port} "
        f"(db={args.log_db!r}, schema={args.schema!r})",
        flush=True,
    )
    with make_server(args.host, args.port, app, server_class=ThreadingWSGIServer, handler_class=WSGIRequestHandler) as httpd:
        httpd.serve_forever()


if __name__ == "__main__":
    main()

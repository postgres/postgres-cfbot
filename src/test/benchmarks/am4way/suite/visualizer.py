"""
Visualization: generates matplotlib charts and an HTML dashboard
from benchmark analysis results.
"""

import html
import json
import logging
import os
from typing import Any, Dict, List, Optional

from .result_analyzer import AnalysisReport, ComparisonResult, StorageComparison

logger = logging.getLogger(__name__)

# Try importing matplotlib; gracefully degrade if missing
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    logger.info("matplotlib not available; chart generation will be skipped")


def _human_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024  # type: ignore
    return f"{n:.1f} PB"


class Visualizer:
    """Generates charts and HTML dashboard from benchmark results."""

    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)

    # ------------------------------------------------------------------
    # Chart generation (requires matplotlib)
    # ------------------------------------------------------------------

    def _save_fig(self, fig, name: str) -> str:
        path = os.path.join(self.output_dir, name)
        fig.savefig(path, dpi=120, bbox_inches="tight")
        plt.close(fig)
        logger.info("Saved chart: %s", path)
        return name

    def generate_speedup_chart(
        self, comparisons: List[ComparisonResult]
    ) -> Optional[str]:
        """Bar chart of speedup ratios by query pattern."""
        if not HAS_MATPLOTLIB or not comparisons:
            return None

        patterns = sorted(set(c.query_pattern for c in comparisons))
        # Average speedup per pattern across all schemas/distributions
        avg_speedups = []
        for p in patterns:
            vals = [c.speedup for c in comparisons if c.query_pattern == p and c.speedup != float("inf")]
            avg_speedups.append(sum(vals) / len(vals) if vals else 1.0)

        fig, ax = plt.subplots(figsize=(10, 6))
        colors = ["#2ecc71" if s > 1.0 else "#e74c3c" for s in avg_speedups]
        bars = ax.barh(patterns, avg_speedups, color=colors)
        ax.axvline(x=1.0, color="black", linestyle="--", linewidth=0.8, label="HEAP baseline")
        ax.set_xlabel("Speedup (Noxu / HEAP)")
        ax.set_title("Query Performance: Noxu vs HEAP")

        for bar, val in zip(bars, avg_speedups):
            ax.text(
                bar.get_width() + 0.05,
                bar.get_y() + bar.get_height() / 2,
                f"{val:.2f}x",
                va="center",
                fontsize=9,
            )

        ax.legend()
        fig.tight_layout()
        return self._save_fig(fig, "speedup_by_pattern.png")

    def generate_storage_chart(
        self, storage_comps: List[StorageComparison]
    ) -> Optional[str]:
        """Grouped bar chart comparing HEAP and Noxu storage sizes."""
        if not HAS_MATPLOTLIB or not storage_comps:
            return None

        labels = [
            f"{sc.schema_name}\n{sc.row_count:,} rows\n{sc.distribution}"
            for sc in storage_comps
        ]
        heap_sizes = [sc.heap_total_bytes / (1024 * 1024) for sc in storage_comps]
        noxu_sizes = [sc.noxu_total_bytes / (1024 * 1024) for sc in storage_comps]

        fig, ax = plt.subplots(figsize=(max(8, len(labels) * 2), 6))
        x = range(len(labels))
        width = 0.35
        ax.bar([i - width / 2 for i in x], heap_sizes, width, label="HEAP", color="#3498db")
        ax.bar([i + width / 2 for i in x], noxu_sizes, width, label="Noxu", color="#2ecc71")

        ax.set_ylabel("Total Size (MB)")
        ax.set_title("Storage Comparison: HEAP vs Noxu")
        ax.set_xticks(list(x))
        ax.set_xticklabels(labels, fontsize=8)
        ax.legend()

        # Annotate compression ratio
        for i, sc in enumerate(storage_comps):
            ax.text(
                i, max(heap_sizes[i], noxu_sizes[i]) + 0.5,
                f"{sc.compression_ratio:.1f}x",
                ha="center", fontsize=9, fontweight="bold",
            )

        fig.tight_layout()
        return self._save_fig(fig, "storage_comparison.png")

    def generate_latency_heatmap(
        self, comparisons: List[ComparisonResult]
    ) -> Optional[str]:
        """Heatmap of median latencies across schemas and query patterns."""
        if not HAS_MATPLOTLIB or not comparisons:
            return None

        schemas = sorted(set(c.schema_name for c in comparisons))
        patterns = sorted(set(c.query_pattern for c in comparisons))

        data = []
        for schema in schemas:
            row = []
            for pattern in patterns:
                vals = [
                    c.speedup
                    for c in comparisons
                    if c.schema_name == schema and c.query_pattern == pattern
                    and c.speedup != float("inf")
                ]
                row.append(sum(vals) / len(vals) if vals else 1.0)
            data.append(row)

        fig, ax = plt.subplots(figsize=(max(8, len(patterns) * 1.5), max(4, len(schemas) * 1.5)))
        im = ax.imshow(data, cmap="RdYlGn", aspect="auto", vmin=0.5, vmax=3.0)
        ax.set_xticks(range(len(patterns)))
        ax.set_xticklabels(patterns, rotation=45, ha="right", fontsize=8)
        ax.set_yticks(range(len(schemas)))
        ax.set_yticklabels(schemas, fontsize=9)
        ax.set_title("Speedup Heatmap (green = Noxu faster)")

        for i in range(len(schemas)):
            for j in range(len(patterns)):
                ax.text(j, i, f"{data[i][j]:.2f}x", ha="center", va="center", fontsize=8)

        fig.colorbar(im, ax=ax, label="Speedup (Noxu/HEAP)")
        fig.tight_layout()
        return self._save_fig(fig, "speedup_heatmap.png")

    def generate_compression_chart(
        self, report: AnalysisReport
    ) -> Optional[str]:
        """Bar chart of per-column compression width ratios."""
        if not HAS_MATPLOTLIB or not report.per_column_compression:
            return None

        # Take the first config's per-column data
        first_key = next(iter(report.per_column_compression))
        col_data = report.per_column_compression[first_key]

        cols = sorted(col_data.keys())
        heap_widths = [col_data[c].get("heap_avg_width", 0) for c in cols]
        noxu_widths = [col_data[c].get("noxu_avg_width", 0) for c in cols]

        fig, ax = plt.subplots(figsize=(max(8, len(cols)), 6))
        x = range(len(cols))
        width = 0.35
        ax.bar([i - width / 2 for i in x], heap_widths, width, label="HEAP avg_width", color="#3498db")
        ax.bar([i + width / 2 for i in x], noxu_widths, width, label="Noxu avg_width", color="#2ecc71")

        ax.set_ylabel("Average Width (bytes)")
        ax.set_title(f"Per-Column Average Width: {first_key}")
        ax.set_xticks(list(x))
        ax.set_xticklabels(cols, rotation=45, ha="right", fontsize=8)
        ax.legend()
        fig.tight_layout()
        return self._save_fig(fig, "column_compression.png")

    # ------------------------------------------------------------------
    # CSV export
    # ------------------------------------------------------------------

    def export_csv(self, report: AnalysisReport) -> str:
        """Export benchmark results to CSV files. Returns path to main CSV."""
        import csv

        # Query timing comparisons
        timing_path = os.path.join(self.output_dir, "timing_results.csv")
        with open(timing_path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow([
                "schema", "row_count", "distribution", "query_pattern",
                "heap_median_s", "noxu_median_s", "speedup",
                "heap_p95_s", "noxu_p95_s",
                "heap_mean_s", "noxu_mean_s",
            ])
            for c in report.comparisons:
                writer.writerow([
                    c.schema_name, c.row_count, c.distribution, c.query_pattern,
                    f"{c.heap_timing.median:.6f}",
                    f"{c.noxu_timing.median:.6f}",
                    f"{c.speedup:.4f}",
                    f"{c.heap_timing.p95:.6f}",
                    f"{c.noxu_timing.p95:.6f}",
                    f"{c.heap_timing.mean:.6f}",
                    f"{c.noxu_timing.mean:.6f}",
                ])

        # Storage comparisons
        storage_path = os.path.join(self.output_dir, "storage_results.csv")
        with open(storage_path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow([
                "schema", "row_count", "distribution",
                "heap_table_bytes", "heap_index_bytes", "heap_total_bytes",
                "noxu_table_bytes", "noxu_index_bytes", "noxu_total_bytes",
                "compression_ratio", "space_savings_pct",
            ])
            for sc in report.storage_comparisons:
                writer.writerow([
                    sc.schema_name, sc.row_count, sc.distribution,
                    sc.heap_table_bytes, sc.heap_index_bytes, sc.heap_total_bytes,
                    sc.noxu_table_bytes, sc.noxu_index_bytes, sc.noxu_total_bytes,
                    f"{sc.compression_ratio:.4f}",
                    f"{sc.space_savings_pct:.2f}",
                ])

        # Per-column compression
        col_path = os.path.join(self.output_dir, "column_compression.csv")
        with open(col_path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow([
                "config", "column", "type",
                "heap_avg_width", "noxu_avg_width", "width_ratio",
                "heap_n_distinct", "noxu_n_distinct",
            ])
            for config_key, cols in report.per_column_compression.items():
                for col_name, stats in cols.items():
                    writer.writerow([
                        config_key, col_name,
                        stats.get("column_type", ""),
                        stats.get("heap_avg_width", ""),
                        stats.get("noxu_avg_width", ""),
                        f"{stats.get('width_ratio', 0):.4f}" if stats.get("width_ratio") else "",
                        stats.get("heap_n_distinct", ""),
                        stats.get("noxu_n_distinct", ""),
                    ])

        logger.info("CSV files written to %s", self.output_dir)
        return timing_path

    # ------------------------------------------------------------------
    # HTML dashboard
    # ------------------------------------------------------------------

    def generate_recommendations(self, report: AnalysisReport) -> list:
        """Generate optimization recommendations based on benchmark results."""
        recs = []
        summary = report.summary

        # Recommendation 1: Column projection performance
        per_pattern = summary.get("per_pattern_avg_speedup", {})
        proj_speedup = per_pattern.get("column_projection", 1.0)
        if proj_speedup < 1.2:
            recs.append({
                "priority": "HIGH",
                "area": "Column Projection",
                "finding": f"Column projection speedup is only {proj_speedup:.2f}x over HEAP.",
                "recommendation": (
                    "Investigate column-skip efficiency. Noxu should show large "
                    "gains for narrow projections on wide tables. Check that "
                    "non-projected columns are truly not read from disk."
                ),
            })
        elif proj_speedup > 2.0:
            recs.append({
                "priority": "INFO",
                "area": "Column Projection",
                "finding": f"Column projection shows strong {proj_speedup:.2f}x speedup.",
                "recommendation": "This is a key Noxu advantage. Highlight in documentation.",
            })

        # Recommendation 2: Aggregation performance
        agg_speedup = per_pattern.get("aggregation", 1.0)
        if agg_speedup < 1.0:
            recs.append({
                "priority": "HIGH",
                "area": "Aggregation",
                "finding": f"Aggregation is {agg_speedup:.2f}x vs HEAP (slower).",
                "recommendation": (
                    "Columnar storage should excel at aggregations. Check for "
                    "unnecessary tuple reconstruction and decompression overhead "
                    "in the aggregation path."
                ),
            })

        # Recommendation 3: Compression ratio
        avg_comp = summary.get("avg_compression_ratio", 1.0)
        if avg_comp < 1.5:
            recs.append({
                "priority": "MEDIUM",
                "area": "Compression",
                "finding": f"Average compression ratio is only {avg_comp:.2f}x.",
                "recommendation": (
                    "Consider implementing additional compression strategies: "
                    "dictionary encoding for low-cardinality text, RLE for "
                    "clustered data, and delta encoding for sorted integers."
                ),
            })

        # Recommendation 4: Full scan overhead
        full_scan_speedup = per_pattern.get("full_scan", 1.0)
        if full_scan_speedup < 0.8:
            recs.append({
                "priority": "MEDIUM",
                "area": "Full Table Scan",
                "finding": f"Full scan is {full_scan_speedup:.2f}x vs HEAP (regression).",
                "recommendation": (
                    "Full scans that read all columns should be close to HEAP "
                    "performance. The overhead suggests tuple reconstruction cost "
                    "is significant. Consider optimizing the column-to-tuple "
                    "assembly path."
                ),
            })

        # Recommendation 5: Index scan performance
        idx_speedup = per_pattern.get("index_scan", 1.0)
        if idx_speedup < 0.9:
            recs.append({
                "priority": "MEDIUM",
                "area": "Index Scan",
                "finding": f"Index scan is {idx_speedup:.2f}x vs HEAP (regression).",
                "recommendation": (
                    "Point lookups via index should not regress. Check that "
                    "TID-to-column-page mapping is efficient and does not "
                    "require scanning through column pages sequentially."
                ),
            })

        # Recommendation 6: Storage efficiency per data type
        for config_key, col_data in report.per_column_compression.items():
            for col_name, stats in col_data.items():
                ratio = stats.get("width_ratio", 0)
                col_type = stats.get("column_type", "")
                if ratio > 0 and ratio < 1.0:
                    recs.append({
                        "priority": "LOW",
                        "area": f"Column Storage ({col_name})",
                        "finding": (
                            f"Column '{col_name}' ({col_type}) has width ratio "
                            f"{ratio:.2f} (Noxu wider than HEAP)."
                        ),
                        "recommendation": (
                            f"Investigate per-column overhead for {col_type} type. "
                            "The columnar format should not be wider than HEAP."
                        ),
                    })
            break  # Only check first configuration

        # If no issues found, add a positive recommendation
        if not recs:
            recs.append({
                "priority": "INFO",
                "area": "Overall",
                "finding": "Benchmark results look good across all patterns.",
                "recommendation": (
                    "Continue with larger dataset sizes to identify scaling behavior."
                ),
            })

        return recs

    def generate_dashboard(self, report: AnalysisReport) -> str:
        """Generate a self-contained HTML dashboard. Returns path to HTML file."""
        charts = {}
        if HAS_MATPLOTLIB:
            charts["speedup"] = self.generate_speedup_chart(report.comparisons)
            charts["storage"] = self.generate_storage_chart(report.storage_comparisons)
            charts["heatmap"] = self.generate_latency_heatmap(report.comparisons)
            charts["compression"] = self.generate_compression_chart(report)

        recommendations = self.generate_recommendations(report)
        html_content = self._render_html(report, charts, recommendations)
        path = os.path.join(self.output_dir, "dashboard.html")
        with open(path, "w") as f:
            f.write(html_content)
        logger.info("Dashboard written to %s", path)
        return path

    def _render_html(
        self, report: AnalysisReport, charts: Dict[str, Optional[str]],
        recommendations: Optional[list] = None,
    ) -> str:
        summary = report.summary

        # Build timing table
        timing_rows = ""
        for c in report.comparisons:
            color = "#2ecc71" if c.speedup > 1.0 else "#e74c3c"
            timing_rows += f"""
            <tr>
                <td>{html.escape(c.schema_name)}</td>
                <td>{c.row_count:,}</td>
                <td>{html.escape(c.distribution)}</td>
                <td>{html.escape(c.query_pattern)}</td>
                <td>{c.heap_timing.median * 1000:.2f}</td>
                <td>{c.noxu_timing.median * 1000:.2f}</td>
                <td style="color: {color}; font-weight: bold;">{c.speedup:.2f}x</td>
            </tr>"""

        # Build storage table
        storage_rows = ""
        for sc in report.storage_comparisons:
            storage_rows += f"""
            <tr>
                <td>{html.escape(sc.schema_name)}</td>
                <td>{sc.row_count:,}</td>
                <td>{html.escape(sc.distribution)}</td>
                <td>{_human_bytes(sc.heap_total_bytes)}</td>
                <td>{_human_bytes(sc.noxu_total_bytes)}</td>
                <td style="font-weight: bold;">{sc.compression_ratio:.2f}x</td>
                <td>{sc.space_savings_pct:.1f}%</td>
            </tr>"""

        # Chart image tags
        def img_tag(name: Optional[str]) -> str:
            if name:
                return f'<img src="{html.escape(name)}" style="max-width:100%;margin:10px 0;">'
            return '<p style="color:#999;">Chart not available (matplotlib not installed)</p>'

        summary_json = html.escape(json.dumps(summary, indent=2, default=str))

        # Build recommendations HTML
        rec_rows = ""
        if recommendations:
            priority_colors = {
                "HIGH": "#e74c3c",
                "MEDIUM": "#f39c12",
                "LOW": "#3498db",
                "INFO": "#2ecc71",
            }
            for rec in recommendations:
                color = priority_colors.get(rec["priority"], "#999")
                rec_rows += f"""
            <tr>
                <td style="color: {color}; font-weight: bold;">{html.escape(rec['priority'])}</td>
                <td>{html.escape(rec['area'])}</td>
                <td>{html.escape(rec['finding'])}</td>
                <td>{html.escape(rec['recommendation'])}</td>
            </tr>"""

        return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Noxu Benchmark Dashboard</title>
<style>
  body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
         max-width: 1200px; margin: 0 auto; padding: 20px; background: #f5f5f5; }}
  h1 {{ color: #2c3e50; border-bottom: 3px solid #3498db; padding-bottom: 10px; }}
  h2 {{ color: #34495e; margin-top: 30px; }}
  .card {{ background: white; border-radius: 8px; padding: 20px; margin: 15px 0;
           box-shadow: 0 2px 4px rgba(0,0,0,0.1); }}
  .summary-grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
                   gap: 15px; }}
  .metric {{ text-align: center; padding: 15px; background: #ecf0f1; border-radius: 6px; }}
  .metric .value {{ font-size: 2em; font-weight: bold; color: #2c3e50; }}
  .metric .label {{ font-size: 0.85em; color: #7f8c8d; margin-top: 5px; }}
  table {{ border-collapse: collapse; width: 100%; }}
  th, td {{ border: 1px solid #ddd; padding: 8px 12px; text-align: right; }}
  th {{ background: #3498db; color: white; }}
  tr:nth-child(even) {{ background: #f9f9f9; }}
  tr:hover {{ background: #eef; }}
  td:first-child, td:nth-child(3), td:nth-child(4) {{ text-align: left; }}
  pre {{ background: #2c3e50; color: #ecf0f1; padding: 15px; border-radius: 6px;
         overflow-x: auto; font-size: 0.85em; }}
  .charts {{ display: grid; grid-template-columns: 1fr 1fr; gap: 20px; }}
  @media (max-width: 800px) {{ .charts {{ grid-template-columns: 1fr; }} }}
</style>
</head>
<body>
<h1>Noxu Benchmark Dashboard</h1>

<div class="card">
<h2>Summary</h2>
<div class="summary-grid">
  <div class="metric">
    <div class="value">{summary.get('median_speedup', 0):.2f}x</div>
    <div class="label">Median Query Speedup</div>
  </div>
  <div class="metric">
    <div class="value">{summary.get('max_speedup', 0):.2f}x</div>
    <div class="label">Best Speedup</div>
  </div>
  <div class="metric">
    <div class="value">{summary.get('avg_compression_ratio', 0):.2f}x</div>
    <div class="label">Avg Compression Ratio</div>
  </div>
  <div class="metric">
    <div class="value">{summary.get('avg_space_savings_pct', 0):.1f}%</div>
    <div class="label">Avg Space Savings</div>
  </div>
</div>
</div>

<div class="card">
<h2>Charts</h2>
<div class="charts">
  <div>{img_tag(charts.get("speedup"))}</div>
  <div>{img_tag(charts.get("storage"))}</div>
  <div>{img_tag(charts.get("heatmap"))}</div>
  <div>{img_tag(charts.get("compression"))}</div>
</div>
</div>

<div class="card">
<h2>Query Timing Comparison</h2>
<table>
<thead>
<tr>
  <th>Schema</th><th>Rows</th><th>Distribution</th><th>Pattern</th>
  <th>HEAP (ms)</th><th>Noxu (ms)</th><th>Speedup</th>
</tr>
</thead>
<tbody>
{timing_rows}
</tbody>
</table>
</div>

<div class="card">
<h2>Storage Comparison</h2>
<table>
<thead>
<tr>
  <th>Schema</th><th>Rows</th><th>Distribution</th>
  <th>HEAP Total</th><th>Noxu Total</th><th>Compression</th><th>Savings</th>
</tr>
</thead>
<tbody>
{storage_rows}
</tbody>
</table>
</div>

<div class="card">
<h2>Optimization Recommendations</h2>
<table>
<thead>
<tr>
  <th style="width:80px;">Priority</th><th style="width:150px;">Area</th>
  <th>Finding</th><th>Recommendation</th>
</tr>
</thead>
<tbody>
{rec_rows}
</tbody>
</table>
</div>

<div class="card">
<h2>Raw Summary Data</h2>
<pre>{summary_json}</pre>
</div>

<footer style="text-align:center;color:#999;margin-top:30px;padding:20px;">
  Generated by Noxu Benchmark Suite
</footer>
</body>
</html>"""

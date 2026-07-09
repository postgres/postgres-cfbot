"""
Results analyzer for atomics benchmark comparisons.

This module provides statistical analysis and visualization of benchmark results,
helping identify performance regressions or improvements from the stdatomic.h
implementation.
"""

import json
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass
class ResultComparison:
    """Comparison of a single benchmark scenario."""
    schema: str
    row_count: int
    distribution: str
    pattern: str
    traditional_ms: float
    stdatomic_ms: float
    speedup: float
    difference_pct: float  # (traditional - stdatomic) / traditional * 100


class AtomicsResultAnalyzer:
    """Analyzes and compares benchmark results."""

    def __init__(self, results_file: Path):
        self.results_file = results_file
        self.comparisons: List[ResultComparison] = []
        self._load_results()

    def _load_results(self):
        """Load results from JSON file."""
        with open(self.results_file) as f:
            data = json.load(f)

        for r in data.get("results", []):
            diff_pct = (
                (r["traditional_ms"] - r["stdatomic_ms"]) / r["traditional_ms"] * 100
            )
            comp = ResultComparison(
                schema=r["schema"],
                row_count=r["row_count"],
                distribution=r["distribution"],
                pattern=r["pattern"],
                traditional_ms=r["traditional_ms"],
                stdatomic_ms=r["stdatomic_ms"],
                speedup=r["speedup"],
                difference_pct=diff_pct,
            )
            self.comparisons.append(comp)

    def analyze_overall(self) -> Dict:
        """Analyze overall performance characteristics."""
        if not self.comparisons:
            return {}

        speedups = [c.speedup for c in self.comparisons]
        diff_pcts = [c.difference_pct for c in self.comparisons]

        # Statistical significance: count how many are outside ±2% threshold
        significant_improvements = sum(1 for d in diff_pcts if d > 2.0)
        significant_regressions = sum(1 for d in diff_pcts if d < -2.0)
        within_noise = sum(1 for d in diff_pcts if -2.0 <= d <= 2.0)

        return {
            "total_scenarios": len(self.comparisons),
            "median_speedup": statistics.median(speedups),
            "mean_speedup": statistics.mean(speedups),
            "stdev_speedup": statistics.stdev(speedups) if len(speedups) > 1 else 0.0,
            "min_speedup": min(speedups),
            "max_speedup": max(speedups),
            "median_diff_pct": statistics.median(diff_pcts),
            "mean_diff_pct": statistics.mean(diff_pcts),
            "significant_improvements": significant_improvements,
            "significant_regressions": significant_regressions,
            "within_noise": within_noise,
        }

    def analyze_by_pattern(self) -> Dict[str, Dict]:
        """Break down performance by query pattern."""
        pattern_groups = {}
        for comp in self.comparisons:
            if comp.pattern not in pattern_groups:
                pattern_groups[comp.pattern] = []
            pattern_groups[comp.pattern].append(comp)

        results = {}
        for pattern, comps in pattern_groups.items():
            speedups = [c.speedup for c in comps]
            diff_pcts = [c.difference_pct for c in comps]

            results[pattern] = {
                "count": len(comps),
                "median_speedup": statistics.median(speedups),
                "mean_speedup": statistics.mean(speedups),
                "median_diff_pct": statistics.median(diff_pcts),
                "mean_diff_pct": statistics.mean(diff_pcts),
                "improvements": sum(1 for d in diff_pcts if d > 2.0),
                "regressions": sum(1 for d in diff_pcts if d < -2.0),
            }

        return results

    def analyze_by_scale(self) -> Dict[int, Dict]:
        """Break down performance by data scale."""
        scale_groups = {}
        for comp in self.comparisons:
            if comp.row_count not in scale_groups:
                scale_groups[comp.row_count] = []
            scale_groups[comp.row_count].append(comp)

        results = {}
        for row_count, comps in scale_groups.items():
            speedups = [c.speedup for c in comps]
            diff_pcts = [c.difference_pct for c in comps]

            results[row_count] = {
                "count": len(comps),
                "median_speedup": statistics.median(speedups),
                "mean_speedup": statistics.mean(speedups),
                "median_diff_pct": statistics.median(diff_pcts),
                "improvements": sum(1 for d in diff_pcts if d > 2.0),
                "regressions": sum(1 for d in diff_pcts if d < -2.0),
            }

        return results

    def find_regressions(self, threshold_pct: float = 2.0) -> List[ResultComparison]:
        """Find scenarios where stdatomic is slower than threshold."""
        return [c for c in self.comparisons if c.difference_pct < -threshold_pct]

    def find_improvements(self, threshold_pct: float = 2.0) -> List[ResultComparison]:
        """Find scenarios where stdatomic is faster than threshold."""
        return [c for c in self.comparisons if c.difference_pct > threshold_pct]

    def generate_report(self, output_file: Optional[Path] = None) -> str:
        """Generate a comprehensive text report."""
        lines = []
        lines.append("=" * 80)
        lines.append("PostgreSQL Atomics Benchmark Analysis Report")
        lines.append("=" * 80)
        lines.append("")

        # Overall analysis
        overall = self.analyze_overall()
        lines.append("OVERALL PERFORMANCE")
        lines.append("-" * 80)
        lines.append(f"Total scenarios:           {overall['total_scenarios']}")
        lines.append(f"Median speedup:            {overall['median_speedup']:.3f}x")
        lines.append(f"Mean speedup:              {overall['mean_speedup']:.3f}x ± {overall['stdev_speedup']:.3f}")
        lines.append(f"Range:                     {overall['min_speedup']:.3f}x - {overall['max_speedup']:.3f}x")
        lines.append("")
        lines.append(f"Median difference:         {overall['median_diff_pct']:+.2f}%")
        lines.append(f"Mean difference:           {overall['mean_diff_pct']:+.2f}%")
        lines.append("")
        lines.append(f"Significant improvements:  {overall['significant_improvements']} (>{2.0:.1f}%)")
        lines.append(f"Significant regressions:   {overall['significant_regressions']} (<-{2.0:.1f}%)")
        lines.append(f"Within measurement noise:  {overall['within_noise']} (±{2.0:.1f}%)")
        lines.append("")

        # Verdict
        median_diff = overall["median_diff_pct"]
        if abs(median_diff) < 2.0:
            verdict = "NEUTRAL - Performance is statistically equivalent"
        elif median_diff > 0:
            verdict = f"POSITIVE - Stdatomic is faster by {median_diff:.2f}%"
        else:
            verdict = f"NEGATIVE - Stdatomic is slower by {abs(median_diff):.2f}%"

        lines.append(f"VERDICT: {verdict}")
        lines.append("")

        # By pattern
        by_pattern = self.analyze_by_pattern()
        lines.append("PERFORMANCE BY QUERY PATTERN")
        lines.append("-" * 80)
        lines.append(f"{'Pattern':<30} {'Median':>10} {'Mean':>10} {'Diff%':>10} {'+/-':>10}")
        lines.append("-" * 80)

        for pattern in sorted(by_pattern.keys()):
            stats = by_pattern[pattern]
            indicator = "+++" if stats["mean_diff_pct"] > 5.0 else "   "
            indicator = "---" if stats["mean_diff_pct"] < -5.0 else indicator
            lines.append(
                f"{pattern:<30} {stats['median_speedup']:>9.3f}x "
                f"{stats['mean_speedup']:>9.3f}x "
                f"{stats['mean_diff_pct']:>9.2f}% "
                f"{indicator}"
            )

        lines.append("")

        # By scale
        by_scale = self.analyze_by_scale()
        lines.append("PERFORMANCE BY DATA SCALE")
        lines.append("-" * 80)
        lines.append(f"{'Rows':<15} {'Median':>10} {'Mean':>10} {'Diff%':>10} {'Status':<20}")
        lines.append("-" * 80)

        for row_count in sorted(by_scale.keys()):
            stats = by_scale[row_count]
            status = "Faster" if stats["median_diff_pct"] > 2.0 else "Neutral"
            status = "Slower" if stats["median_diff_pct"] < -2.0 else status
            lines.append(
                f"{row_count:<15,} {stats['median_speedup']:>9.3f}x "
                f"{stats['mean_speedup']:>9.3f}x "
                f"{stats['median_diff_pct']:>9.2f}% "
                f"{status:<20}"
            )

        lines.append("")

        # Top regressions
        regressions = sorted(
            self.find_regressions(2.0), key=lambda c: c.difference_pct
        )
        if regressions:
            lines.append("TOP REGRESSIONS (stdatomic slower by >2%)")
            lines.append("-" * 80)
            for comp in regressions[:10]:
                lines.append(
                    f"  {comp.difference_pct:>6.2f}% - {comp.pattern:<25} "
                    f"{comp.schema}/{comp.row_count:,}/{comp.distribution}"
                )
            lines.append("")

        # Top improvements
        improvements = sorted(
            self.find_improvements(2.0), key=lambda c: c.difference_pct, reverse=True
        )
        if improvements:
            lines.append("TOP IMPROVEMENTS (stdatomic faster by >2%)")
            lines.append("-" * 80)
            for comp in improvements[:10]:
                lines.append(
                    f"  {comp.difference_pct:>6.2f}% - {comp.pattern:<25} "
                    f"{comp.schema}/{comp.row_count:,}/{comp.distribution}"
                )
            lines.append("")

        lines.append("=" * 80)

        report = "\n".join(lines)

        if output_file:
            output_file.parent.mkdir(parents=True, exist_ok=True)
            with open(output_file, "w") as f:
                f.write(report)

        return report


def main():
    """CLI entry point for results analysis."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Analyze atomics benchmark results"
    )
    parser.add_argument(
        "results_file",
        type=Path,
        help="Path to results.json file",
    )
    parser.add_argument(
        "--output",
        "-o",
        type=Path,
        help="Output file for report (default: stdout)",
    )

    args = parser.parse_args()

    if not args.results_file.exists():
        print(f"ERROR: Results file not found: {args.results_file}")
        return 1

    analyzer = AtomicsResultAnalyzer(args.results_file)
    report = analyzer.generate_report(args.output)

    if not args.output:
        print(report)

    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())

# PostgreSQL Atomics Performance Benchmark Suite

This benchmark suite compares the performance of traditional platform-specific atomics implementation versus the C11 stdatomic.h implementation.

## Overview

The benchmark evaluates atomic operations impact across different:
- **Workload patterns**: Full scans, aggregations, index scans, concurrent operations
- **Table schemas**: Narrow (4 cols), medium (11 cols), wide (55 cols)
- **Data distributions**: Random, clustered, low cardinality
- **Data scales**: 10K, 100K, 1M, 10M, 100M rows

## Prerequisites

1. **Two PostgreSQL builds**:
   ```bash
   # Traditional atomics
   meson setup build-traditional -Duse_stdatomic=no --buildtype=debugoptimized
   meson compile -C build-traditional

   # Stdatomic.h implementation
   meson setup build-stdatomic -Duse_stdatomic=yes --buildtype=debugoptimized
   meson compile -C build-stdatomic
   ```

2. **Python dependencies**:
   ```bash
   pip install asyncpg
   ```

## Usage

### Quick Start

Run with default settings (medium test matrix):

```bash
cd src/test/benchmarks
python -m atomics_cli
```

This will:
- Start two PostgreSQL instances (ports 5433 and 5434)
- Run benchmarks on both implementations
- Compare results
- Save to `atomics_benchmark_results/results.json`

### Full Matrix

Run comprehensive tests including large datasets:

```bash
python -m atomics_cli --full-matrix
```

**Warning**: This can take several hours.

### Custom Configuration

```bash
# Specify custom build directories
python -m atomics_cli \
    --traditional-build /path/to/build-traditional \
    --stdatomic-build /path/to/build-stdatomic

# Test specific schema and row counts
python -m atomics_cli --schema medium --rows 100000 1000000

# Test specific query patterns
python -m atomics_cli --pattern full_scan --pattern aggregation

# Adjust measurement precision
python -m atomics_cli --warmup 5 --iterations 20

# Verbose logging
python -m atomics_cli -v
```

### Analyzing Results

After running benchmarks, analyze the results:

```bash
python -m atomics_analyzer atomics_benchmark_results/results.json
```

Or save to a file:

```bash
python -m atomics_analyzer atomics_benchmark_results/results.json \
    --output analysis_report.txt
```

## Benchmark Methodology

### Query Patterns

1. **full_scan**: Sequential scan - stresses spinlock acquisition and buffer management
2. **column_projection**: Selective scan - tuple visibility checks with atomic reads
3. **filtered_scan**: Index + sequential - mixed contention patterns
4. **aggregation**: Heavy computation - shared buffer access
5. **group_by**: Hash aggregation - memory ordering stress
6. **index_scan**: Index-only - buffer pin/unpin contention

### Measurement Process

For each scenario:
1. **Warmup**: Run query N times (default: 3) to prime caches
2. **Measure**: Run query M times (default: 10), record each timing
3. **Report**: Use **median** timing to minimize outlier impact

### Statistical Significance

- **Noise threshold**: ±2% (measurements within this are considered equivalent)
- **Significant improvement**: >2% faster
- **Significant regression**: >2% slower

### Good Benchmarking Practices

1. **Dedicated hardware**: Run on isolated system, no background processes
2. **CPU frequency**: Disable CPU frequency scaling:
   ```bash
   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```
3. **NUMA**: Pin PostgreSQL to single NUMA node if applicable
4. **Disk I/O**: Use fast storage (SSD/NVMe) or tmpfs for data directories
5. **Multiple runs**: Run benchmark multiple times, compare results
6. **Baseline**: Establish baseline performance before code changes

## Output Format

Results are saved in JSON format:

```json
{
  "summary": {
    "total_scenarios": 54,
    "median_speedup": 1.002,
    "mean_speedup": 1.005,
    "significant_improvements": 5,
    "significant_regressions": 2,
    "per_pattern_avg_speedup": {
      "full_scan": 0.998,
      "aggregation": 1.015,
      ...
    }
  },
  "results": [
    {
      "schema": "atomics_medium",
      "row_count": 100000,
      "distribution": "random",
      "pattern": "full_scan",
      "traditional_ms": 125.3,
      "stdatomic_ms": 126.1,
      "speedup": 0.994
    },
    ...
  ]
}
```

## Interpreting Results

### Speedup Metric

- **Speedup = traditional_ms / stdatomic_ms**
- `speedup > 1.0`: stdatomic is **faster**
- `speedup < 1.0`: stdatomic is **slower**
- `speedup ≈ 1.0`: Performance is **equivalent**

### Example Analysis

```
VERDICT: NEUTRAL - Performance is statistically equivalent

Median speedup: 1.002x (stdatomic ~0.2% faster)
Within measurement noise: 47/54 scenarios (87%)
Significant improvements: 5 (9%)
Significant regressions: 2 (4%)
```

**Interpretation**: The stdatomic.h implementation has no material performance impact.

## Troubleshooting

### Port Already in Use

```
ERROR: Port 5433 already in use
```

**Solution**: Stop existing PostgreSQL instances or use different ports:
```bash
python -m atomics_cli --traditional-port 5435 --stdatomic-port 5436
```

### Build Directory Not Found

```
ERROR: Traditional build directory not found: ./build-traditional
```

**Solution**: Build PostgreSQL first or specify correct path:
```bash
python -m atomics_cli --traditional-build /path/to/build
```

### Insufficient Disk Space

For large datasets, ensure adequate space in `/tmp`:
```bash
df -h /tmp
```

Consider using custom data directories:
```bash
python -m atomics_cli \
    --traditional-datadir /mnt/fast-storage/pgdata-trad \
    --stdatomic-datadir /mnt/fast-storage/pgdata-std
```

### Connection Timeout

If PostgreSQL takes >30s to start, check:
1. Disk I/O performance
2. System resources (RAM, CPU)
3. PostgreSQL logs in data directory

## Architecture

```
src/test/benchmarks/
├── atomics_config.py      - Configuration and schema definitions
├── atomics_benchmark.py   - Main orchestrator
├── atomics_workload.py    - Query generation and execution
├── atomics_cli.py         - Command-line interface
├── atomics_analyzer.py    - Results analysis
├── data_generator.py      - Data generation
└── README_ATOMICS.md      - This file
```

## For PostgreSQL Committers

This benchmark suite is designed for:
- Validating atomics implementation changes
- Performance regression testing
- Platform-specific optimization evaluation
- Memory ordering impact analysis

### CI Integration

For automated testing:

```bash
# Quick smoke test (fast, ~5 minutes)
python -m atomics_cli --schema narrow --rows 10000 --iterations 3

# Standard test (moderate, ~30 minutes)
python -m atomics_cli --schema medium --rows 100000 1000000

# Full validation (slow, several hours)
python -m atomics_cli --full-matrix
```

### Expected Performance

On x86_64 with strong memory model:
- **Median difference**: ±1-2% (within noise)
- **Pattern variation**: Some patterns may show ±5% variation
- **Scale impact**: Minimal difference across scales

On ARM64/RISC-V with weak memory model:
- **Potential regression**: 2-10% (stronger ordering overhead)
- **Workload dependent**: Write-heavy workloads more impacted
- **Trade-off**: Correctness vs. performance

## References

- [SEMANTIC_DIFFERENCES.md](../../SEMANTIC_DIFFERENCES.md) - Implementation differences
- [MEMORY_ORDERING_ANALYSIS.md](../../MEMORY_ORDERING_ANALYSIS.md) - Memory ordering decisions
- [PostgreSQL atomics documentation](https://www.postgresql.org/docs/current/atomics.html)

/*
 * src/pgbench_chart.c
 * Reference C implementation of the 439-byte wasm/pgbench_chart.wasm kernel.
 *
 * Can be compiled with clang/lld:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--export-all \
 *         -o wasm/pgbench_chart.wasm src/pgbench_chart.c
 * Or assembled reproducibly with zero external dependencies via:
 *   python3 src/build_wasm.py
 */

typedef unsigned int uint32_t;

__attribute__((export_name("series_min")))
double
series_min(const double *ptr, uint32_t n)
{
    if (n == 0)
        return 0.0;
    double acc = ptr[0];
    for (uint32_t i = 1; i < n; i++)
    {
        double v = ptr[i];
        acc = __builtin_wasm_min_f64(acc, v);
    }
    return acc;
}

__attribute__((export_name("series_max")))
double
series_max(const double *ptr, uint32_t n)
{
    if (n == 0)
        return 0.0;
    double acc = ptr[0];
    for (uint32_t i = 1; i < n; i++)
    {
        double v = ptr[i];
        acc = __builtin_wasm_max_f64(acc, v);
    }
    return acc;
}

__attribute__((export_name("series_mean")))
double
series_mean(const double *ptr, uint32_t n)
{
    if (n == 0)
        return 0.0;
    double acc = 0.0;
    for (uint32_t i = 0; i < n; i++)
        acc += ptr[i];
    return acc / (double) n;
}

__attribute__((export_name("project_1d")))
void
project_1d(const double *in_ptr, uint32_t n, double min_v, double max_v,
           double out_start, double out_span, double *out_ptr)
{
    double span = max_v - min_v;
    if (__builtin_fabs(span) < 1e-12)
        span = 1.0;
    for (uint32_t i = 0; i < n; i++)
        out_ptr[i] = out_start + ((in_ptr[i] - min_v) / span) * out_span;
}

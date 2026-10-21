;; src/pgbench_chart.wat
;; Minimalistic 439-byte WebAssembly f64 statistical & 1D SVG coordinate projection kernel
;; Exported interface:
;;   - memory (2 pages = 128 KiB linear memory for Float64Array views)
;;   - series_min(ptr: i32, n: i32) -> f64
;;   - series_max(ptr: i32, n: i32) -> f64
;;   - series_mean(ptr: i32, n: i32) -> f64
;;   - project_1d(in_ptr: i32, n: i32, min_v: f64, max_v: f64,
;;                out_start: f64, out_span: f64, out_ptr: i32) -> void
(module
  (type $t_stat (func (param i32 i32) (result f64)))
  (type $t_proj (func (param i32 i32 f64 f64 f64 f64 i32)))
  (memory (export "memory") 2)

  ;; series_min(ptr, n) -> f64
  (func (export "series_min") (type $t_stat) (param $ptr i32) (param $n i32) (result f64)
    (local $i i32)
    (local $acc f64)
    (if (result f64) (i32.eqz (local.get $n))
      (then (f64.const 0.0))
      (else
        (local.set $acc (f64.load align=8 (local.get $ptr)))
        (local.set $i (i32.const 1))
        (block $brk
          (loop $lp
            (br_if $brk (i32.ge_u (local.get $i) (local.get $n)))
            (local.set $acc
              (f64.min
                (local.get $acc)
                (f64.load align=8
                  (i32.add (local.get $ptr) (i32.shl (local.get $i) (i32.const 3))))))
            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $lp)))
        (local.get $acc))))

  ;; series_max(ptr, n) -> f64
  (func (export "series_max") (type $t_stat) (param $ptr i32) (param $n i32) (result f64)
    (local $i i32)
    (local $acc f64)
    (if (result f64) (i32.eqz (local.get $n))
      (then (f64.const 0.0))
      (else
        (local.set $acc (f64.load align=8 (local.get $ptr)))
        (local.set $i (i32.const 1))
        (block $brk
          (loop $lp
            (br_if $brk (i32.ge_u (local.get $i) (local.get $n)))
            (local.set $acc
              (f64.max
                (local.get $acc)
                (f64.load align=8
                  (i32.add (local.get $ptr) (i32.shl (local.get $i) (i32.const 3))))))
            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $lp)))
        (local.get $acc))))

  ;; series_mean(ptr, n) -> f64
  (func (export "series_mean") (type $t_stat) (param $ptr i32) (param $n i32) (result f64)
    (local $i i32)
    (local $acc f64)
    (if (result f64) (i32.eqz (local.get $n))
      (then (f64.const 0.0))
      (else
        (local.set $acc (f64.const 0.0))
        (local.set $i (i32.const 0))
        (block $brk
          (loop $lp
            (br_if $brk (i32.ge_u (local.get $i) (local.get $n)))
            (local.set $acc
              (f64.add
                (local.get $acc)
                (f64.load align=8
                  (i32.add (local.get $ptr) (i32.shl (local.get $i) (i32.const 3))))))
            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $lp)))
        (f64.div (local.get $acc) (f64.convert_i32_u (local.get $n))))))

  ;; project_1d(in_ptr, n, min_v, max_v, out_start, out_span, out_ptr)
  (func (export "project_1d") (type $t_proj)
    (param $in_ptr i32) (param $n i32) (param $min_v f64) (param $max_v f64)
    (param $out_start f64) (param $out_span f64) (param $out_ptr i32)
    (local $i i32)
    (local $span f64)
    (local.set $span (f64.sub (local.get $max_v) (local.get $min_v)))
    (if (f64.lt (f64.abs (local.get $span)) (f64.const 1e-12))
      (then (local.set $span (f64.const 1.0))))
    (local.set $i (i32.const 0))
    (block $brk
      (loop $lp
        (br_if $brk (i32.ge_u (local.get $i) (local.get $n)))
        (f64.store align=8
          (i32.add (local.get $out_ptr) (i32.shl (local.get $i) (i32.const 3)))
          (f64.add
            (local.get $out_start)
            (f64.mul
              (f64.div
                (f64.sub
                  (f64.load align=8
                    (i32.add (local.get $in_ptr) (i32.shl (local.get $i) (i32.const 3))))
                  (local.get $min_v))
                (local.get $span))
              (local.get $out_span))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $lp))))
)

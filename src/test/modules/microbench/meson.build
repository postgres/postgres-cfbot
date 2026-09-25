# Copyright (c) 2024-2026, PostgreSQL Global Development Group

microbench_tests = [
  'lwlock',
]

microbench_sources = files(
  'multiprocessing.c',
) + files(
  '@0@/bench.c'.format(test) for test in microbench_tests
)

if host_system == 'windows'
  microbench_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'microbench',
    '--FILEDESC', 'microbench - micro-benchmark suite',])
endif

microbench_sql = custom_target('microbench--1.0.sql',
  output: 'microbench--1.0.sql',
  input: files('microbench--1.0.sql.head') + files(
    '@0@/install.sql'.format(test) for test in microbench_tests
  ),
  command: [find_program('cat'), '@INPUT@'],
  build_by_default: true,
)

microbench = shared_module('microbench',
  microbench_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += microbench

test_install_data += microbench_sql
test_install_data += files('microbench.control')

# Copyright (c) 2026, PostgreSQL Global Development Group

test_pg_threads_sources = files(
  'test_pg_threads.c',
)

if host_system == 'windows'
  test_pg_threads_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_pg_threads',
    '--FILEDESC', 'test pg_threads.h program',])
endif

test_pg_threads = executable('test_pg_threads',
  test_pg_threads_sources,
  dependencies: [frontend_code, thread_dep],
  kwargs: default_bin_args + {
    'install': false,
  },
)
testprep_targets += test_pg_threads


tests += {
  'name': 'test_pg_threads',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_test_pg_threads.pl',
    ],
    'deps': [test_pg_threads],
  },
}

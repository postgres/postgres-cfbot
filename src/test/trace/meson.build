# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_regress_c helpfully provided by regress/meson.build

trace_sources = pg_regress_c + files(
  'trace_main.c',
)

if host_system == 'windows'
  trace_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_trace_regress',
    '--FILEDESC', 'pg_trace_regress - libpq trace test driver',])
endif

pg_trace_regress = executable('pg_trace_regress',
  trace_sources,
  c_args: pg_regress_cflags,
  include_directories: pg_regress_inc,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install_dir': dir_pgxs / 'src/test/trace',
  },
)
bin_targets += pg_trace_regress

tests += {
  'name': 'trace',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'trace': {
    'schedule': files('trace_schedule'),
    'test_kwargs': {
      'priority': 40,
      'timeout': 1000,
    },
    'dbname': 'trace_regression',
  },
}

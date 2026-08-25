# Copyright (c) 2026, PostgreSQL Global Development Group

pgbench_ext_sources = files(
  'pgbench.c',
  '../../src/common/pgbench_funcs.c',
)

if host_system == 'windows'
  pgbench_ext_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgbench',
    '--FILEDESC', 'pgbench - random distributions and benchmarking utilities',])
endif

pgbench_ext = shared_module('pgbench',
  pgbench_ext_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += pgbench_ext

install_data(
  'pgbench.control',
  'pgbench--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pgbench_ext',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pgbench',
    ],
  },
}

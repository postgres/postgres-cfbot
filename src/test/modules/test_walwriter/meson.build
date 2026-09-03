# Copyright (c) 2026, PostgreSQL Global Development Group

test_walwriter_sources = files(
  'test_walwriter.c',
)

if host_system == 'windows'
  test_walwriter_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_walwriter',
    '--FILEDESC', 'test_walwriter - test facilities for the WAL writer',])
endif

test_walwriter = shared_module('test_walwriter',
  test_walwriter_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_walwriter

test_install_data += files(
  'test_walwriter.control',
  'test_walwriter--1.0.sql',
)

tests += {
  'name': 'test_walwriter',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_bogus_async_lsn.pl',
    ],
  },
}

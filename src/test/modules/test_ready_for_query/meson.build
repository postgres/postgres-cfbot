# Copyright (c) 2026, PostgreSQL Global Development Group

test_ready_for_query_sources = files(
  'test_ready_for_query.c',
)

if host_system == 'windows'
  test_ready_for_query_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_ready_for_query',
    '--FILEDESC', 'test_ready_for_query - test code for ReadyForQuery hook',])
endif

test_ready_for_query = shared_module('test_ready_for_query',
  test_ready_for_query_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_ready_for_query

test_install_data += files(
  'test_ready_for_query.control',
  'test_ready_for_query--1.0.sql',
)

tests += {
  'name': 'test_ready_for_query',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_ready_for_query',
    ],
  },
  'tap': {
    'tests': [
      't/001_ready_for_query.pl',
    ],
  },
}

# Copyright (c) 2026, PostgreSQL Global Development Group

dummy_table_am_sources = files(
  'dummy_table_am.c',
)

if host_system == 'windows'
  dummy_table_am_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dummy_table_am',
    '--FILEDESC', 'dummy_table_am - table access method template',])
endif

dummy_table_am = shared_module('dummy_table_am',
  dummy_table_am_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += dummy_table_am

test_install_data += files(
  'dummy_table_am.control',
  'dummy_table_am--1.0.sql',
)

tests += {
  'name': 'dummy_table_am',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'reloptions',
    ],
  },
}

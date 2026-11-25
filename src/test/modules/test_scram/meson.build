# Copyright (c) 2026, PostgreSQL Global Development Group

test_scram_sources = files(
  'test_scram.c',
)

if host_system == 'windows'
  test_scram_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_scram',
    '--FILEDESC', 'test_scram - test SCRAM helpers in src/common',])
endif

test_scram = shared_module('test_scram',
  test_scram_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_scram

test_install_data += files(
  'test_scram.control',
  'test_scram--1.0.sql',
)

tests += {
  'name': 'test_scram',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_scram',
    ],
  },
}

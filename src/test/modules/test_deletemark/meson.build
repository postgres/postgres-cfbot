# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_deletemark_sources = files(
  'test_deletemark.c',
)

if host_system == 'windows'
  test_deletemark_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_deletemark',
    '--FILEDESC', 'test_deletemark - synthetic exerciser for nbtree delete-marking',])
endif

test_deletemark = shared_module('test_deletemark',
  test_deletemark_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_deletemark

test_install_data += files(
  'test_deletemark.control',
  'test_deletemark--1.0.sql',
)

tests += {
  'name': 'test_deletemark',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_deletemark',
    ],
  },
  'tap': {
    'tests': [
      't/001_deletemark_recovery.pl',
    ],
  },
}

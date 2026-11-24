# Copyright (c) 2026, PostgreSQL Global Development Group

test_tablespace_options_sources = files(
  'test_tablespace_options.c',
)

if host_system == 'windows'
  test_tablespace_options_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_tablespace_options',
    '--FILEDESC', 'test_tablespace_options - test extension-defined tablespace options',])
endif

test_tablespace_options = shared_module('test_tablespace_options',
  test_tablespace_options_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_tablespace_options

tests += {
  'name': 'test_tablespace_options',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_tablespace_options.pl',
      't/002_pg_upgrade.pl',
    ],
  },
}

# Copyright (c) 2026, PostgreSQL Global Development Group

test_tidbitmap_sources = files(
  'test_tidbitmap.c',
)

if host_system == 'windows'
  test_tidbitmap_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_tidbitmap',
    '--FILEDESC', 'test_tidbitmap - test code for src/backend/nodes/tidbitmap.c',])
endif

test_tidbitmap = shared_module('test_tidbitmap',
  test_tidbitmap_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_tidbitmap

test_install_data += files(
  'test_tidbitmap.control',
  'test_tidbitmap--1.0.sql',
)

tests += {
  'name': 'test_tidbitmap',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_tidbitmap',
    ],
  },
}

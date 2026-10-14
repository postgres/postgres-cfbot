# Copyright (c) 2026, PostgreSQL Global Development Group

test_extendplan_sources = files(
  'test_extendplan.c',
)

if host_system == 'windows'
  test_extendplan_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_extendplan',
    '--FILEDESC', 'test_extendplan - test module for planner extension state',])
endif

test_extendplan = shared_module('test_extendplan',
  test_extendplan_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_extendplan

test_install_data += files(
  'test_extendplan.control',
  'test_extendplan--1.0.sql',
)

tests += {
  'name': 'test_extendplan',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_extendplan',
    ],
  },
}

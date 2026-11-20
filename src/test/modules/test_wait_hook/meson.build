# Copyright (c) 2026, PostgreSQL Global Development Group

test_wait_hook_sources = files(
  'test_wait_hook.c',
)

if host_system == 'windows'
  test_wait_hook_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_wait_hook',
    '--FILEDESC', 'test_wait_hook - test code for the wait event hook contract',])
endif

test_wait_hook = shared_module('test_wait_hook',
  test_wait_hook_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_wait_hook

test_install_data += files(
  'test_wait_hook.control',
  'test_wait_hook--1.0.sql',
)

tests += {
  'name': 'test_wait_hook',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_wait_hook',
    ],
  },
}

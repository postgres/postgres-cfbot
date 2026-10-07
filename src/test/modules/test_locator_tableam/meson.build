# Copyright (c) 2026, PostgreSQL Global Development Group

test_locator_tableam_sources = files(
  'test_locator_tableam.c',
)

if host_system == 'windows'
  test_locator_tableam_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_locator_tableam',
    '--FILEDESC', 'test_locator_tableam - test table AMs for the locator contract',])
endif

test_locator_tableam = shared_module('test_locator_tableam',
  test_locator_tableam_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_locator_tableam

test_install_data += files(
  'test_locator_tableam.control',
  'test_locator_tableam--1.0.sql',
)

tests += {
  'name': 'test_locator_tableam',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'noretain',
      'width_check',
      'max_offset',
      'bitmap_hooks',
    ],
  },
}

# Copyright (c) 2025, PostgreSQL Global Development Group

test_copy_custom_format_sources = files(
'test_copy_custom_format.c',
)

if host_system == 'windows'
  test_copy_custom_format_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_copy_custom_format',
    '--FILEDESC', 'test_copy_custom_format - test custom COPY FORMAT',])
endif

test_copy_custom_format = shared_module('test_copy_custom_format',
  test_copy_custom_format_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_copy_custom_format

tests += {
  'name': 'test_copy_custom_format',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_copy_custom_format',
    ],
    # Disabled because these tests require
    # "shared_preload_libraries=test_custom_copy_format", which typical
    # runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
}

# Copyright (c) 2026, PostgreSQL Global Development Group

test_upper_paths_hook_sources = files(
  'test_upper_paths_hook.c',
)

if host_system == 'windows'
  test_upper_paths_hook_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_upper_paths_hook',
    '--FILEDESC', 'test_upper_paths_hook - test create_upper_paths_hook',])
endif

test_upper_paths_hook = shared_module('test_upper_paths_hook',
  test_upper_paths_hook_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_upper_paths_hook

tests += {
  'name': 'test_upper_paths_hook',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_upper_paths_hook',
    ],
  },
}

# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_logicalmsg_hooks_sources = files(
  'test_logicalmsg_hooks.c',
)

if host_system == 'windows'
  test_logicalmsg_hooks_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_logicalmsg_hooks',
    '--FILEDESC', 'test_logicalmsg_hooks - test logical message hooks',])
endif

test_logicalmsg_hooks = shared_module('test_logicalmsg_hooks',
  test_logicalmsg_hooks_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_logicalmsg_hooks

tests += {
  'name': 'test_logicalmsg_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}

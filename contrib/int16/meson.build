# Copyright (c) 2022-2026, PostgreSQL Global Development Group

int16_sources = files(
    'int16.c',
    'int16_numeric.c',
    'int16_funcs.c',
)

if host_system == 'windows'
  int16_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'int16',
    '--FILEDESC', 'int16 - 128-bit signed integer data type',
  ])
endif

int16 = shared_module('int16',
    int16_sources,
    kwargs: contrib_mod_args,
)
contrib_targets += int16

install_data(
    'int16--1.0.sql',
    'int16.control',
    kwargs: contrib_data_args,
)

# Regression tests
tests += {
  'name': 'int16',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'init',
      'int16',
      'int16_overflow',
      'int16_numeric',
      'int16_funcs',
    ]
  },
}

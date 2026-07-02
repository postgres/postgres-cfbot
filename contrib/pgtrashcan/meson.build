# Copyright (c) 2026, PostgreSQL Global Development Group

pgtrashcan_sources = files(
  'pgtrashcan.c',
)

if host_system == 'windows'
  pgtrashcan_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgtrashcan',
    '--FILEDESC', 'pgtrashcan - intercept DROP TABLE and move tables to a trash schema',])
endif

pgtrashcan = shared_module('pgtrashcan',
  pgtrashcan_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pgtrashcan

install_data(
  'pgtrashcan--1.0.sql',
  'pgtrashcan.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pgtrashcan',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pgtrashcan',
    ],
  },
}

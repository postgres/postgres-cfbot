# Copyright (c) 2026, PostgreSQL Global Development Group

copy_fixed_sources = files(
  'copy_fixed.c',
)

if host_system == 'windows'
  copy_fixed_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'copy_fixed',
    '--FILEDESC', 'copy_fixed - fixed-width COPY format',])
endif

copy_fixed = shared_module('copy_fixed',
  copy_fixed_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += copy_fixed

tests += {
  'name': 'copy_fixed',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'copy_fixed',
    ],
  },
  'tap': {
    'tests': [
      't/001_encoding.pl',
    ],
  },
}

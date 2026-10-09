# Copyright (c) 2026, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

tests += {
  'name': 'detoast_reuse',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'detoast_reuse',
    ],
    # The injection points are cluster-wide, so disable installcheck
    'runningcheck': false,
  },
}

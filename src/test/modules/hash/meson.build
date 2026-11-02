# Copyright (c) 2022-2026, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

tests += {
  'name': 'hash',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'hash_deferred_count',
      'hash_incomplete_splits',
    ],
  },
  'tap': {
    'tests': [
      't/001_concurrent_inserts.pl',
    ],
  },
}

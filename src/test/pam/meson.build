# Copyright (c) 2022-2026, PostgreSQL Global Development Group

tests += {
  'name': 'pam',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_auth.pl',
    ],
    'env': {
      'with_pam': pam.found() ? 'yes' : 'no',
    },
  },
}


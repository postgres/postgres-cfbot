# Copyright (c) 2026, PostgreSQL Global Development Group

tests += {
  'name': 'test_progress',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
      'PROGRESS_H': meson.project_source_root() / 'src/include/commands/progress.h',
      'SYSTEM_VIEWS_SQL': meson.project_source_root() / 'src/backend/catalog/system_views.sql',
      'MONITORING_SGML': meson.project_source_root() / 'doc/src/sgml/monitoring.sgml',
    },
    'tests': [
      't/001_progress.pl',
      't/002_doc_phases.pl',
    ],
    # The test reads the server log, which is cluster-wide.
    'runningcheck': false,
  },
}

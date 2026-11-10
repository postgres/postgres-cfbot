# Copyright (c) 2026, PostgreSQL Global Development Group

test_wait_totals_sources = files(
  'test_wait_totals.c',
)

if host_system == 'windows'
  test_wait_totals_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_wait_totals',
    '--FILEDESC', 'test_wait_totals - producer for the pg_wait_event_tracing totals tests',])
endif

test_wait_totals = shared_module('test_wait_totals',
  test_wait_totals_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_wait_totals

test_install_data += files(
  'test_wait_totals.control',
  'test_wait_totals--1.0.sql',
)

tests += {
  'name': 'test_wait_totals',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_wait_totals',
    ],
  },
  'tap': {
    'tests': [
      't/001_publication.pl',
      't/002_exit.pl',
      't/003_capture_reset.pl',
      't/004_slots_servers.pl',
      't/005_dictionary.pl',
      't/006_coherence.pl',
      't/007_restart.pl',
      't/008_transactions.pl',
      't/009_reset.pl',
      't/010_report.pl',
    ],
  },
}

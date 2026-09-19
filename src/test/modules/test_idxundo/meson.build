# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_idxundo_sources = files(
  'test_idxundo.c',
)

if host_system == 'windows'
  test_idxundo_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_idxundo',
    '--FILEDESC', 'test_idxundo - exercise index UNDO',])
endif

test_idxundo = shared_module('test_idxundo',
  test_idxundo_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_idxundo

test_install_data += files(
  'test_idxundo.control',
  'test_idxundo--1.0.sql',
)

tests += {
  'name': 'test_idxundo',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_idxundo',
    ],
    # The end-to-end tests read the index back and verify it with amcheck.
    'regress_args': ['--load-extension=amcheck', '--load-extension=pageinspect',
                     '--temp-config', files('test_idxundo.conf')],
    'runningcheck': false,
  },
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_idxundo_crash.pl',
      't/002_idxundo_abort_threshold.pl',
      't/003_idxundo_apply_failure.pl',
      't/004_idxundo_apply_order.pl',
      't/005_idxundo_atm_retention.pl',
      't/006_idxundo_revert_retry.pl',
      't/007_idxundo_2pc_database.pl',
      't/008_idxundo_revert_liveness.pl',
      't/009_idxundo_recovery_first_batch.pl',
      't/011_idxundo_wal_consistency.pl',
    ],
  },
}

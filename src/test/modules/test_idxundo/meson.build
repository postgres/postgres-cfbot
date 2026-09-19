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
    'regress_args': ['--load-extension=amcheck'],
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
      't/013_idxundo_revert_error_after_checkpoint.pl',
      't/014_idxundo_revert_interrupt.pl',
      't/015_idxundo_inline_abort_ctx.pl',
      't/016_idxundo_round_robin.pl',
      't/017_idxundo_launcher_dropped_db.pl',
      't/019_idxundo_bulk_fetch_stale_log.pl',
      't/020_idxundo_revert_retry_table_full.pl',
      't/021_idxundo_launcher_register_fail.pl',
      't/022_idxundo_revert_template_name.pl',
    ],
  },
}

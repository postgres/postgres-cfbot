# Copyright (c) 2024-2026, PostgreSQL Global Development Group

test_fileops_sources = files(
  'test_fileops.c',
)

if host_system == 'windows'
  test_fileops_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_fileops',
    '--FILEDESC', 'test_fileops - regression testing of FILEOPS subsystem',])
endif

test_fileops = shared_module('test_fileops',
  test_fileops_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_fileops

test_install_data += files(
  'test_fileops.control',
  'test_fileops--1.0.sql',
)

tests += {
  'name': 'test_fileops',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_fileops',
    ],
  },
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_fileops_apply_failure.pl',
      't/002_fileops_large_rollback.pl',
      't/003_fileops_atm_reapply.pl',
      't/004_fileops_fs_errors.pl',
      't/005_fileops_checkpoint_loser.pl',
      't/006_fileops_rollback_prepared_crash.pl',
      't/007_fileops_commit_crash.pl',
      't/008_fileops_standby_abort.pl',
      't/009_fileops_reuse.pl',
      't/010_fileops_abort_redo.pl',
      't/011_fileops_undo_ownership.pl',
      't/012_fileops_contracts.pl',
      't/013_fileops_loser_reuse.pl',
      't/014_fileops_loser_ownership.pl',
      't/015_fileops_promote_loser.pl',
      't/016_fileops_inverse_fsync.pl',
      't/017_fileops_inverse_fsync_batch.pl',
      't/018_fileops_rename.pl',
      't/019_fileops_logical_revert_fsync_batch.pl',
      't/020_fileops_write_stat_error.pl',
    ],
  },
}

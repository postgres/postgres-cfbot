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
      't/007_fileops_commit_crash.pl',
    ],
  },
}

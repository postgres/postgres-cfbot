# Copyright (c) 2022-2026, PostgreSQL Global Development Group

libpq_test_deps = []

libpq_uri_regress_sources = files(
  'libpq_uri_regress.c',
)

if host_system == 'windows'
  libpq_uri_regress_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libpq_uri_regress',
    '--FILEDESC', 'libpq test program',])
endif

libpq_test_deps += executable('libpq_uri_regress',
  libpq_uri_regress_sources,
  dependencies: [frontend_no_fe_utils_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  }
)


libpq_testclient_sources = files(
  'libpq_testclient.c',
)

libpq_testclient_deps = [frontend_no_fe_utils_code, libpq]
if zstd.found()
  libpq_testclient_deps += zstd
endif

if host_system == 'windows'
  libpq_testclient_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libpq_testclient',
    '--FILEDESC', 'libpq test program',])
endif

libpq_test_deps += executable('libpq_testclient',
  libpq_testclient_sources,
  include_directories: [libpq_inc],
  dependencies: libpq_testclient_deps,
  kwargs: default_bin_args + {
    'install': false,
  }
)

testprep_targets += libpq_test_deps

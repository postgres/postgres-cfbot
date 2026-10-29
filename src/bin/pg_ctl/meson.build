# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_ctl_sources = files(
  'pg_ctl.c',
)

if host_system == 'windows'
  pg_ctl_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_ctl',
    '--FILEDESC', 'pg_ctl - starts/stops/restarts the PostgreSQL server',])
endif

pg_ctl = executable('pg_ctl',
  pg_ctl_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
bin_targets += pg_ctl

socket_test_deps = []
socket_test_env = {}
if host_system == 'windows'
  subdir('t')
  socket_test_deps += socket_test_helper
  socket_test_env = {'PG_CTL_SOCKET_HELPER': socket_test_helper.full_path()}
endif

tests += {
  'name': 'pg_ctl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'deps': socket_test_deps,
    'env': socket_test_env,
    'tests': [
      't/001_start_stop.pl',
      't/002_status.pl',
      't/003_promote.pl',
      't/004_logrotate.pl',
      't/005_socket_handle_inheritance.pl',
    ],
  },
}

subdir('po', if_found: libintl)

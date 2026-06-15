# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# Generate the errcode-name lookup header from the in-tree errcodes.txt.
pg_stat_log_errcodes = custom_target('pg_stat_log_errcodes',
  input: '../../src/backend/utils/errcodes.txt',
  output: 'pg_stat_log_errcodes.h',
  command: [
    perl, files('generate-errcode-names.pl'), '--outfile', '@OUTPUT@', '@INPUT@',
  ],
)
generated_sources += pg_stat_log_errcodes

pg_stat_log_sources = files(
  'pg_stat_log.c',
)
pg_stat_log_sources += pg_stat_log_errcodes

if host_system == 'windows'
  pg_stat_log_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_stat_log',
    '--FILEDESC', 'pg_stat_log - cumulative statistics about log messages',])
endif

pg_stat_log = shared_module('pg_stat_log',
  pg_stat_log_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)
contrib_targets += pg_stat_log

install_data(
  'pg_stat_log.control',
  'pg_stat_log--0.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_stat_log',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_stat_log',
    ],
    'regress_args': ['--temp-config', files('pg_stat_log.conf')],
    # Disabled because these tests require
    # "shared_preload_libraries=pg_stat_log", which typical runningcheck
    # users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
  'tap': {
    'tests': [
      't/001_pg_stat_log.pl',
    ],
  },
}

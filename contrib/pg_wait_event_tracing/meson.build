# Copyright (c) 2026, PostgreSQL Global Development Group

pg_wait_event_tracing_sources = files(
  'pg_wait_event_tracing.c',
)

if host_system == 'windows'
  pg_wait_event_tracing_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_wait_event_tracing',
    '--FILEDESC', 'pg_wait_event_tracing - statistics and trace collection for explicitly instrumented wait events',])
endif

pg_wait_event_tracing = shared_module('pg_wait_event_tracing',
  pg_wait_event_tracing_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_wait_event_tracing

install_data(
  'pg_wait_event_tracing--1.0.sql',
  'pg_wait_event_tracing.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_wait_event_tracing',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_wait_event_tracing',
      'pg_wait_event_tracing_trace',
    ],
    'regress_args': ['--temp-config', files('pg_wait_event_tracing.conf')],
    # Needs shared_preload_libraries, which typical runningcheck users do
    # not have.
    'runningcheck': false,
  },
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_memory.pl',
      't/002_ownership.pl',
      't/003_reset_acl.pl',
      't/004_reset_race.pl',
      't/005_orphan_reuse.pl',
      't/006_server_processes.pl',
      't/007_lazy_hooks.pl',
      't/010_trace_seqlock.pl',
      't/011_trace_wrap.pl',
      't/012_trace_markers.pl',
      't/013_deferred_flush.pl',
    ],
  },
}

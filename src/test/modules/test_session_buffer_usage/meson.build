# Copyright (c) 2026, PostgreSQL Global Development Group

test_session_buffer_usage_sources = files(
  'test_session_buffer_usage.c',
)

if host_system == 'windows'
  test_session_buffer_usage_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_session_buffer_usage',
    '--FILEDESC', 'test_session_buffer_usage - show buffer usage statistics for the current session',])
endif

test_session_buffer_usage = shared_module('test_session_buffer_usage',
  test_session_buffer_usage_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_session_buffer_usage

test_install_data += files(
  'test_session_buffer_usage.control',
  'test_session_buffer_usage--1.0.sql',
)

tests += {
  'name': 'test_session_buffer_usage',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_session_buffer_usage',
    ],
  },
}

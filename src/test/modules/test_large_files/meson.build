# src/test/modules/test_large_files/meson.build

test_large_files_sources = files(
  'test_large_files.c',
)

if host_system == 'windows'
  test_large_files_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_large_files',
    '--FILEDESC', 'test_large_files - test module for large file I/O',])
endif

test_large_files = shared_module('test_large_files',
  test_large_files_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_large_files

test_install_data += files(
  'test_large_files.control',
  'test_large_files--1.0.sql',
)

tests += {
  'name': 'test_large_files',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_large_files',
    ],
    # Don't run on installcheck - only during regular check
    'regress_args': ['--no-locale'],
    'runningcheck': false,
  },
}

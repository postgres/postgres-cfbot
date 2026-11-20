# Copyright (c) 2026, PostgreSQL Global Development Group

test_xlogreader_sources = files(
  'test_xlogreader.c',
)
test_xlogreader_sources += xlogreader_sources

if host_system == 'windows'
  test_xlogreader_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_xlogreader',
    '--FILEDESC', 'standalone XLogReader decoder tester',
  ])
endif

test_xlogreader = executable('test_xlogreader',
  test_xlogreader_sources,
  dependencies: [frontend_code, lz4, zstd],
  c_args: ['-DFRONTEND'],
  kwargs: default_bin_args + {
    'install': false,
  },
)

testprep_targets += test_xlogreader

tests += {
  'name': 'test_xlogreader',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_hole_geometry.pl',
    ],
    'deps': [
      test_xlogreader,
    ],
  },
}

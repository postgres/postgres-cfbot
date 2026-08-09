# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_bloom_customscan_sources = files(
  'test_bloom_customscan.c',
)

if host_system == 'windows'
  test_bloom_customscan_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_bloom_customscan',
    '--FILEDESC', 'test_bloom_customscan - CustomScan consuming a hashjoin bloom filter',])
endif

test_bloom_customscan = shared_module('test_bloom_customscan',
  test_bloom_customscan_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_bloom_customscan

test_install_data += files(
  'test_bloom_customscan.control',
  'test_bloom_customscan--1.0.sql',
)

tests += {
  'name': 'test_bloom_customscan',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_bloom_customscan',
    ],
  },
}

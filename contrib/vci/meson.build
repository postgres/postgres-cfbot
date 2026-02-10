# Copyright (c) 2026, PostgreSQL Global Development Group

subdir('executor')
subdir('storage')
subdir('utils')


vci_sources = files(
        'vci_main.c',
        'vci_read_guc.c',
        'vci_shmem.c',
        'vci_supported_funcs.c',
        'vci_supported_types.c',
)

vci_sources += vci_executor_sources

vci_sources += vci_storage_sources

vci_sources += vci_utils_sources

if host_system == 'windows'
  vci_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vci',
    '--FILEDESC', 'vci - vertical clustered index',])
endif

if host_system == 'solaris'
   ldflags += ['-lc -lkstat']
endif

vci_cflags = []

vci_cflags += var_cflags_sl

vci = shared_module('vci',
  vci_sources,
  c_args : vci_cflags,
  include_directories : include_directories('../../contrib/vci/include'),
  link_args: ldflags,
  kwargs: contrib_mod_args,
)

install_data(
  'vci.control',
  'vci--1.0.sql',
  kwargs:contrib_data_args,
)

tests += {
  'name': 'vci',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'vci',
      'bugs',
    ],
    'regress_args': ['--temp-config', files('vci.conf')],

	# runningcheck is disabled because a running instance needs to have
	# "shared_preload_libraries=vci", but typical runnigcheck users
	# (e.g. build farm clients) won't have that setting so they would fail.
	# Note: This is copied from contrib/pg_stat_statements/meson.build
	'runningcheck' : false,
  }
}

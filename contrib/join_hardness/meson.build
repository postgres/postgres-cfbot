# Copyright (c) 2022-2026, PostgreSQL Global Development Group

join_hardness_sources = files(
  'join_hardness.c',
)

if host_system == 'windows'
  join_hardness_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'join_hardness',
    '--FILEDESC', 'join_hardness - join search hardness estimator',])
endif

join_hardness = shared_module('join_hardness',
  join_hardness_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += join_hardness

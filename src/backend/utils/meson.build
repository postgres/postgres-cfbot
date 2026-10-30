# Copyright (c) 2022-2026, PostgreSQL Global Development Group

install_data('errcodes.txt',
  install_dir: dir_data,
)

subdir('activity')
subdir('adt')
subdir('cache')
subdir('error')
subdir('fmgr')
subdir('hash')
subdir('init')
subdir('mb')
subdir('misc')
subdir('mmgr')
subdir('resowner')
subdir('sort')
subdir('time')

# ETW provider definition and lifecycle, only built with ETW support
if etw.found()
  backend_sources += files('pg_trace_etw.c')
endif

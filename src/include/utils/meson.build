# Copyright (c) 2022-2026, PostgreSQL Global Development Group

wait_event_output = ['wait_event_types.h', 'pgstat_wait_event.c', 'wait_event_funcs_data.c']
wait_event_target = custom_target('wait_event_names',
  input: files('../../backend/utils/activity/wait_event_names.txt'),
  output: wait_event_output,
  command: [
    perl, files('../../backend/utils/activity/generate-wait_event_types.pl'),
    '--outdir', '@OUTDIR@', '--code',
    '@INPUT@'
  ],
  build_by_default: true,
  install: true,
  install_dir: [dir_include_server / 'utils', false, false],
)

wait_event_types_h = wait_event_target[0]
generated_backend_headers += wait_event_types_h

errcodes = custom_target('errcodes',
  input: files('../../backend/utils/errcodes.txt'),
  output: ['errcodes.h'],
  command: [
    perl, files('../../backend/utils/generate-errcodes.pl'),
    '--outfile', '@OUTPUT@',
    '@INPUT@',
  ],
  install: true,
  install_dir: dir_include_server / 'utils',
)
generated_headers += errcodes

guc_tables = custom_target('guc_tables',
  input: files('../../backend/utils/misc/guc_parameters.dat'),
  output: ['guc_tables.inc.c'],
  depend_files: catalog_pm,
  command: [perl, files('../../backend/utils/misc/gen_guc_tables.pl'), '@INPUT@', '@OUTPUT@'])
generated_headers += guc_tables

# Builds without ETW still check probes_etw.txt against probes.d before
# generating probes.h, so that a probe change is caught on the platform
# where it is made.
probes_etw_check = []
if not etw.found()
  probes_etw_check = custom_target('probes_etw_check',
    input: files(
      '../../backend/utils/probes.d',
      '../../backend/utils/probes_etw.txt'),
    output: 'probes_etw_check.stamp',
    capture: true,
    command: [perl, files('../../backend/utils/Gen_etw_probes.pl'),
      '--check', '@INPUT0@', '@INPUT1@'],
  )
endif

if dtrace.found()
  probes_tmp = custom_target('probes.h.tmp',
    input: files('../../backend/utils/probes.d'),
    output: 'probes.h.tmp',
    command: [dtrace, '-C', '-h', '-s', '@INPUT@', '-o', '@OUTPUT@'],
  )
  probes = custom_target('probes.h',
    input: probes_tmp,
    output: 'probes.h',
    capture: true,
    command: [sed, '-f', files('../../backend/utils/postprocess_dtrace.sed'), '@INPUT@'],
    depends: probes_etw_check,
    install: true,
    install_dir: dir_include_server / 'utils',
  )
elif etw.found()
  # Generate TraceLogging macros from probes.d (argument types) and
  # probes_etw.txt (keyword, level and field names).
  probes = custom_target('probes.h',
    input: files(
      '../../backend/utils/probes.d',
      '../../backend/utils/probes_etw.txt'),
    output: 'probes.h',
    capture: true,
    command: [perl, files('../../backend/utils/Gen_etw_probes.pl'),
      '@INPUT0@', '@INPUT1@'],
    install: true,
    install_dir: dir_include_server / 'utils',
  )
else
  probes = custom_target('probes.h',
    input: files('../../backend/utils/probes.d'),
    output: 'probes.h',
    capture: true,
    command: [perl, '-n', files('../../backend/utils/Gen_dummy_probes.pl'), '@INPUT@'],
    depends: probes_etw_check,
    install: true,
    install_dir: dir_include_server / 'utils',
  )
endif

generated_backend_headers += probes

fmgrtab_output = ['fmgroids.h', 'fmgrprotos.h', 'fmgrtab.c']
fmgrtab_target = custom_target('fmgrtab',
  input: '../catalog/pg_proc.dat',
  output : fmgrtab_output,
  depend_files: catalog_pm,
  command: [perl, '-I', '@SOURCE_ROOT@/src/backend/catalog/', files('../../backend/utils/Gen_fmgrtab.pl'), '--include-path=@SOURCE_ROOT@/src/include', '--output=@OUTDIR@', '@INPUT@'],
  install: true,
  install_dir: [dir_include_server / 'utils', dir_include_server / 'utils', false],
)

generated_backend_headers += fmgrtab_target[0]
generated_backend_headers += fmgrtab_target[1]

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {
  'src/backend/utils': fmgrtab_output + ['errcodes.h', 'wait_event_types.h', 'probes.h', 'fmgr-stamp'],
  'src/include/utils': ['header-stamp'],
}

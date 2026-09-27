# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# used by frontend programs like pg_waldump
rmgr_desc_sources = files(
  'atmdesc.c',
  'brindesc.c',
  'clogdesc.c',
  'committsdesc.c',
  'dbasedesc.c',
  'genericdesc.c',
  'gindesc.c',
  'gistdesc.c',
  'hashdesc.c',
  'heapdesc.c',
  'logicalmsgdesc.c',
  'mxactdesc.c',
  'nbtdesc.c',
  'relmapdesc.c',
  'replorigindesc.c',
  'rmgrdesc_utils.c',
  'seqdesc.c',
  'smgrdesc.c',
  'spgdesc.c',
  'standbydesc.c',
  'tblspcdesc.c',
  'pbu_undoactiondesc.c',
  'pbu_undologdesc.c',
  'undodesc.c',
  'xactdesc.c',
  'xlogdesc.c',
)


# FLUX is always built; rmgrlist.h registers flux_desc/flux_identify

# FILEOPS is always built (USE_FILEOPS); rmgrlist.h registers
# fileops_desc/fileops_identify, so fileopsdesc.c must always be compiled or
# pg_waldump fails to link.  The autoconf build picks this up via its
# *desc*.c wildcard; meson needs it listed explicitly.
rmgr_desc_sources += files('fileopsdesc.c')

backend_sources += rmgr_desc_sources

#-------------------------------------------------------------------------
#
# Makefile for src/test/pam
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/pam/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/pam
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export with_pam

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check


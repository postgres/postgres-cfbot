#-------------------------------------------------------------------------
#
# Makefile for src/test/protocol
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/protocol/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/protocol
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export OPENSSL with_ssl

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check

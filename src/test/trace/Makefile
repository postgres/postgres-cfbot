#
# Makefile for trace tests
#

PGFILEDESC = "pg_trace_regress - trace test driver"
PGAPPICON = win32

subdir = src/test/trace
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) \
	-I$(srcdir)/../regress $(CPPFLAGS)

all: pg_trace_regress$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_trace_regress$(X) '$(DESTDIR)$(pgxsdir)/$(subdir)/pg_trace_regress$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/pg_trace_regress$(X)'

submake-regress:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress.o

pg_regress.o: | submake-regress
	rm -f $@ && $(LN_S) $(top_builddir)/src/test/regress/pg_regress.o .

pg_trace_regress$(X): trace_main.o pg_regress.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

clean distclean:
	rm -f pg_trace_regress$(X) trace_main.o pg_regress.o
	rm -rf $(pg_regress_clean_files)

installcheck: all
	$(pg_trace_regress_installcheck) --schedule=$(srcdir)/trace_schedule

check: all
	$(pg_trace_regress_check) --schedule=$(srcdir)/trace_schedule

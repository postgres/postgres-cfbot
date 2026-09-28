# src/test/modules/test_xlogreader/Makefile

PGFILEDESC = "standalone XLogReader decoder tester"
PGAPPICON = win32

TAP_TESTS = 1

OBJS = \
	$(WIN32RES) \
	test_xlogreader.o \
	xlogreader.o

EXTRA_CLEAN = test_xlogreader$(X) xlogreader.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_xlogreader
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)

all: test_xlogreader$(X)

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

test_xlogreader$(X): $(OBJS)
	$(CC) $(CFLAGS) $^ $(PG_LIBS_INTERNAL) $(LDFLAGS) $(LDFLAGS_EX) $(PG_LIBS) $(LIBS) -o $@

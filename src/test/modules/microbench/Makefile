# src/test/modules/microbench/Makefile

PGFILEDESC = "microbench - suite of functions for micro-benchmarks"

EXTENSION = microbench
MODULE_big = microbench

# One subdir per benchmark: <name>/bench.c, <name>/install.sql, <name>/query.sql
MICROBENCH_TESTS := $(sort $(patsubst %/bench.c,%,$(wildcard */bench.c)))
SQL_FRAGMENTS := $(addsuffix /install.sql,$(MICROBENCH_TESTS))

MICROBENCH_SQL_HEAD = microbench-head--1.0.sql
MICROBENCH_SQL_BUILT = microbench--1.0.sql

OBJS = \
	$(WIN32RES) \
	multiprocessing.o \
	$(addsuffix /bench.o,$(MICROBENCH_TESTS))

# Generated extension script; must be DATA_built so all/install depend on it.
DATA_built = $(MICROBENCH_SQL_BUILT)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/microbench
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

# Link the module against an explicit postgres binary when requested, e.g.
# make install MICROBENCH_POSTGRES=/path/to/bin/postgres
ifdef MICROBENCH_POSTGRES
BE_DLLLIBS := -bundle_loader $(MICROBENCH_POSTGRES)
endif
endif

$(MICROBENCH_SQL_BUILT): $(addprefix $(srcdir)/,$(MICROBENCH_SQL_HEAD)) \
		$(addprefix $(srcdir)/,$(SQL_FRAGMENTS))
	cat $^ > $@

# Incremental compile of a single benchmark folder, e.g. make lwlock
$(MICROBENCH_TESTS): %: %/bench.o

%/bench.o: %/bench.c randomize.h timing-magic.h multiprocessing.h
	$(COMPILE.c) -I. -o $@ $<

.PHONY: run tests list $(MICROBENCH_TESTS)

tests: $(MICROBENCH_TESTS)

list:
	@echo $(MICROBENCH_TESTS)

run:
ifndef TEST
	$(error TEST is required, e.g. make run TEST=lwlock)
endif
	@test -d '$(TEST)' || (echo "unknown test: $(TEST)" && exit 1)
	$(SHELL) '$(srcdir)/scripts/run-test.sh' '$(TEST)'

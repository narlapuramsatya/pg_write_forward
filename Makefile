# pg_write_forward - PGXS-based standalone extension Makefile.
#
# Build:
#   make PG_CONFIG=/path/to/pg_config
# Install:
#   make PG_CONFIG=/path/to/pg_config install
# Test (requires Test::More + IPC::Run; brings up a primary+standby pair):
#   make PG_CONFIG=/path/to/pg_config check

MODULE_big = pg_write_forward
EXTENSION  = pg_write_forward
DATA       = sql/pg_write_forward--1.0.sql \
             sql/pg_write_forward--1.1.sql \
             sql/pg_write_forward--1.0--1.1.sql
PGFILEDESC = "pg_write_forward - forward writes from a hot standby to the primary"

OBJS = src/pg_write_forward.o $(WIN32RES)

# Need libpq to talk to the primary.
PG_CPPFLAGS         = -I$(shell $(PG_CONFIG) --includedir)
SHLIB_LINK_INTERNAL = $(libpq)
SHLIB_LINK         += -L$(shell $(PG_CONFIG) --libdir) -lpq

# installcheck (the regress-style one) makes no sense (no regress dir), but
# we still want `make installcheck` to run our TAP tests.
TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

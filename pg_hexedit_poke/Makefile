# pg_hexedit_poke - dev-only shared-buffer page poking for pg_hexedit's live
# bridge.  Standard PGXS extension build.

MODULE_big = pg_hexedit_poke
OBJS = pg_hexedit_poke.o

EXTENSION = pg_hexedit_poke
DATA = pg_hexedit_poke--1.0.sql
PGFILEDESC = "pg_hexedit_poke - write raw bytes into a shared-buffer page (dev only)"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

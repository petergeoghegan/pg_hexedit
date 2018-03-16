#-------------------------------------------------------------------------
#
# Makefile for pg_hexedit
#
# Copyright (c) 2017-2018, VMware, Inc.
# Copyright (c) 2002-2010, Red Hat, Inc.
# Copyright (c) 2011-2018, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_hexedit - emits descriptive XML tag format for PostgreSQL relation files"
PGAPPICON=win32
HEXEDIT_VERSION = 0.1

PG_CONFIG = pg_config
PGSQL_CFLAGS = $(shell $(PG_CONFIG) --cflags)
PGSQL_INCLUDE_DIR = $(shell $(PG_CONFIG) --includedir-server)
PGSQL_LDFLAGS = $(shell $(PG_CONFIG) --ldflags)
PGSQL_LIB_DIR = $(shell $(PG_CONFIG) --libdir)
PGSQL_BIN_DIR = $(shell $(PG_CONFIG) --bindir)

DISTFILES= README.md Makefile pg_hexedit.c pg_relfilenodemapdata.c

all: pg_hexedit pg_relfilenodemapdata

pg_hexedit: pg_hexedit.o
	${CC} ${PGSQL_LDFLAGS} ${LDFLAGS} -o pg_hexedit pg_hexedit.o -L${PGSQL_LIB_DIR} -lpgcommon

pg_relfilenodemapdata: pg_relfilenodemapdata.o
	${CC} ${PGSQL_LDFLAGS} ${LDFLAGS} -o pg_relfilenodemapdata pg_relfilenodemapdata.o -L${PGSQL_LIB_DIR} -lpgport

pg_hexedit.o: pg_hexedit.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_hexedit.c -c

pg_relfilenodemapdata.o: pg_relfilenodemapdata.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_relfilenodemapdata.c -c

dist:
	rm -rf pg_hexedit-${HEXEDIT_VERSION} pg_hexedit-${HEXEDIT_VERSION}.tar.gz
	mkdir pg_hexedit-${HEXEDIT_VERSION}
	cp -p ${DISTFILES} pg_hexedit-${HEXEDIT_VERSION}
	tar cfz pg_hexedit-${HEXEDIT_VERSION}.tar.gz pg_hexedit-${HEXEDIT_VERSION}
	rm -rf pg_hexedit-${HEXEDIT_VERSION}

install:
	mkdir -p $(DESTDIR)$(PGSQL_BIN_DIR)
	install pg_hexedit $(DESTDIR)$(PGSQL_BIN_DIR)
	install pg_relfilenodemapdata $(DESTDIR)$(PGSQL_BIN_DIR)

uninstall:
	rm -f '$(DESTDIR)$(PGSQL_BIN_DIR)/pg_hexedit$(X)'
	rm -f '$(DESTDIR)$(PGSQL_BIN_DIR)/pg_relfilenodemapdata$(X)'

clean:
	rm -f *.o pg_hexedit pg_relfilenodemapdata

distclean:
	rm -f *.o pg_hexedit pg_relfilenodemapdata
	rm -rf pg_hexedit-${HEXEDIT_VERSION} pg_hexedit-${HEXEDIT_VERSION}.tar.gz

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

DISTFILES= README.md Makefile pg_hexedit.c pg_filenodemapdata.c
TESTFILES= t/1249 t/2685 t/expected_attributes.tags \
	t/expected_attributes_idx.tags t/expected_no_attributes.tags \
	t/expected_no_attributes_idx.tags t/test_pg_hexedit

all: pg_hexedit pg_filenodemapdata

pg_hexedit: pg_hexedit.o
	${CC} ${PGSQL_LDFLAGS} ${LDFLAGS} -o pg_hexedit pg_hexedit.o -L${PGSQL_LIB_DIR} -lpgcommon

pg_filenodemapdata: pg_filenodemapdata.o
	${CC} ${PGSQL_LDFLAGS} ${LDFLAGS} -o pg_filenodemapdata pg_filenodemapdata.o -L${PGSQL_LIB_DIR} -lpgport

pg_hexedit.o: pg_hexedit.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_hexedit.c -c

pg_filenodemapdata.o: pg_filenodemapdata.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_filenodemapdata.c -c

check:
	t/test_pg_hexedit

dist:
	rm -rf pg_hexedit-${HEXEDIT_VERSION} pg_hexedit-${HEXEDIT_VERSION}.tar.gz
	mkdir pg_hexedit-${HEXEDIT_VERSION}
	cp -p ${DISTFILES} pg_hexedit-${HEXEDIT_VERSION}
	mkdir pg_hexedit-${HEXEDIT_VERSION}/t
	cp -p ${TESTFILES} pg_hexedit-${HEXEDIT_VERSION}/t
	tar cfz pg_hexedit-${HEXEDIT_VERSION}.tar.gz pg_hexedit-${HEXEDIT_VERSION}
	rm -rf pg_hexedit-${HEXEDIT_VERSION}

install:
	mkdir -p $(DESTDIR)$(PGSQL_BIN_DIR)
	install pg_hexedit $(DESTDIR)$(PGSQL_BIN_DIR)
	install pg_filenodemapdata $(DESTDIR)$(PGSQL_BIN_DIR)

uninstall:
	rm -f '$(DESTDIR)$(PGSQL_BIN_DIR)/pg_hexedit$(X)'
	rm -f '$(DESTDIR)$(PGSQL_BIN_DIR)/pg_filenodemapdata$(X)'

clean:
	rm -f *.o pg_hexedit pg_filenodemapdata
	rm -f t/*diff
	rm -f t/output*tags

distclean:
	rm -f *.o pg_hexedit pg_filenodemapdata
	rm -f t/*diff
	rm -f t/output*tags
	rm -rf pg_hexedit-${HEXEDIT_VERSION} pg_hexedit-${HEXEDIT_VERSION}.tar.gz

# View README.md first

# note this must match version macros in pg_hexedit.c
FD_VERSION=11.0

# If working with a PG source directory, point PGSQL_INCLUDE_DIR to its
# src/include subdirectory.  If working with an installed tree, point to
# the server include subdirectory, eg /usr/local/include/postgresql/server
PG_CONFIG=pg_config
PGSQL_CFLAGS=$(shell $(PG_CONFIG) --cflags)
PGSQL_INCLUDE_DIR=$(shell $(PG_CONFIG) --includedir-server)
PGSQL_LDFLAGS=$(shell $(PG_CONFIG) --ldflags)
PGSQL_LIB_DIR=$(shell $(PG_CONFIG) --libdir)
PGSQL_BIN_DIR=$(shell $(PG_CONFIG) --bindir)

DISTFILES= README.md Makefile Makefile.contrib \
	pg_hexedit.h pg_hexedit.c

all: pg_hexedit

pg_hexedit: pg_hexedit.o
	${CC} ${PGSQL_LDFLAGS} ${LDFLAGS} -o pg_hexedit pg_hexedit.o -L${PGSQL_LIB_DIR} -lpgport

pg_hexedit.o: pg_hexedit.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_hexedit.c -c

dist:
	rm -rf pg_hexedit-${FD_VERSION} pg_hexedit-${FD_VERSION}.tar.gz
	mkdir pg_hexedit-${FD_VERSION}
	cp -p ${DISTFILES} pg_hexedit-${FD_VERSION}
	tar cfz pg_hexedit-${FD_VERSION}.tar.gz pg_hexedit-${FD_VERSION}
	rm -rf pg_hexedit-${FD_VERSION}

install: pg_hexedit
	mkdir -p $(DESTDIR)$(PGSQL_BIN_DIR)
	install pg_hexedit $(DESTDIR)$(PGSQL_BIN_DIR)

clean:
	rm -f *.o pg_hexedit

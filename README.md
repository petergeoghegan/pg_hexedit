pg_hexedit
==========

## Open PostgreSQL relation files in a hex editor with annotations

Copyright (c) 2018-2021, Crunchy Data Solutions, Inc.  
Copyright (c) 2017-2018, VMware, Inc.  
Copyright (c) 2002-2010, Red Hat, Inc.  
Copyright (c) 2011-2021, PostgreSQL Global Development Group

Author: Peter Geoghegan [`<pg@bowt.ie>`](mailto:pg@bowt.ie)  
Author (pg_filedump): Patrick Macdonald [`<patrickm@redhat.com>`](mailto:patrickm@redhat.com)

License: [GNU General Public License version 2](https://opensource.org/licenses/GPL-2.0)

[![Build Status](https://img.shields.io/cirrus/github/petergeoghegan/pg_hexedit/master)](https://cirrus-ci.com/github/petergeoghegan/pg_hexedit)

## Overview

pg_hexedit is an experimental toolkit for viewing and editing PostgreSQL
relation files -- heap, sequence, and index files (B-Tree, GiST, GIN, hash,
BRIN, and SP-GiST) -- inside the open source hex editor
[ImHex](https://imhex.werwolv.net/).  PostgreSQL versions 14+ are supported.

The pg_hexedit frontend executable is a generator for ImHex's [pattern
language](https://docs.werwolv.net/pattern-language): it emits a
self-contained `.hexpat` file that, when loaded into ImHex alongside a
relation file, annotates every page.  Convenience bash scripts automate the
whole workflow: they take the name of a table or index, connect to a database
to locate the underlying file and describe its tuples, generate the pattern,
and open ImHex.  A live mode serves pages straight out of the server's
shared_buffers instead of going through the filesystem at all.

(pg_hexedit began life as a fork of
[pg_filedump](https://wiki.postgresql.org/wiki/Pg_filedump); earlier
versions targeted the wxHexEditor hex editor.)

__CAUTION:__ Do not use pg_hexedit with a PostgreSQL data directory if you
are not prepared to have it __corrupt data__!  pg_hexedit is primarily made
available for educational purposes.  It is an *experimental* tool, originally
used for simulating corruption/corruption analysis.

## How it works

pg_hexedit does not read relation files itself.  Page interpretation happens
inside ImHex's pattern evaluator: the emitted pattern classifies each page by
its special section (size plus the "page id" bytes in the last two bytes of
the page), walks line pointers, places tuples at their `lp_off` offsets, and
decodes attribute values by honoring each tuple's own attribute count, NULL
bitmap, alignment, and varlena headers.

What the C program contributes is correctness by construction.  Every struct
layout, field offset, flag mask, and alignment rule in the emitted pattern is
computed from the PostgreSQL headers that pg_hexedit is compiled against,
using `sizeof`/`offsetof` and the real macros -- so the pattern is correct
for that PostgreSQL version and platform (`BLCKSZ`, `MAXIMUM_ALIGNOF`, and
friends included) without anything being transcribed by hand.

There are three kinds of knowledge in a generated pattern:

* *Per PostgreSQL version*: struct layouts and flag masks, from the compiled
  headers.
* *Per relation*: the tuple descriptor (attribute names, lengths, alignments,
  and type OIDs), passed with `-A`.  The convenience scripts build it from
  `pg_attribute` via SQL; nothing in the page bytes could supply it.  This is
  why there is one pattern per relation: the tuple descriptor is baked into
  the pattern as literal decoding code.
* *Static*: the traversal itself -- page-kind dispatch, line pointer walking,
  the attribute loop -- which is version-stable.

A pattern generated without `-A` is relation-independent: it annotates page
headers, line pointers, tuple headers, special sections, and metapages for
every access method, with each tuple's data area shown as one opaque region.

## What you get inside ImHex

Once a pattern is evaluated, the Pattern Data view shows one row per block,
labeled with the block number and page kind (`block 3 (nbtree level 1)`,
`block 0 (hash metapage)`, `block 167 (GIN posting tree internal)`, ...).
Within a block:

* Line pointer slots present the *tuple* as the primary object.  Each
  occupied slot row is named with the tuple's own tuple identifier
  (`tuple (0,47)`); its first child is the tuple (clicking it jumps the hex
  view there), and the line pointer itself is the last child, for
  drill-down.  Slots without storage keep plain `lp N` naming.  Note the
  tuple's own TID is not `t_ctid`, which for an updated tuple points at its
  successor version.
* Hovering any byte shows the enclosing patterns with names, values, and the
  tuple's TID as a comment -- context without visiting the Pattern Data pane.
* Status-bit fields (`pd_flags`, `t_infomask2`, `t_infomask`, index
  `t_info`, and per-AM special-section flags) are bitfields: collapsed they
  display the set flags by their grep'able names
  (`HEAP_XMIN_COMMITTED|HEAP_XMAX_INVALID`); expanded, each flag is its own
  row.  Only "base" flag bits appear; composite flags such as
  `HEAP_XMIN_FROZEN` never do.
* Collapsed tuples read `xmin=695 3 atts, ctid=(0,47)`, with FROZEN/ABORTED
  xmin and MultiXact/lock-only xmax status carried in text.  Each tuple's
  attribute walk is governed by the tuple's own attribute count, so tuples
  that predate `ALTER TABLE .. ADD COLUMN` show fewer attributes -- as they
  should.
* Varlena headers are annotated separately from their values
  (`varattrib_1b`, `va_4byte`, `va_compressed`), and TOAST pointers are
  broken into their `varatt_external` fields.
* Values are editable in place: double-click a value (say `t_xmin`), type a
  new one, save, and ImHex writes the bytes back to the file.

### The use of color

pg_hexedit uses color to convey conceptual similarities between distinct
fields: red xmin/xmax, blue item pointers, green infomasks, yellow
tuple-shape metadata, bright green special sections, pink metapage fields.  Adjacent fields that
share a color get slightly offset shades of the same hue (`t_xmin` vs
`t_xmax`, consecutive line pointers, metapage field runs), since ImHex draws
highlights without gaps between patterns; the shade alternation keeps
boundaries visible.

Line pointers are colored by state: light blue for `LP_NORMAL`, dark blue
for `LP_REDIRECT`, dark brown-red for `LP_DEAD`, and muted gray for
`LP_UNUSED`.  LP_DEAD index tuples (which retain storage until deleted) are
painted in the same dark brown-red as their line pointers.  nbtree posting
list TIDs alternate colors per entry.

Columns/attributes are dynamically assigned pastel colors hashed from the
attribute name, so indexes have the same attribute colors as the tables they
index.

## Initial setup

There are no pg_hexedit packages available.  pg_hexedit follows a simple
trunk-based development model without any formal assigned versions.  The only
way to get pg_hexedit is to build it from source yourself.  This is a
deliberate trade-off; pg_hexedit is experimental, and it's not difficult to
build from source anyway.

### Building pg_hexedit

To build pg_hexedit you will need to have a PostgreSQL source tree or
complete install tree (with include files).  PostgreSQL system packages
should provide all you need.  If you run into trouble when building against
system packages, please open a Github issue.

The Makefile builds the pg_hexedit frontend utility program.
[pg_config](https://www.postgresql.org/docs/current/app-pgconfig.html) must
be visible in your $PATH.  To build pg_hexedit from within the source
directory:

```shell
  $ which pg_config # Check which Postgres installation you'll build against:
  /code/postgresql/REL_18_STABLE/install/bin/pg_config
  $ make
```

You probably won't need to actually install the pg_hexedit executable you
just built: the convenience scripts are designed to be run from the
pg_hexedit source directory directly, and expect to find the frontend
program in the current working directory.  `make install` works in the usual
way if you want it; the convenience scripts won't be installed.

### Installing ImHex

ImHex has packages for all major platforms; see the [ImHex
website](https://imhex.werwolv.net/) or [Github
releases](https://github.com/WerWolv/ImHex/releases).  On macOS:

```shell
  $ brew install --cask imhex
```

On Linux, distribution packages, Flatpak, and AppImage builds are available.
pg_hexedit is developed against ImHex 1.38; older versions may lack pattern
language features that the generated patterns rely on.

The convenience scripts look for `imhex` on `$PATH`, then fall back to the
macOS app bundle path.  Set `$IMHEX` in `hexedit.cfg` (or the environment)
to override.

## Quickstart guide - Using the convenience scripts

The convenience scripts are designed to be run on a PostgreSQL backend
hacker's laptop, while the target PostgreSQL server is actually running.
The server is queried to locate the relevant relation files and to build the
relation's tuple descriptor from `pg_attribute`.

```shell
  # Should be invoked with CWD that finds pg_hexedit executable:
  $ pwd
  /home/pg/code/pg_hexedit
  # Confirm configuration:
  $ $EDITOR hexedit.cfg
  # Invoke generic script (works on tables and indexes):
  $ ./relation_hexedit pg_type
```

`relation_hexedit` performs a `CHECKPOINT`, generates the pattern next to
the relation file (`<relfile>.hexpat`), and opens ImHex on the file with the
pattern.

### Pattern auto-loading

Generated patterns carry a `#pragma magic` matching PostgreSQL page files,
and the scripts install a copy of the current relation's pattern into
ImHex's user patterns folder (`~/Library/Application Support/imhex/patterns`
on macOS, `~/.local/share/imhex/patterns` on Linux).  ImHex offers to apply
an installed pattern whenever a matching file is opened -- including into an
already-running ImHex instance, where the `--pattern` command line flag is
otherwise dropped by single-instance forwarding.  Accept the prompt and the
annotations appear; no navigating to the pattern file.

Because attribute decoding is per relation, the scripts keep exactly one
relation pattern installed at a time (the most recent), so the prompt has a
single obvious choice.  A structure-only pattern
(`postgresql_page_structure.hexpat`), correct for any PostgreSQL relation
file, is kept alongside for manual use but excluded from auto-offering.

If the prompt doesn't appear, drag the `.hexpat` file onto the ImHex window
(or File → Import → Pattern File) and press the play button in the Pattern
Editor.

### Other scripts

`btree_hexedit` additionally prints the byte offsets of the B-Tree root page
and the non-leaf children of the root, to paste into ImHex's Goto dialog.
`gin_hexedit` prints a summary of contiguous runs of GIN page types.  Both
require `contrib/pageinspect`; `relation_hexedit` does not.

### Requirements and cautions

`psql` should be within your $PATH when the scripts are invoked.  libpq
environment variables like $PGDATABASE can be set within the `hexedit.cfg`
file.  `psql` must connect as a superuser (to determine relfile paths and
install `contrib/pageinspect`).  The scripts assume they run as an OS user
that can read the PostgreSQL relation files at the same absolute paths as
PostgreSQL itself; be very careful if the data directory is containerized.

The convenience scripts will download a portion of the relation using psql
in the event of not finding relation files at the expected location on the
filesystem.  For the fallback to work, `contrib/pageinspect` must be
installed and tagging must start at the beginning of the relation file (see
`hexedit.cfg`).

Having a PostgreSQL relfilenode file open in a hex editor __risks data
corruption__, especially when the PostgreSQL server is actually running
throughout.  The scripts were designed with backend development convenience
in mind, where __the database should only contain disposable test data__.

The scripts will only open the first 1GB segment file in the relation.

## Live mode - viewing shared_buffers instead of files

`live_hexedit` gives a view that tracks the server, without CHECKPOINTs and
without opening the real relation file:

```shell
  $ ./live_hexedit pg_type
```

It generates and installs the relation's pattern, then starts
`pg_hexedit_live_bridge`: a small Python daemon that speaks the GDB remote
serial protocol understood by ImHex's built-in __GDB Server__ provider and
translates every read into `contrib/pageinspect`'s `get_raw_page()`, which
reads through the buffer manager -- the server's *current* view, dirty pages
included.  Nothing nonstandard runs in the server: the bridge is an ordinary
psql client, and pageinspect is standard contrib (where even contrib is
unavailable, it falls back to core `pg_read_binary_file()`, showing file
contents instead).  Only one bridge runs at a time; rerunning the script
restarts it, forgotten bridges exit after 30 idle minutes, and all output is
mirrored to `$TMPDIR/pg_hexedit_live_bridge.log`.

The bridge writes a pre-configured provider entry into ImHex's recents, so
the ImHex side is: restart ImHex, __File → Recent →
"GDB Server <127.0.0.1:4441>"__, accept the pattern prompt.  (The entry's
size is the relation plus an 8MiB pad; the pad is functional, not headroom
-- see below -- so avoid entering sizes by hand.)

ImHex's GDB provider caches every read until it is closed and has no
refresh, so generated patterns defeat the cache themselves: the bridge tags
the pad's last line with the live relation size, and each evaluation reads
that tag (tracking relation growth), sizes its page array to the real
relation, and flushes exactly the provider-cache slots that can hold
relation lines by reading pad addresses that alias them.  Memory replies are
run-length encoded.  The result: hex-view bytes track the server
continuously (view repaints keep re-fetching), and re-evaluating the pattern
(play) refreshes annotations in about the time a local file takes.  Run DML
in psql, watch the bytes move, press play when you want the structure
re-derived.  On plain files the sweep self-disables at zero cost.

By default the live provider is read-only; editing pages otherwise remains a
filesystem-workflow (`relation_hexedit`) task.  `./live_hexedit relname
mirror` maintains a local read-only mirror file instead.

### Writing back to shared_buffers

`./live_hexedit relname write` makes ImHex edits poke straight into
shared_buffers, so a value changed in the hex view takes effect in the
running server immediately -- no file, no CHECKPOINT.

This needs the one piece that cannot be done from a plain client: there is no
SQL to write a shared buffer (pageinspect's `get_raw_page()` reads but has no
inverse).  The `pg_hexedit_poke/` directory is a tiny, superuser-only,
dev-only extension providing a single function that overwrites a byte range
within a page in shared_buffers and marks the buffer dirty.  Build and
install it against the target server, once:

```shell
  $ make -C pg_hexedit_poke install    # uses pg_config from $PATH
```

The bridge then runs `CREATE EXTENSION IF NOT EXISTS pg_hexedit_poke` and
maps each GDB write packet to a `pg_hexedit_poke(rel, block, offset, bytes)`
call (writes into the flush pad are refused).

The write is deliberately __not__ WAL-logged and __not__ crash safe: it
changes the page in shared memory (and, at the next flush, on disk), does not
replicate, and does not advance the page LSN.  That is the right shape for a
raw hex editor and corruption-simulation tool, and the reason it is a
separate opt-in extension rather than part of the read path.  __Only ever use
it on a cluster whose data is disposable.__

## Getting acceptable performance

ImHex keeps every annotation in memory: expect roughly 2MB of evaluator
memory and 35ms of evaluation time per fully annotated block.  A window of a
few hundred blocks is comfortable; annotating an entire large relation at
once is not viable.

The generated pattern therefore only fully annotates blocks in the window
given with `-R` (all other blocks get page-header annotations only), and the
convenience scripts default to blocks 0 - 200 via `$MIN_BLOCK_TAGS` and
`$MAX_BLOCK_TAGS` in `hexedit.cfg`.  Regenerate with a different window to
inspect other parts of a large relation, or `dd` out a slice of the file and
annotate that (in which case, generate with `-n`/`-s` so that block numbers
in annotations remain relation-relative).

## Direct invocation

Invoking pg_hexedit directly (not using convenience scripts) is useful when
you want to work on a copy of the database that is not under the control of
a running PostgreSQL server, or when a psql connection to the running
PostgreSQL server cannot be established.  pg_hexedit writes the pattern to
stdout, and does not need the relation file at all: the same generated
pattern annotates any relation file with a compatible schema.

```shell
  pg_hexedit [options] > relfile.hexpat
```

The `-A` flag provides the tuple descriptor used to decode attributes: a
list with an entry for each pg_attribute entry's attlen, attname, attalign,
and atttypid, in `pg_attribute.attnum` order:

`-A 'attlen,attname,attalign,atttypid,attlen,attname,attalign,atttypid,...'`

The convenience scripts use an SQL query to form the string (see the
`__open_relation` utility script).  The attrlist SQL query will produce a
correct `-A` argument when run against a relation with the same schema as
the target relation/file, even when run against an unrelated PostgreSQL
installation, provided a compatible CPU architecture is used.  Note that
dropped columns need to be represented in the attrlist string: they still
occupy space in tuples that predate the DROP, so omitting them would skew
every later attribute's offset.  Elements that contain whitespace or comma
characters can be parsed as a single element by appearing within double
quotes.  It's good practice to use single quotes for the attrlist argument
as a whole.

The type OID selects how values are displayed (integer, text, point, and so
on); unrecognized types fall back to a width-appropriate display.  The
legacy `-D` flag accepts the historical 3-value attrlist format without type
OIDs.

See `pg_hexedit -h` for full details of all available options.

### Using pg_hexedit while debugging Postgres with GDB

Sometimes, it is useful to invoke pg_hexedit/ImHex to visualize an arbitrary
Postgres page image from within an interactive GDB debugging session that is
attached to a Postgres backend.  This also works well with core dumps.

Generalize from the following example, which can be added to your .gdbinit
dotfile:

```
define dump_page
  dump binary memory /tmp/gdb_postgres_page.dump $arg0 ($arg0 + 8192)
  echo Invoking pg_hexedit + ImHex on page...\n
  ! ~/code/pg_hexedit/pg_hexedit -n 1 > /tmp/gdb_postgres_page.dump.hexpat
  ! imhex --open /tmp/gdb_postgres_page.dump --pattern /tmp/gdb_postgres_page.dump.hexpat &> /dev/null &
end
```

This creates a user-defined GDB command that dumps 8192 bytes (default
BLCKSZ) at an arbitrary memory address to a temp file.  From there, a
pg_hexedit pattern is generated for the temp file.  (Generate once with your
relation's `-A` attrlist for attribute-level decoding.)

Note that the block number displayed by pg_hexedit's annotations will be
spurious when produced by `dump_page`, because there is no general way to
determine the correct block number without more context.  This is usually
only a minor annoyance, though.

See also:  
[PostgreSQL documentation - Database Page
Layout](https://www.postgresql.org/docs/current/static/storage-page-layout.html)  
[PostgreSQL documentation - pageinspect
module](https://www.postgresql.org/docs/current/pageinspect.html)  
[PostgreSQL documentation - Preventing Transaction ID Wraparound Failures
(xmin and xmax fields, Multixacts, and
freezing)](https://www.postgresql.org/docs/current/static/routine-vacuuming.html#VACUUM-FOR-WRAPAROUND)

/*
 * pg_hexedit.c - PostgreSQL relation file annotation generator for ImHex.
 *
 * Copyright (c) 2018-2021, Crunchy Data Solutions, Inc.
 * Copyright (c) 2017-2018, VMware, Inc.
 * Copyright (c) 2002-2010, Red Hat, Inc.
 * Copyright (c) 2011-2021, PostgreSQL Global Development Group
 *
 * Historically a specialized fork of pg_filedump that emitted wxHexEditor
 * XML tags.  Now a generator for the ImHex pattern language: it emits a
 * self-contained .hexpat file to stdout that annotates PostgreSQL relation
 * files (heap and all index access methods) when loaded into ImHex.
 *
 * pg_hexedit no longer reads relation files itself.  Page interpretation
 * happens inside ImHex's pattern evaluator; the emitted pattern classifies
 * each page by its special section (the same way the old tool's
 * GetSpecialSectionType() did) and walks line pointers, tuples, and
 * attributes accordingly.  What this program contributes is correctness by
 * construction: every struct layout, offset, flag mask, and alignment rule
 * in the emitted pattern is computed from the PostgreSQL headers that this
 * program is compiled against, via sizeof/offsetof and the real macros.
 *
 * Attribute-level decoding requires a description of the relation's tuples,
 * passed with -A (or legacy -D); the convenience scripts generate it from
 * pg_attribute via SQL.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Original pg_filedump Author: Patrick Macdonald <patrickm@redhat.com>
 * pg_hexedit author:           Peter Geoghegan <pg@bowt.ie>
 */
#define FRONTEND 1
#include "postgres.h"
#include "common/fe_memutils.h"
#include "utils/elog.h"

/*
 * We must #undef frontend because certain headers are not really supposed to
 * be included in frontend utilities because they include atomics.h.
 */
#undef FRONTEND

/*
 * Define TrapMacro() as a no-op expression on builds that have assertions
 * enabled.  This is redundant though harmless when building without
 * assertions, since the same macro definition happens to be visible there.  It
 * is only visible there because no-op definitions for all assert-related
 * macros happen to be shared by frontend and backend code.
 */
#define TrapMacro(condition, errorType) (true)

#include <time.h>

#include "access/brin_page.h"
#include "access/brin_tuple.h"
#include "access/gin_private.h"
#include "access/gist.h"
#include "access/hash.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/nbtree.h"
#include "access/spgist_private.h"

/*
 * Backend error-reporting stubs.
 *
 * Because we #undef FRONTEND before including the access headers, a few
 * static inline functions that live outside the headers' own FRONTEND guards
 * are compiled with their backend elog(ERROR, ...) paths.  Those paths are
 * unreachable for valid input but still produce undefined references at link
 * time.  Provide minimal stubs so the binary links; if we ever do reach one,
 * abort loudly.
 */
bool
errstart_cold(int elevel, const char *domain)
{
	fprintf(stderr, "pg_hexedit: unexpected backend error path reached\n");
	abort();
}
bool
errstart(int elevel, const char *domain)
{
	fprintf(stderr, "pg_hexedit: unexpected backend error path reached\n");
	abort();
}
void
errfinish(const char *filename, int lineno, const char *funcname)
{
	abort();
}
int
errmsg_internal(const char *fmt,...)
{
	return 0;
}

#define HEXEDIT_VERSION			"0.2"
#define SEQUENCE_MAGIC			0x1717	/* PostgreSQL defined magic number */

/*
 * pg_hexedit color scheme, carried over from the wxHexEditor incarnation.
 * ImHex has a single highlight color per pattern (no separate font color);
 * status that used to be conveyed with font colors lives in formatted value
 * text instead.
 */
#define COLOR_BLACK				"000000"
#define COLOR_BLUE_DARK			"2980B9"
#define COLOR_BLUE_LIGHT		"3498DB"
#define COLOR_BROWN				"97333D"
#define COLOR_GRAY				"808B96"
#define COLOR_GREEN_BRIGHT		"50E964"
#define COLOR_GREEN_DARK		"16A085"
#define COLOR_GREEN_LIGHT		"1ABC9C"
#define COLOR_MAROON			"E96950"
#define COLOR_ORANGE			"FF8C00"
#define COLOR_PINK				"E949D1"
#define COLOR_RED_DARK			"912C21"
#define COLOR_RED_LIGHT			"E74C3C"
#define COLOR_WHITE				"CCD1D1"
#define COLOR_YELLOW_DARK		"F1C40F"
#define COLOR_YELLOW_LIGHT		"E9E850"

/* Page kind codes used within the emitted pattern (mirrors SPEC_SECT_*) */
#define KIND_UNKNOWN	0
#define KIND_HEAP		1
#define KIND_SEQUENCE	2
#define KIND_BTREE		3
#define KIND_HASH		4
#define KIND_GIST		5
#define KIND_GIN		6
#define KIND_SPGIST		7
#define KIND_BRIN		8

/* Attribute display classes, chosen from atttypid/attlen/attalign */
typedef enum attClass
{
	ATT_CLASS_BOOL,				/* bool */
	ATT_CLASS_CHAR,				/* single byte "char" */
	ATT_CLASS_INT16,
	ATT_CLASS_INT32,
	ATT_CLASS_INT64,
	ATT_CLASS_UINT32,			/* oid, xid, cid, regclass etc */
	ATT_CLASS_FLOAT32,
	ATT_CLASS_FLOAT64,
	ATT_CLASS_POINT,			/* two float8s */
	ATT_CLASS_NAME,				/* fixed NAMEDATALEN string */
	ATT_CLASS_FIXED_OPAQUE,		/* other fixed-length: raw bytes */
	ATT_CLASS_VARLENA_TEXT,		/* text-like varlena */
	ATT_CLASS_VARLENA_BIN,		/* other varlena */
	ATT_CLASS_CSTRING			/* attlen -2 */
} attClass;

/* Per-relation tuple descriptor state, filled from -A/-D argument */
static int	nrelatts = 0;
static int *attlenrel = NULL;
static char **attnamerel = NULL;
static char **attcolorrel = NULL;
static char *attalignrel = NULL;
static unsigned int *atttypidrel = NULL;
static attClass *attclassrel = NULL;
static char **attplnamerel = NULL;	/* sanitized PL member identifiers */

/* Options */
static int	blockStart = -1;	/* -R start */
static int	blockEnd = -1;		/* -R end */
static unsigned int segmentNumber = 0;	/* -n */
static unsigned int segmentSize = RELSEG_SIZE * BLCKSZ; /* -s */
static char *relName = NULL;	/* -N, for provenance/labels only */
static int	exitCode = 0;

static uint32 sdbmhash(const unsigned char *elem, size_t len);
static char *GetColorFromAttrname(const char *attrName);

/*
 * Output helpers.  Emitted pattern text goes through these exclusively.
 * L() emits a fixed line with no printf interpretation, which keeps the
 * many '%' and '"' characters in pattern-language text readable.  F() is
 * printf-style for lines that interpolate computed values.
 */
static void
L(const char *s)
{
	fputs(s, stdout);
	fputc('\n', stdout);
}

static void
F(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
}

/*
 * Return color shaded towards black by a small fixed amount.  Used to make
 * adjacent same-color annotations visually separable in ImHex, which has no
 * gap or border between pattern highlights.  Returns a static buffer.
 */
static const char *
ShadeColor(const char *color)
{
	static char shaded[8];
	unsigned int r,
				g,
				b;

	if (sscanf(color, "%2x%2x%2x", &r, &g, &b) != 3)
		return color;
	r = (r > 28) ? r - 28 : 0;
	g = (g > 28) ? g - 28 : 0;
	b = (b > 28) ? b - 28 : 0;
	snprintf(shaded, sizeof(shaded), "%02X%02X%02X", r, g, b);
	return shaded;
}

/*
 * Runtime pattern colors (std::core::set_pattern_color) want ImGui ABGR;
 * alpha 0x70 matches ImHex's native highlight translucency.  Rotating static
 * buffers so that two calls can appear in one F() invocation.
 */
static const char *
AbgrLit(const char *rrggbb)
{
	static char bufs[4][16];
	static int	n = 0;
	unsigned int r,
				g,
				b;
	char	   *buf = bufs[n++ % 4];

	if (sscanf(rrggbb, "%2x%2x%2x", &r, &g, &b) != 3)
		return "0x70FFFFFF";
	snprintf(buf, 16, "0x70%02X%02X%02X", b, g, r);
	return buf;
}

/*
 * Escape a string for embedding in a pattern-language double-quoted string
 * literal.  Returns a palloc'd buffer.
 */
static char *
PLString(const char *s)
{
	char	   *buf = pg_malloc(strlen(s) * 2 + 1);
	char	   *p = buf;

	for (; *s; s++)
	{
		if (*s == '"' || *s == '\\')
			*p++ = '\\';
		*p++ = *s;
	}
	*p = '\0';
	return buf;
}

/*
 * Hash function shared with historical pg_hexedit: sdbm hash from
 * http://www.cse.yorku.ca/~oz/hash.html
 */
static uint32
sdbmhash(const unsigned char *elem, size_t len)
{
	uint32		hash = 0;
	size_t		i;

	for (i = 0; i < len; i++)
		hash = elem[i] + (hash << 6) + (hash << 16) - hash;

	return hash;
}

/*
 * Return a cstring of an html color that is a function of attrName argument.
 * (No leading '#'; ImHex [[color]] wants bare RRGGBB.)
 */
static char *
GetColorFromAttrname(const char *attrName)
{
	uint32		hash;
	uint8		red;
	uint8		green;
	uint8		blue;
	char	   *colorStr = pg_malloc(8);

	hash = sdbmhash((const unsigned char *) attrName, strlen(attrName) + 1);

	/* Use muted pastel shades for user attributes */
	red = 150 + ((uint8) hash % 90);
	green = 150 + ((uint8) (hash >> 8) % 90);
	blue = 150 + ((uint8) (hash >> 16) % 90);
	snprintf(colorStr, 8, "%02X%02X%02X", red, green, blue);

	return colorStr;
}

/*
 * Choose display class for an attribute from type OID (0 when unknown),
 * attlen, and attalign.
 */
static attClass
GetAttClass(unsigned int typid, int attlen, char attalign)
{
	switch (typid)
	{
		case 16:				/* bool */
			return ATT_CLASS_BOOL;
		case 18:				/* "char" */
			return ATT_CLASS_CHAR;
		case 21:				/* int2 */
			return ATT_CLASS_INT16;
		case 23:				/* int4 */
			return ATT_CLASS_INT32;
		case 20:				/* int8 */
			return ATT_CLASS_INT64;
		case 26:				/* oid */
		case 28:				/* xid */
		case 29:				/* cid */
		case 24:				/* regproc */
		case 2202:				/* regprocedure */
		case 2203:				/* regoper */
		case 2204:				/* regoperator */
		case 2205:				/* regclass */
		case 2206:				/* regtype */
			return ATT_CLASS_UINT32;
		case 700:				/* float4 */
			return ATT_CLASS_FLOAT32;
		case 701:				/* float8 */
			return ATT_CLASS_FLOAT64;
		case 600:				/* point */
			return ATT_CLASS_POINT;
		case 19:				/* name */
			return ATT_CLASS_NAME;
		case 25:				/* text */
		case 1042:				/* bpchar */
		case 1043:				/* varchar */
		case 114:				/* json */
		case 142:				/* xml */
			return ATT_CLASS_VARLENA_TEXT;
	}

	if (attlen == -1)
		return ATT_CLASS_VARLENA_BIN;
	if (attlen == -2)
		return ATT_CLASS_CSTRING;

	/* Fixed length with unknown type: display by width where natural */
	switch (attlen)
	{
		case 1:
			return ATT_CLASS_CHAR;
		case 2:
			return ATT_CLASS_INT16;
		case 4:
			return ATT_CLASS_INT32;
		case 8:
			return ATT_CLASS_INT64;
		default:
			return ATT_CLASS_FIXED_OPAQUE;
	}
}

/*
 * Turn attname into a valid, unique pattern-language identifier for use as a
 * member name.  Falls back to att<N> style names.  Dropped columns become
 * dropped_<N>.
 */
static char *
GetPLIdentifier(const char *attname, int attno)
{
	static const char *reserved[] = {
		"if", "else", "while", "for", "fn", "struct", "union", "enum",
		"bitfield", "namespace", "using", "return", "match", "in", "out",
		"be", "le", "signed", "unsigned", "bool", "char", "str", "float",
		"double", "padding", "parent", "this", "true", "false", "auto",
		"ref", "null", "break", "continue", "import", "u8", "u16", "u24",
		"u32", "u48", "u64", "u96", "u128", "s8", "s16", "s24", "s32",
		"s48", "s64", "s96", "s128", "lp", "htup", "itup", "header",
		"linp", "tuple"
	};
	char	   *ident;
	int			i;
	bool		ok;

	if (strncmp(attname, "........pg.dropped.", 19) == 0)
	{
		ident = pg_malloc(32);
		snprintf(ident, 32, "dropped_%d", attno + 1);
		return ident;
	}

	ident = pg_strdup(attname);
	ok = (ident[0] != '\0');
	for (i = 0; ident[i]; i++)
	{
		char		c = ident[i];
		bool		valid = (c == '_' ||
							 (c >= 'a' && c <= 'z') ||
							 (c >= 'A' && c <= 'Z') ||
							 (i > 0 && c >= '0' && c <= '9'));

		if (!valid)
		{
			ok = false;
			break;
		}
	}
	if (ok && ident[0] >= '0' && ident[0] <= '9')
		ok = false;
	if (ok)
	{
		for (i = 0; i < (int) lengthof(reserved); i++)
		{
			if (strcmp(ident, reserved[i]) == 0)
			{
				ok = false;
				break;
			}
		}
	}
	if (ok)
	{
		/* Reject duplicates of earlier columns */
		for (i = 0; i < attno; i++)
		{
			if (attplnamerel[i] && strcmp(ident, attplnamerel[i]) == 0)
			{
				ok = false;
				break;
			}
		}
	}

	if (!ok)
	{
		pg_free(ident);
		ident = pg_malloc(32);
		snprintf(ident, 32, "att%d", attno + 1);
	}
	return ident;
}

/*
 * Parse -A/-D attribute list argument.
 *
 * The string is a list of comma separated values, groupsize values per
 * attribute:  attlen,"attname",attalign  for the legacy 3-value form (-D),
 * plus a trailing atttypid for the 4-value form (-A).  attname is double
 * quoted since it might contain commas.
 */
static bool
ParseAttributeListString(const char *arg, int groupsize)
{
	char	   *attrlist;
	int			itemno;
	char	   *curropt;
	char	   *nextopt;

	/* Allocate space for decoding state */
	attlenrel = pg_malloc(sizeof(int) * MaxTupleAttributeNumber);
	attnamerel = pg_malloc(sizeof(char *) * MaxTupleAttributeNumber);
	attcolorrel = pg_malloc(sizeof(char *) * MaxTupleAttributeNumber);
	attalignrel = pg_malloc(sizeof(char) * MaxTupleAttributeNumber);
	atttypidrel = pg_malloc0(sizeof(unsigned int) * MaxTupleAttributeNumber);
	attclassrel = pg_malloc(sizeof(attClass) * MaxTupleAttributeNumber);
	attplnamerel = pg_malloc0(sizeof(char *) * MaxTupleAttributeNumber);

	/* Create copy of argument string to scribble on */
	attrlist = pg_strdup(arg);

	nrelatts = 0;
	itemno = 0;
	curropt = attrlist;
	while (curropt)
	{
		if (*curropt == '"')
		{
			/* Move past leading " character, omitting it */
			curropt++;
			/* Find terminating " character, skipping any contained ',' char */
			nextopt = strchr(curropt, '"');
			if (!nextopt)
				return false;
			/* Eliminate trailing " character from current option */
			*nextopt = '\0';
			/* Prepare next option */
			nextopt++;
			nextopt = strchr(nextopt, ',');
		}
		else
			nextopt = strchr(curropt, ',');

		if (nextopt)
		{
			/* Eliminate , character from current option */
			*nextopt = '\0';
			nextopt++;
		}

		switch (itemno)
		{
			case 0:
				if (sscanf(curropt, "%d", &attlenrel[nrelatts]) != 1)
				{
					fprintf(stderr, "pg_hexedit error: could not parse attlen from attrlist argument\n");
					return false;
				}
				break;
			case 1:
				/* Copy attribute name, and dynamically generate color */
				attnamerel[nrelatts] = pg_strdup(curropt);
				attcolorrel[nrelatts] = GetColorFromAttrname(curropt);
				break;
			case 2:
				{
					char		attalign = *curropt;

					if (attalign != 'i' && attalign != 'c' && attalign != 'd' &&
						attalign != 's')
					{
						fprintf(stderr, "pg_hexedit error: invalid attalign value '%c' in attrlist argument\n",
								attalign);
						return false;
					}
					if (attlenrel[nrelatts] == -2 && attalign != 'c')
					{
						fprintf(stderr, "pg_hexedit error: unexpected attalign '%c' for cstring in attrlist argument\n",
								attalign);
						return false;
					}
					attalignrel[nrelatts] = attalign;
				}
				break;
			case 3:
				if (sscanf(curropt, "%u", &atttypidrel[nrelatts]) != 1)
				{
					fprintf(stderr, "pg_hexedit error: could not parse atttypid from attrlist argument\n");
					return false;
				}
				break;
		}
		itemno++;

		if (itemno == groupsize)
		{
			/* Attribute complete */
			attclassrel[nrelatts] = GetAttClass(atttypidrel[nrelatts],
												attlenrel[nrelatts],
												attalignrel[nrelatts]);
			attplnamerel[nrelatts] = GetPLIdentifier(attnamerel[nrelatts],
													 nrelatts);
			if (nrelatts >= MaxTupleAttributeNumber)
			{
				fprintf(stderr, "pg_hexedit error: too many attributes represented in attrlist argument\n");
				return false;
			}
			itemno = 0;
			nrelatts++;
		}

		curropt = nextopt;
	}

	/* Be tidy */
	pg_free(attrlist);

	return nrelatts > 0 && itemno == 0;
}

/*
 * Alignment requirement in bytes for an attalign code
 */
static int
AlignBytes(char attalign)
{
	switch (attalign)
	{
		case 'c':
			return 1;
		case 's':
			return ALIGNOF_SHORT;
		case 'i':
			return ALIGNOF_INT;
		case 'd':
			return ALIGNOF_DOUBLE;
	}
	return 1;
}

static void
DisplayOptions(void)
{
	printf
		("\npg_hexedit (for PostgreSQL " PG_VERSION ")"
		 " - emits an ImHex pattern (.hexpat) that annotates PostgreSQL relation files\n\n"
		 "Copyright (c) 2017-2018, VMware, Inc.\n"
		 "Copyright (c) 2018-2021, Crunchy Data Solutions, Inc.\n"
		 "Copyright (c) 2002-2010 Red Hat, Inc.\n"
		 "Copyright (c) 2011-2021, PostgreSQL Global Development Group\n\n"
		 "Usage: pg_hexedit [-hn] [-A attrlist | -D attrlist] [-R startblock endblock] [-n segnumber] [-s segsize] [-N relname]\n\n"
		 "The pattern is written to stdout; redirect it to a .hexpat file and\n"
		 "load it in ImHex together with (a copy of) the relation file.\n\n"
		 "Options:\n"
		 "  -A  Decode tuple attributes; argument is a comma separated list of\n"
		 "      attlen,\"attname\",attalign,atttypid values, one group per attribute\n"
		 "  -D  Legacy form of -A without atttypid\n"
		 "  -R  Only fully annotate blocks in the given (inclusive) file-relative range;\n"
		 "      other blocks get page headers only.  Bounds evaluator memory usage\n"
		 "  -n  Force segment number (normally derived by the convenience scripts);\n"
		 "      makes TIDs in annotations relation-relative for .1, .2, ... segments\n"
		 "  -s  Force segment size (default from build: %u)\n"
		 "  -N  Relation name, used in annotations and provenance comments\n"
		 "  -h  Display this information\n\n"
		 "Report bugs to <pg@bowt.ie>\n", RELSEG_SIZE * BLCKSZ);
}

/*
 * Option consumption.  Returns true when the program should proceed to
 * pattern emission.
 */
static bool
ConsumeOptions(int numOptions, char **options)
{
	int			x;

	for (x = 1; x < numOptions; x++)
	{
		char	   *optionString = options[x];

		if (strcmp(optionString, "-A") == 0 || strcmp(optionString, "-D") == 0)
		{
			int			groupsize = (optionString[1] == 'A') ? 4 : 3;

			if (x >= numOptions - 1)
			{
				fprintf(stderr, "pg_hexedit error: missing attrlist string\n");
				return false;
			}
			if (nrelatts > 0)
			{
				fprintf(stderr, "pg_hexedit error: duplicate attrlist option\n");
				return false;
			}
			optionString = options[++x];
			if (!ParseAttributeListString(optionString, groupsize))
			{
				fprintf(stderr, "pg_hexedit error: invalid attrlist string %s\n",
						optionString);
				return false;
			}
		}
		else if (strcmp(optionString, "-R") == 0)
		{
			if (x >= numOptions - 2)
			{
				fprintf(stderr, "pg_hexedit error: -R requires start and end block\n");
				return false;
			}
			blockStart = atoi(options[++x]);
			blockEnd = atoi(options[++x]);
			if (blockStart < 0 || blockEnd < blockStart)
			{
				fprintf(stderr, "pg_hexedit error: invalid -R block range\n");
				return false;
			}
		}
		else if (strcmp(optionString, "-n") == 0)
		{
			if (x >= numOptions - 1)
			{
				fprintf(stderr, "pg_hexedit error: -n requires segment number\n");
				return false;
			}
			segmentNumber = (unsigned int) atoi(options[++x]);
		}
		else if (strcmp(optionString, "-s") == 0)
		{
			if (x >= numOptions - 1)
			{
				fprintf(stderr, "pg_hexedit error: -s requires segment size\n");
				return false;
			}
			segmentSize = (unsigned int) atoi(options[++x]);
			if (segmentSize == 0 || segmentSize % BLCKSZ != 0)
			{
				fprintf(stderr, "pg_hexedit error: invalid segment size\n");
				return false;
			}
		}
		else if (strcmp(optionString, "-N") == 0)
		{
			if (x >= numOptions - 1)
			{
				fprintf(stderr, "pg_hexedit error: -N requires relation name\n");
				return false;
			}
			relName = options[++x];
		}
		else if (strcmp(optionString, "-h") == 0)
			return false;
		else
		{
			fprintf(stderr, "pg_hexedit error: unknown option \"%s\"\n",
					optionString);
			return false;
		}
	}

	return true;
}

/* ----------------------------------------------------------------
 * Pattern emission
 * ----------------------------------------------------------------
 */

/*
 * Struct field emitter that tracks a running offset and inserts padding[]
 * wherever the C compiler laid out padding, so emitted layouts follow
 * offsetof() by construction.  Reset emitOff to 0 at each struct start.
 */
static int	emitOff = 0;

static void
EFIELD(const char *pltype, const char *name, int off, int size,
	   const char *color)
{
	if (off > emitOff)
		F("    padding[%d];", off - emitOff);
	F("    %s %s [[color(\"%s\")]];", pltype, name, color);
	emitOff = off + size;
}

/*
 * Prologue: pragmas, imports, provenance, and the evaluation-state globals
 * that page/tuple structs use to communicate with nested types (pattern
 * struct locals are not visible to nested struct evaluations, but globals
 * are assignable from struct bodies).
 */
static void
EmitProlog(int numOptions, char **options)
{
	time_t		rightNow = time(NULL);
	struct tm  *localNow = localtime(&rightNow);
	int			i;

	L("// PostgreSQL relation file annotations for ImHex.");
	F("// Generated by pg_hexedit " HEXEDIT_VERSION " for PostgreSQL " PG_VERSION "%s%s",
	  relName ? ", relation: " : "", relName ? relName : "");
	printf("// Options:");
	for (i = 1; i < numOptions; i++)
		printf(" %s", options[i]);
	printf("\n");
	F("// Generated on: %04d-%02d-%02d %02d:%02d:%02d",
	  localNow->tm_year + 1900, localNow->tm_mon + 1, localNow->tm_mday,
	  localNow->tm_hour, localNow->tm_min, localNow->tm_sec);
	F("// Build parameters: BLCKSZ=%u MAXIMUM_ALIGNOF=%u INDEX_MAX_KEYS=%u RELSEG_SIZE=%u",
	  (unsigned int) BLCKSZ, (unsigned int) MAXIMUM_ALIGNOF,
	  (unsigned int) INDEX_MAX_KEYS, (unsigned int) RELSEG_SIZE);
	L("//");
	L("// All struct layouts, offsets, and flag masks in this file were computed");
	L("// with sizeof/offsetof against the PostgreSQL headers at generation time.");
	L("");
	F("#pragma description PostgreSQL relation file%s%s (pg_hexedit)",
	  relName ? " " : "", relName ? relName : "");
	L("// Match any PostgreSQL page file: pd_pagesize_version on block 0.");
	L("// When this pattern sits in ImHex's user patterns folder, ImHex offers");
	L("// to apply it automatically whenever a matching file is opened.");
	F("#pragma magic [ %02X %02X ] @ 0x%02X",
	  (unsigned int) ((BLCKSZ | PG_PAGE_LAYOUT_VERSION) & 0xFF),
	  (unsigned int) (((BLCKSZ | PG_PAGE_LAYOUT_VERSION) >> 8) & 0xFF),
	  (unsigned int) offsetof(PageHeaderData, pd_pagesize_version));
	L("#pragma endian little");
	L("#pragma pattern_limit 0x1000000");
	L("#pragma array_limit 0x100000");
	L("");
	L("import std.core;");
	L("import std.io;");
	L("import std.mem;");
	L("import std.string;");
	L("");
	F("// Only blocks in this (file-relative) range are fully annotated; others");
	L("// get page headers only.  Regenerate with -R to move the window.");
	F("u32 FIRST_TAG_BLOCK = %d;", blockStart >= 0 ? blockStart : 0);
	F("u32 LAST_TAG_BLOCK = %d;", blockEnd >= 0 ? blockEnd : 0x7FFFFFFF);
	F("u32 BASE_BLOCK = %u;      // segment number %u at %u blocks per segment",
	  segmentNumber * (unsigned int) (segmentSize / BLCKSZ), segmentNumber,
	  (unsigned int) (segmentSize / BLCKSZ));
	L("");
	L("// Per-page/per-tuple evaluation state (see generator comments)");
	F("u8  g_kind = %d;", KIND_UNKNOWN);
	L("u32 g_blk = 0;          // relation-relative block number of current page");
	L("bool g_isleaf = false;  // page-level leaf flag (GIN entry / SP-GiST)");
	L("u64 g_item_end = 0;     // absolute end of current item's storage");
	L("u32 g_natts = 0;        // attribute walk: this tuple's attribute count");
	L("u64 g_bits_addr = 0;    // attribute walk: NULL bitmap address (0 = no NULLs)");
	L("u64 g_data_end = 0;     // attribute walk: absolute end of tuple data");
	L("bool g_walk_bad = false; // attribute walk: stop decoding this tuple");
	L("u32 g_special_off = 0;  // current page's pd_special");
	L("u32 g_content_len = 0;  // page content length for raw regions");
	L("");
	L("fn att_present(u32 attno) {");
	L("    if (g_bits_addr == 0)");
	L("        return true;");
	L("    return ((std::mem::read_unsigned(g_bits_addr + (attno >> 3), 1) >> (attno & 7)) & 1) != 0;");
	L("};");
	L("");
}

/*
 * Page-level basics: LSN, page header (with pd_flags bitfield), line
 * pointers, item pointers.
 */
static void
EmitPageBasics(void)
{
	L("// ---------------- page header ----------------");
	L("");
	L("// Unsealed so xlogid and xrecoff are individually editable in Pattern");
	L("// Data (in live --writable mode, edits poke shared_buffers); the");
	L("// collapsed row still reads X/XXXXXXXX via the format.");
	L("struct PageXLogRecPtr {");
	L("    u32 xlogid;");
	L("    u32 xrecoff;");
	L("} [[format(\"fmt_lsn\")]];");
	L("");
	L("fn fmt_lsn(ref auto l) {");
	L("    return std::format(\"{:X}/{:08X}\", l.xlogid, l.xrecoff);");
	L("};");
	L("");
	L("bitfield PageFlags {");
	F("    PD_HAS_FREE_LINES : 1;	// 0x%04X", (unsigned int) PD_HAS_FREE_LINES);
	F("    PD_PAGE_FULL      : 1;	// 0x%04X", (unsigned int) PD_PAGE_FULL);
	F("    PD_ALL_VISIBLE    : 1;	// 0x%04X", (unsigned int) PD_ALL_VISIBLE);
	L("    padding           : 13;");
	L("} [[format(\"fmt_pd_flags\")]];");
	L("");
	L("fn fmt_pd_flags(ref auto f) {");
	L("    str s = \"\";");
	L("    if (f.PD_HAS_FREE_LINES) s = s + \"PD_HAS_FREE_LINES|\";");
	L("    if (f.PD_PAGE_FULL)      s = s + \"PD_PAGE_FULL|\";");
	L("    if (f.PD_ALL_VISIBLE)    s = s + \"PD_ALL_VISIBLE|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct PageHeaderData {");
	F("    PageXLogRecPtr pd_lsn       [[color(\"%s\")]];", COLOR_YELLOW_LIGHT);
	F("    u16 pd_checksum             [[color(\"%s\")]];", COLOR_GREEN_DARK);
	F("    PageFlags pd_flags          [[color(\"%s\")]];", COLOR_YELLOW_DARK);
	F("    u16 pd_lower                [[color(\"%s\")]];", COLOR_MAROON);
	F("    u16 pd_upper                [[color(\"%s\")]];", ShadeColor(COLOR_MAROON));
	F("    u16 pd_special              [[color(\"%s\")]];", COLOR_GREEN_BRIGHT);
	F("    u16 pd_pagesize_version     [[color(\"%s\")]];", COLOR_BROWN);
	F("    u32 pd_prune_xid            [[color(\"%s\")]];", COLOR_RED_LIGHT);
	L("};");
	L("");
	L("// ---------------- line pointers ----------------");
	L("");
	L("fn lp_flags_str(u8 f) {");
	L("    match (f) {");
	L("        (0): return \"LP_UNUSED\";");
	L("        (1): return \"LP_NORMAL\";");
	L("        (2): return \"LP_REDIRECT\";");
	L("        (3): return \"LP_DEAD\";");
	L("    }");
	L("    return \"?\";");
	L("};");
	L("");
	L("bitfield ItemIdData {");
	L("    lp_off   : 15;");
	L("    lp_flags : 2;");
	L("    lp_len   : 15;");
	L("} [[format(\"fmt_itemid\")]];");
	L("");
	L("fn fmt_itemid(ref auto lp) {");
	L("    if (lp.lp_flags == 0 && lp.lp_off == 0 && lp.lp_len == 0)");
	L("        return \"LP_UNUSED\";");
	L("    if (lp.lp_flags == 2)");
	L("        return std::format(\"LP_REDIRECT -> lp {}\", lp.lp_off);");
	L("    return std::format(\"{} off={} len={}\",");
	L("                       lp_flags_str(lp.lp_flags), lp.lp_off, lp.lp_len);");
	L("};");
	L("");
	L("// ---------------- item pointers ----------------");
	L("");
	L("struct ItemPointerData {");
	F("    u16 bi_hi                   [[color(\"%s\")]];", COLOR_BLUE_LIGHT);
	F("    u16 bi_lo                   [[color(\"%s\")]];", ShadeColor(COLOR_BLUE_LIGHT));
	F("    u16 ip_posid                [[color(\"%s\")]];", COLOR_BLUE_DARK);
	L("} [[format(\"fmt_ctid\")]];");
	L("");
	L("fn fmt_ctid(ref auto c) {");
	L("    return std::format(\"({},{})\", (c.bi_hi << 16) | c.bi_lo, c.ip_posid);");
	L("};");
	L("");
}

/*
 * Attribute value types and the attribute walk, generated from the -A/-D
 * tuple descriptor.  The walk is shared by heap tuples and index tuples: the
 * enclosing tuple struct sets g_natts/g_bits_addr/g_data_end and then places
 * a TupleAttrs member at the start of tuple data.
 *
 * Two invariants, carried over from the old implementation:
 *
 * 1. The per-tuple attribute count (heap: HeapTupleHeaderGetNatts(); nbtree
 * pivots: BTreeTupleGetNAtts()) governs the walk, not the catalog's column
 * count.  Tuples that predate ALTER TABLE ADD COLUMN have fewer attributes.
 *
 * 2. Dropped columns must remain in the descriptor; they still occupy space
 * in existing tuples, so omitting them would skew every later offset.
 */
static void
EmitTupleAttrs(void)
{
	int			i;

	L("// ---------------- attribute values ----------------");
	L("");

	if (nrelatts == 0)
	{
		/*
		 * Without a tuple descriptor there is nothing to decode; emit a
		 * TupleAttrs that shows tuple data as raw contents.
		 */
		L("struct TupleAttrs {");
		L("    if ($ < g_data_end) {");
		F("        u8 contents[g_data_end - $] [[sealed, color(\"%s\")]];", COLOR_WHITE);
		L("    }");
		L("};");
		L("");
		return;
	}

	/*
	 * Shared attribute value types.  Type names show up in ImHex's hover
	 * tooltip (that line is not suppressible from the pattern side), so keep
	 * them short and informative rather than generated-looking.
	 */
	L("fn fmt_att_str(ref auto a) { return std::format(\"\\\"{}\\\"\", a.data); };");
	L("fn fmt_att_bin(ref auto a) { return std::format(\"{} bytes\", sizeof(a.data)); };");
	L("fn fmt_att_toast(ref auto a) {");
	L("    return std::format(\"TOAST valueid={} rawsize={}\", a.va_valueid, a.va_rawsize);");
	L("};");
	L("fn fmt_att_point(ref auto a) { return std::format(\"({},{})\", a.x, a.y); };");
	L("fn fmt_att_compressed(ref auto a) {");
	L("    return std::format(\"compressed ({} bytes)\", (a.va_header >> 2) - 8);");
	L("};");
	L("");
	L("// Fixed scalar columns are emitted as bare typed members in the walk");
	L("// (below), so their values are directly editable in the Pattern Data");
	L("// view.  Only types that genuinely need a struct keep one: point pairs");
	L("// its two doubles (unsealed, so x and y stay individually editable");
	L("// while the collapsed row still reads \"(x,y)\"), and varlena keeps its");
	L("// header/value split.");
	L("struct pgpoint { double x; double y; } [[format(\"fmt_att_point\")]];");
	L("");

	/*
	 * Varlena types are emitted per column: the header member is represented
	 * separately from the value (just as the old tool tagged varattrib_1b/
	 * va_4byte/va_compressed headers apart from the attribute value), in a
	 * shaded variant of the column's color, and colors are static type
	 * attributes.
	 */
	for (i = 0; i < nrelatts; i++)
	{
		const char *color = attcolorrel[i];
		const char *shade;
		char	   *plname = attplnamerel[i];
		attClass	class = attclassrel[i];
		const char *base;
		const char *datatype;
		const char *datafmt;

		if (class != ATT_CLASS_VARLENA_TEXT && class != ATT_CLASS_VARLENA_BIN)
			continue;

		shade = pg_strdup(ShadeColor(color));
		base = (class == ATT_CLASS_VARLENA_TEXT) ? "pgtext" : "pgvarlena";
		datatype = (class == ATT_CLASS_VARLENA_TEXT) ? "char" : "u8";
		datafmt = (class == ATT_CLASS_VARLENA_TEXT) ? "fmt_att_str" : "fmt_att_bin";

		F("// varlena shapes for attribute %d: \"%s\"", i + 1, attnamerel[i]);
		F("struct %s_1b_%s {", base, plname);
		F("    u8 va_header [[color(\"%s\"), name(\"varattrib_1b\")]];", shade);
		F("    %s data[(va_header >> 1) - 1] [[color(\"%s\")]];", datatype, color);
		F("} [[format(\"%s\")]];", datafmt);
		F("struct %s_4b_%s {", base, plname);
		F("    u32 va_header [[color(\"%s\"), name(\"va_4byte\")]];", shade);
		F("    %s data[(va_header >> 2) - 4] [[color(\"%s\")]];", datatype, color);
		F("} [[format(\"%s\")]];", datafmt);
		F("struct %s_4bc_%s {", base, plname);
		F("    u32 va_header [[color(\"%s\"), name(\"va_compressed\")]];", shade);
		F("    u32 va_tcinfo [[color(\"%s\")]];", shade);
		F("    u8 cdata[(va_header >> 2) - 8] [[sealed, color(\"%s\")]];", color);
		L("} [[format(\"fmt_att_compressed\")]];");
		F("struct %s_toast_%s {", base, plname);
		F("    u8 va_header [[color(\"%s\"), name(\"varattrib_1b_e\")]];", shade);
		F("    u8 va_tag [[color(\"%s\")]];", shade);
		F("    s32 va_rawsize [[color(\"%s\")]];", color);
		F("    u32 va_extinfo [[color(\"%s\")]];", shade);
		F("    u32 va_valueid [[color(\"%s\")]];", color);
		F("    u32 va_toastrelid [[color(\"%s\")]];", shade);
		L("} [[format(\"fmt_att_toast\")]];");
		pg_free((void *) shade);
	}
	L("");

	/*
	 * The walk itself.  Cursor starts at the beginning of tuple data (the
	 * enclosing tuple struct is responsible for getting it there).
	 */
	L("struct TupleAttrs {");
	for (i = 0; i < nrelatts; i++)
	{
		attClass	class = attclassrel[i];
		int			attlen = attlenrel[i];
		int			alignb = AlignBytes(attalignrel[i]);
		char	   *plname = attplnamerel[i];
		const char *base = (class == ATT_CLASS_VARLENA_TEXT) ? "pgtext" : "pgvarlena";
		const char *fixedtype = NULL;	/* bare PL type token */
		char		arraysuffix[16];	/* "" or "[n]" or "[]" */

		arraysuffix[0] = '\0';
		switch (class)
		{
			case ATT_CLASS_BOOL:
				fixedtype = "bool";
				break;
			case ATT_CLASS_CHAR:
				fixedtype = "char";
				break;
			case ATT_CLASS_INT16:
				fixedtype = "s16";
				break;
			case ATT_CLASS_INT32:
				fixedtype = "s32";
				break;
			case ATT_CLASS_INT64:
				fixedtype = "s64";
				break;
			case ATT_CLASS_UINT32:
				fixedtype = "u32";
				break;
			case ATT_CLASS_FLOAT32:
				fixedtype = "float";
				break;
			case ATT_CLASS_FLOAT64:
				fixedtype = "double";
				break;
			case ATT_CLASS_POINT:
				fixedtype = "pgpoint";
				break;
			case ATT_CLASS_NAME:
				fixedtype = "char";
				snprintf(arraysuffix, sizeof(arraysuffix), "[%d]", attlen);
				break;
			case ATT_CLASS_CSTRING:
				fixedtype = "char";
				snprintf(arraysuffix, sizeof(arraysuffix), "[]");
				break;
			case ATT_CLASS_FIXED_OPAQUE:
				fixedtype = "u8";
				snprintf(arraysuffix, sizeof(arraysuffix), "[%d]", attlen);
				break;
			case ATT_CLASS_VARLENA_TEXT:
			case ATT_CLASS_VARLENA_BIN:
				break;
		}

		F("    // attribute %d: \"%s\"", i + 1, attnamerel[i]);
		F("    if (!g_walk_bad && g_natts >= %d && att_present(%d)) {", i + 1, i);
		if (attlen >= 0 || class == ATT_CLASS_CSTRING)
		{
			/* att_align_nominal(): unconditional alignment */
			if (alignb > 1)
				F("        padding[(%d - ($ %% %d)) %% %d];", alignb, alignb, alignb);
			if (class == ATT_CLASS_CSTRING)
				L("        if ($ < g_data_end) {");
			else
				F("        if ($ + %d <= g_data_end) {", attlen);
			F("            %s %s%s [[color(\"%s\")]];", fixedtype, plname,
			  arraysuffix, attcolorrel[i]);
			L("        } else {");
			L("            g_walk_bad = true;");
			L("        }");
		}
		else
		{
			/*
			 * att_align_pointer(): a varlena starting with an odd byte is a
			 * 1-byte-header datum with no alignment padding; otherwise pad
			 * to attalign and expect a 4-byte header.  A zero first byte
			 * must be alignment padding.
			 */
			F("        u8 va_first_%d = std::mem::read_unsigned($, 1);", i + 1);
			F("        if ((va_first_%d & 0x01) == 0x01) {", i + 1);
			F("            if (va_first_%d == 0x01) {", i + 1);
			L("                // TOAST pointer: 1-byte header with zero length");
			F("                if ($ + 2 + %d <= g_data_end) {",
			  (int) sizeof(varatt_external));
			F("                    %s_toast_%s %s;", base, plname, plname);
			L("                } else {");
			L("                    g_walk_bad = true;");
			L("                }");
			F("            } else if ($ + (va_first_%d >> 1) <= g_data_end) {", i + 1);
			F("                %s_1b_%s %s;", base, plname, plname);
			L("            } else {");
			L("                g_walk_bad = true;");
			L("            }");
			L("        } else {");
			if (alignb > 1)
				F("            padding[(%d - ($ %% %d)) %% %d];", alignb, alignb, alignb);
			F("            u32 va_hdr32_%d = std::mem::read_unsigned($, 4);", i + 1);
			F("            if ((va_hdr32_%d & 0x03) == 0x02) {", i + 1);
			F("                if ($ + (va_hdr32_%d >> 2) <= g_data_end && (va_hdr32_%d >> 2) >= 8) {", i + 1, i + 1);
			F("                    %s_4bc_%s %s;", base, plname, plname);
			L("                } else {");
			L("                    g_walk_bad = true;");
			L("                }");
			F("            } else if ($ + (va_hdr32_%d >> 2) <= g_data_end && (va_hdr32_%d >> 2) >= 4) {", i + 1, i + 1);
			F("                %s_4b_%s %s;", base, plname, plname);
			L("            } else {");
			L("                g_walk_bad = true;");
			L("            }");
			L("        }");
		}
		L("    }");
	}
	L("};");
	L("");
}

/*
 * Heap tuples: infomask bitfields with grep-able base flag names, plus
 * HeapTupleHeaderData.  Bit positions are derived from the real HEAP_* mask
 * values at generation time.
 */
static void
EmitHeapStructs(void)
{
	L("// ---------------- heap tuples ----------------");
	L("");
	L("bitfield HeapInfomask2 {");
	F("    natts             : 11;	// HEAP_NATTS_MASK 0x%04X", (unsigned int) HEAP_NATTS_MASK);
	L("    padding           : 2;");
	F("    HEAP_KEYS_UPDATED : 1;	// 0x%04X", (unsigned int) HEAP_KEYS_UPDATED);
	F("    HEAP_HOT_UPDATED  : 1;	// 0x%04X", (unsigned int) HEAP_HOT_UPDATED);
	F("    HEAP_ONLY_TUPLE   : 1;	// 0x%04X", (unsigned int) HEAP_ONLY_TUPLE);
	L("} [[format(\"fmt_infomask2\")]];");
	L("");
	L("fn fmt_infomask2(ref auto m) {");
	L("    str s = \"\";");
	L("    if (m.HEAP_KEYS_UPDATED) s = s + \"|HEAP_KEYS_UPDATED\";");
	L("    if (m.HEAP_HOT_UPDATED)  s = s + \"|HEAP_HOT_UPDATED\";");
	L("    if (m.HEAP_ONLY_TUPLE)   s = s + \"|HEAP_ONLY_TUPLE\";");
	L("    return std::format(\"natts={}{}\", m.natts, s);");
	L("};");
	L("");
	L("bitfield HeapInfomask {");
	F("    HEAP_HASNULL          : 1;	// 0x%04X", (unsigned int) HEAP_HASNULL);
	F("    HEAP_HASVARWIDTH      : 1;	// 0x%04X", (unsigned int) HEAP_HASVARWIDTH);
	F("    HEAP_HASEXTERNAL      : 1;	// 0x%04X", (unsigned int) HEAP_HASEXTERNAL);
	F("    HEAP_HASOID_OLD       : 1;	// 0x%04X", (unsigned int) HEAP_HASOID_OLD);
	F("    HEAP_XMAX_KEYSHR_LOCK : 1;	// 0x%04X", (unsigned int) HEAP_XMAX_KEYSHR_LOCK);
	F("    HEAP_COMBOCID         : 1;	// 0x%04X", (unsigned int) HEAP_COMBOCID);
	F("    HEAP_XMAX_EXCL_LOCK   : 1;	// 0x%04X", (unsigned int) HEAP_XMAX_EXCL_LOCK);
	F("    HEAP_XMAX_LOCK_ONLY   : 1;	// 0x%04X", (unsigned int) HEAP_XMAX_LOCK_ONLY);
	F("    HEAP_XMIN_COMMITTED   : 1;	// 0x%04X", (unsigned int) HEAP_XMIN_COMMITTED);
	F("    HEAP_XMIN_INVALID     : 1;	// 0x%04X", (unsigned int) HEAP_XMIN_INVALID);
	F("    HEAP_XMAX_COMMITTED   : 1;	// 0x%04X", (unsigned int) HEAP_XMAX_COMMITTED);
	F("    HEAP_XMAX_INVALID     : 1;	// 0x%04X", (unsigned int) HEAP_XMAX_INVALID);
	F("    HEAP_XMAX_IS_MULTI    : 1;	// 0x%04X", (unsigned int) HEAP_XMAX_IS_MULTI);
	F("    HEAP_UPDATED          : 1;	// 0x%04X", (unsigned int) HEAP_UPDATED);
	F("    HEAP_MOVED_OFF        : 1;	// 0x%04X", (unsigned int) HEAP_MOVED_OFF);
	F("    HEAP_MOVED_IN         : 1;	// 0x%04X", (unsigned int) HEAP_MOVED_IN);
	L("} [[format(\"fmt_infomask\")]];");
	L("");
	L("fn fmt_infomask(ref auto m) {");
	L("    str s = \"\";");
	L("    if (m.HEAP_HASNULL)          s = s + \"HEAP_HASNULL|\";");
	L("    if (m.HEAP_HASVARWIDTH)      s = s + \"HEAP_HASVARWIDTH|\";");
	L("    if (m.HEAP_HASEXTERNAL)      s = s + \"HEAP_HASEXTERNAL|\";");
	L("    if (m.HEAP_HASOID_OLD)       s = s + \"HEAP_HASOID_OLD|\";");
	L("    if (m.HEAP_XMAX_KEYSHR_LOCK) s = s + \"HEAP_XMAX_KEYSHR_LOCK|\";");
	L("    if (m.HEAP_COMBOCID)         s = s + \"HEAP_COMBOCID|\";");
	L("    if (m.HEAP_XMAX_EXCL_LOCK)   s = s + \"HEAP_XMAX_EXCL_LOCK|\";");
	L("    if (m.HEAP_XMAX_LOCK_ONLY)   s = s + \"HEAP_XMAX_LOCK_ONLY|\";");
	L("    if (m.HEAP_XMIN_COMMITTED)   s = s + \"HEAP_XMIN_COMMITTED|\";");
	L("    if (m.HEAP_XMIN_INVALID)     s = s + \"HEAP_XMIN_INVALID|\";");
	L("    if (m.HEAP_XMAX_COMMITTED)   s = s + \"HEAP_XMAX_COMMITTED|\";");
	L("    if (m.HEAP_XMAX_INVALID)     s = s + \"HEAP_XMAX_INVALID|\";");
	L("    if (m.HEAP_XMAX_IS_MULTI)    s = s + \"HEAP_XMAX_IS_MULTI|\";");
	L("    if (m.HEAP_UPDATED)          s = s + \"HEAP_UPDATED|\";");
	L("    if (m.HEAP_MOVED_OFF)        s = s + \"HEAP_MOVED_OFF|\";");
	L("    if (m.HEAP_MOVED_IN)         s = s + \"HEAP_MOVED_IN|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct HeapTupleHeaderData {");
	L("    u64 tupstart = $;");
	F("    u32 t_xmin                  [[color(\"%s\")]];", COLOR_RED_LIGHT);
	F("    u32 t_xmax                  [[color(\"%s\")]];", ShadeColor(COLOR_RED_LIGHT));
	L("");
	L("    // t_field3 is the t_cid/t_xvac union; resolve it the way the old");
	L("    // tool did, by peeking at t_infomask for HEAP_MOVED bits");
	F("    if ((std::mem::read_unsigned($ + %d, 2) & 0x%04X) != 0) {",
	  (int) (offsetof(HeapTupleHeaderData, t_infomask) -
			 offsetof(HeapTupleHeaderData, t_choice.t_heap.t_field3)),
	  (unsigned int) HEAP_MOVED);
	F("        u32 t_xvac              [[color(\"%s\")]];", COLOR_PINK);
	L("    } else {");
	F("        u32 t_cid               [[color(\"%s\")]];", COLOR_RED_DARK);
	L("    }");
	L("");
	L("    ItemPointerData t_ctid;");
	F("    HeapInfomask2 t_infomask2   [[color(\"%s\")]];", COLOR_GREEN_LIGHT);
	F("    HeapInfomask t_infomask     [[color(\"%s\")]];", COLOR_GREEN_DARK);
	F("    u8  t_hoff                  [[color(\"%s\")]];", COLOR_YELLOW_LIGHT);
	L("");
	L("    // Per-tuple natts governs the attribute walk, NOT the catalog count");
	L("    u16 tnatts = t_infomask2.natts;");
	L("");
	L("    u16 bl = 0;");
	L("    if (t_infomask.HEAP_HASNULL) {");
	L("        bl = (tnatts + 7) / 8;");
	F("        u8 t_bits[bl] [[color(\"%s\")]];", COLOR_YELLOW_DARK);
	L("    }");
	L("");
	L("    // Sanity: t_hoff must be MAXALIGN(bitmap end).  If it is not, this");
	L("    // tuple is garbage; show the header but skip the attribute walk");
	L("    // instead of aborting the whole evaluation on negative padding.");
	F("    bool sane_hoff = t_hoff == ((%d + bl + %d) / %d) * %d;",
	  (int) offsetof(HeapTupleHeaderData, t_bits),
	  (int) MAXIMUM_ALIGNOF - 1, (int) MAXIMUM_ALIGNOF, (int) MAXIMUM_ALIGNOF);
	L("    if (sane_hoff && g_item_end >= tupstart + t_hoff) {");
	F("        padding[t_hoff - %d - bl];",
	  (int) offsetof(HeapTupleHeaderData, t_bits));
	L("        g_natts = tnatts;");
	L("        g_bits_addr = bl > 0 ? tupstart + 23 : 0;");
	L("        g_data_end = g_item_end;");
	L("        g_walk_bad = false;");
	L("        TupleAttrs attrs [[inline]];");
	L("    }");
	L("} [[format(\"fmt_heap_tuple\")]];");
	L("");
	L("// Collapsed-tuple summary.  Deliberately does NOT lead with t_ctid: for");
	L("// an updated tuple that is the successor, not this tuple.  The tuple's");
	L("// own TID goes into its display name in the naming pass.  Status the");
	L("// old tool conveyed with font colors rides along as text: FROZEN and");
	L("// ABORTED xmin; xmax shown only when it is not invalid.");
	L("fn fmt_heap_tuple(ref auto t) {");
	L("    str xmin = std::format(\"xmin={}\", t.t_xmin);");
	L("    if (t.t_infomask.HEAP_XMIN_COMMITTED && t.t_infomask.HEAP_XMIN_INVALID)");
	L("        xmin = xmin + \" FROZEN\";");
	L("    else if (t.t_infomask.HEAP_XMIN_INVALID)");
	L("        xmin = xmin + \" ABORTED\";");
	L("    str xmax = \"\";");
	L("    if (!t.t_infomask.HEAP_XMAX_INVALID) {");
	L("        xmax = std::format(\" xmax={}\", t.t_xmax);");
	L("        if (t.t_infomask.HEAP_XMAX_IS_MULTI)");
	L("            xmax = xmax + \" (multi)\";");
	L("        if (t.t_infomask.HEAP_XMAX_LOCK_ONLY)");
	L("            xmax = xmax + \" (lock only)\";");
	L("    }");
	L("    return std::format(\"{}{} {} atts, ctid={}\",");
	L("                       xmin, xmax, t.t_infomask2.natts, fmt_ctid(t.t_ctid));");
	L("};");
	L("");
}

/*
 * Index tuples: IndexTupleData with the t_info bitfield, the fixed-size
 * IndexAttributeBitMapData NULL bitmap, and nbtree's special t_tid
 * interpretations (suffix-truncated pivot tuples with optional heap TID
 * tiebreaker at the end of the tuple; posting list tuples from
 * deduplication).
 */
static void
EmitIndexStructs(void)
{
	L("// ---------------- index tuples ----------------");
	L("");
	L("bitfield IndexTInfo {");
	F("    size               : 13;	// INDEX_SIZE_MASK 0x%04X", (unsigned int) INDEX_SIZE_MASK);
	F("    INDEX_AM_RESERVED  : 1;	// 0x%04X (nbtree: INDEX_ALT_TID_MASK)", (unsigned int) INDEX_AM_RESERVED_BIT);
	F("    INDEX_VAR_MASK     : 1;	// 0x%04X", (unsigned int) INDEX_VAR_MASK);
	F("    INDEX_NULL_MASK    : 1;	// 0x%04X", (unsigned int) INDEX_NULL_MASK);
	L("} [[format(\"fmt_t_info\")]];");
	L("");
	L("fn fmt_t_info(ref auto m) {");
	L("    str s = \"\";");
	L("    if (m.INDEX_AM_RESERVED) {");
	F("        if (g_kind == %d) s = s + \"|INDEX_ALT_TID_MASK\";", KIND_BTREE);
	F("        else if (g_kind == %d) s = s + \"|INDEX_MOVED_BY_SPLIT_MASK\";", KIND_HASH);
	L("        else s = s + \"|INDEX_AM_RESERVED_BIT\";");
	L("    }");
	L("    if (m.INDEX_VAR_MASK)  s = s + \"|INDEX_VAR_MASK\";");
	L("    if (m.INDEX_NULL_MASK) s = s + \"|INDEX_NULL_MASK\";");
	L("    return std::format(\"IndexTupleSize()={}{}\", m.size, s);");
	L("};");
	L("");
	F("struct BTPostingTid {");
	F("    u16 bi_hi                   [[color(\"%s\")]];", COLOR_RED_LIGHT);
	F("    u16 bi_lo                   [[color(\"%s\")]];", ShadeColor(COLOR_RED_LIGHT));
	F("    u16 ip_posid                [[color(\"%s\")]];", COLOR_GREEN_LIGHT);
	L("} [[format(\"fmt_ctid\")]];");
	L("");
	F("struct BTPivotHeapTid {");
	F("    u16 bi_hi                   [[color(\"%s\")]];", COLOR_PINK);
	F("    u16 bi_lo                   [[color(\"%s\")]];", ShadeColor(COLOR_PINK));
	F("    u16 ip_posid                [[color(\"%s\")]];", COLOR_PINK);
	L("} [[format(\"fmt_ctid\")]];");
	L("");
	L("struct IndexTupleData {");
	L("    u64 tupstart = $;");
	L("    g_walk_bad = false;");
	L("    ItemPointerData t_tid;");
	F("    IndexTInfo t_info           [[color(\"%s\")]];", COLOR_YELLOW_DARK);
	L("");
	L("    u32 itupsize = t_info.size;");
	L("    g_data_end = tupstart + itupsize;");
	L("");
	L("    // nbtree tuples that abuse t_tid (see nbtree.h):");
	F("    //   pivot:   INDEX_ALT_TID_MASK && !(ip_posid & BT_IS_POSTING 0x%04X)",
	  (unsigned int) BT_IS_POSTING);
	F("    //   posting: INDEX_ALT_TID_MASK && (ip_posid & BT_IS_POSTING)");
	L("    bool bt_pivot = false;");
	L("    bool bt_posting = false;");
	F("    if (g_kind == %d && t_info.INDEX_AM_RESERVED) {", KIND_BTREE);
	F("        if ((t_tid.ip_posid & 0x%04X) != 0) {", (unsigned int) BT_IS_POSTING);
	L("            bt_posting = true;");
	L("        } else {");
	L("            bt_pivot = true;");
	L("        }");
	L("    }");
	L("");
	L("    // Attribute count for the walk: catalog count, except suffix");
	L("    // truncation makes it variable for nbtree pivot tuples");
	F("    u32 itnatts = %d;", nrelatts);
	L("    if (bt_pivot) {");
	F("        itnatts = t_tid.ip_posid & 0x%04X;	// BTreeTupleGetNAtts()", (unsigned int) BT_OFFSET_MASK);
	F("        if (itnatts > %d) {", nrelatts);
	L("            g_walk_bad = true;");
	F("            itnatts = %d;", nrelatts);
	L("        }");
	L("        // Pivot tuples may store a heap TID tiebreaker in their final");
	L("        // 6 bytes (BTreeTupleGetHeapTID())");
	F("        if ((t_tid.ip_posid & 0x%04X) != 0 && itupsize >= %d) {",
	  (unsigned int) BT_PIVOT_HEAP_TID_ATTR,
	  (int) (sizeof(IndexTupleData) + sizeof(ItemPointerData)));
	F("            BTPivotHeapTid pivot_htid @ tupstart + itupsize - %d [[no_unique_address]];",
	  (int) sizeof(ItemPointerData));
	F("            g_data_end = tupstart + itupsize - %d;",
	  (int) sizeof(ItemPointerData));
	L("        }");
	L("    }");
	L("");
	L("    if (t_info.INDEX_NULL_MASK) {");
	F("        u8 nullbitmap[%d] [[color(\"%s\"), name(\"IndexAttributeBitMapData\")]];",
	  (int) sizeof(IndexAttributeBitMapData), COLOR_YELLOW_DARK);
	F("        padding[%d];",
	  (int) (IndexInfoFindDataOffset(INDEX_NULL_MASK) -
			 sizeof(IndexTupleData) - sizeof(IndexAttributeBitMapData)));
	F("        g_bits_addr = tupstart + %d;", (int) sizeof(IndexTupleData));
	L("    } else {");
	F("        padding[%d];",
	  (int) (IndexInfoFindDataOffset(0) - sizeof(IndexTupleData)));
	L("        g_bits_addr = 0;");
	L("    }");
	L("");
	L("    // GIN main entry tree, leaf level: tuples can embed a compressed");
	L("    // posting list (posid = number of items, bi_lo = offset), or point");
	L("    // to a posting tree with magic offset GIN_TREE_POSTING");
	F("    if (g_kind == %d && g_isleaf) {", KIND_GIN);
	F("        if (t_tid.ip_posid != 0x%04X && t_tid.ip_posid > 0 &&",
	  (unsigned int) GIN_TREE_POSTING);
	F("            t_tid.bi_lo >= %d && t_tid.bi_lo <= itupsize) {",
	  (int) sizeof(IndexTupleData));
	F("            u8 gin_posting[itupsize - t_tid.bi_lo] @ tupstart + t_tid.bi_lo [[sealed, no_unique_address, color(\"%s\")]];",
	  COLOR_ORANGE);
	L("            g_data_end = tupstart + t_tid.bi_lo;");
	L("        }");
	L("    }");
	L("    if (bt_posting) {");
	L("        // Deduplication posting list tuple: keys, then an array of");
	L("        // heap TIDs at BTreeTupleGetPostingOffset()");
	F("        u32 bt_nposting = t_tid.ip_posid & 0x%04X;", (unsigned int) BT_OFFSET_MASK);
	L("        u32 bt_postingoff = (t_tid.bi_hi << 16) | t_tid.bi_lo;");
	F("        if (bt_postingoff >= %d && bt_postingoff < itupsize &&",
	  (int) sizeof(IndexTupleData));
	F("            bt_postingoff + bt_nposting * %d <= itupsize) {",
	  (int) sizeof(ItemPointerData));
	L("            BTPostingTid posting_tids[bt_nposting] @ tupstart + bt_postingoff [[no_unique_address]];");
	L("            g_data_end = tupstart + bt_postingoff;");
	L("        } else {");
	L("            g_walk_bad = true;");
	L("        }");
	L("    }");
	L("");
	// Multi-column GIN entry tuples carry an implicit int16 attribute
	// number prefix; we do not decode those (matches the old tool)
	F("    if (g_kind == %d && %d > 1) g_walk_bad = true;", KIND_GIN, nrelatts);
	L("    g_natts = itnatts;");
	L("    if ($ <= g_data_end) {");
	L("        TupleAttrs attrs [[inline]];");
	L("    }");
	L("} [[format(\"fmt_index_tuple\")]];");
	L("");
	L("fn fmt_index_tuple(ref auto t) {");
	L("    return std::format(\"t_tid={} size={}\", fmt_ctid(t.t_tid), t.t_info.size);");
	L("};");
	L("");
}

/*
 * Special sections and metapages.  Stage A emits the nbtree special/meta
 * structs in full detail; other AMs currently get their special area as a
 * labeled raw region (detailed structs follow the same pattern and are
 * added AM by AM).
 */
static void
EmitSpecialStructs(void)
{
	L("// ---------------- special sections ----------------");
	L("");
	L("bitfield BTPageFlags {");
	F("    BTP_LEAF             : 1;	// 0x%04X", (unsigned int) BTP_LEAF);
	F("    BTP_ROOT             : 1;	// 0x%04X", (unsigned int) BTP_ROOT);
	F("    BTP_DELETED          : 1;	// 0x%04X", (unsigned int) BTP_DELETED);
	F("    BTP_META             : 1;	// 0x%04X", (unsigned int) BTP_META);
	F("    BTP_HALF_DEAD        : 1;	// 0x%04X", (unsigned int) BTP_HALF_DEAD);
	F("    BTP_SPLIT_END        : 1;	// 0x%04X", (unsigned int) BTP_SPLIT_END);
	F("    BTP_HAS_GARBAGE      : 1;	// 0x%04X", (unsigned int) BTP_HAS_GARBAGE);
	F("    BTP_INCOMPLETE_SPLIT : 1;	// 0x%04X", (unsigned int) BTP_INCOMPLETE_SPLIT);
	F("    BTP_HAS_FULLXID      : 1;	// 0x%04X", (unsigned int) BTP_HAS_FULLXID);
	L("    padding              : 7;");
	L("} [[format(\"fmt_btpo_flags\")]];");
	L("");
	L("fn fmt_btpo_flags(ref auto f) {");
	L("    str s = \"\";");
	L("    if (f.BTP_LEAF)             s = s + \"BTP_LEAF|\";");
	L("    if (f.BTP_ROOT)             s = s + \"BTP_ROOT|\";");
	L("    if (f.BTP_DELETED)          s = s + \"BTP_DELETED|\";");
	L("    if (f.BTP_META)             s = s + \"BTP_META|\";");
	L("    if (f.BTP_HALF_DEAD)        s = s + \"BTP_HALF_DEAD|\";");
	L("    if (f.BTP_SPLIT_END)        s = s + \"BTP_SPLIT_END|\";");
	L("    if (f.BTP_HAS_GARBAGE)      s = s + \"BTP_HAS_GARBAGE|\";");
	L("    if (f.BTP_INCOMPLETE_SPLIT) s = s + \"BTP_INCOMPLETE_SPLIT|\";");
	L("    if (f.BTP_HAS_FULLXID)      s = s + \"BTP_HAS_FULLXID|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct BTPageOpaqueData {");
	F("    u32 btpo_prev               [[color(\"%s\")]];", COLOR_GREEN_BRIGHT);
	F("    u32 btpo_next               [[color(\"%s\")]];", ShadeColor(COLOR_GREEN_BRIGHT));
	F("    u32 btpo_level              [[color(\"%s\")]];", COLOR_GREEN_BRIGHT);
	F("    BTPageFlags btpo_flags      [[color(\"%s\")]];", ShadeColor(COLOR_GREEN_BRIGHT));
	F("    u16 btpo_cycleid            [[color(\"%s\")]];", COLOR_GREEN_BRIGHT);
	L("} [[format(\"fmt_btree_special\")]];");
	L("");
	L("fn fmt_btree_special(ref auto s) {");
	L("    return std::format(\"level={} prev={} next={} {}\",");
	L("                       s.btpo_level, s.btpo_prev, s.btpo_next,");
	L("                       fmt_btpo_flags(s.btpo_flags));");
	L("};");
	L("");
	L("struct BTMetaPageData {");
	F("    u32 btm_magic               [[color(\"%s\")]];", COLOR_PINK);
	F("    u32 btm_version             [[color(\"%s\")]];", ShadeColor(COLOR_PINK));
	F("    u32 btm_root                [[color(\"%s\")]];", COLOR_PINK);
	F("    u32 btm_level               [[color(\"%s\")]];", ShadeColor(COLOR_PINK));
	F("    u32 btm_fastroot            [[color(\"%s\")]];", COLOR_PINK);
	F("    u32 btm_fastlevel           [[color(\"%s\")]];", ShadeColor(COLOR_PINK));
	F("    u32 btm_last_cleanup_num_delpages [[color(\"%s\")]];", COLOR_PINK);
	if (offsetof(BTMetaPageData, btm_last_cleanup_num_heap_tuples) >
		offsetof(BTMetaPageData, btm_last_cleanup_num_delpages) + sizeof(uint32))
		F("    padding[%d];",
		  (int) (offsetof(BTMetaPageData, btm_last_cleanup_num_heap_tuples) -
				 offsetof(BTMetaPageData, btm_last_cleanup_num_delpages) -
				 sizeof(uint32)));
	F("    double btm_last_cleanup_num_heap_tuples [[color(\"%s\")]];", ShadeColor(COLOR_PINK));
	F("    bool btm_allequalimage      [[color(\"%s\")]];", COLOR_PINK);
	L("} [[format(\"fmt_btree_meta\")]];");
	L("");
	L("fn fmt_btree_meta(ref auto m) {");
	L("    return std::format(\"root={} level={} fastroot={} fastlevel={}\",");
	L("                       m.btm_root, m.btm_level, m.btm_fastroot, m.btm_fastlevel);");
	L("};");
	L("");
	L("struct SequenceMagic {");
	F("    u32 magic                   [[color(\"%s\")]];", COLOR_GREEN_BRIGHT);
	L("};");
	L("");
	/* ---- hash ---- */
	L("bitfield HashPageFlags {");
	F("    LH_OVERFLOW_PAGE              : 1;	// 0x%04X", (unsigned int) LH_OVERFLOW_PAGE);
	F("    LH_BUCKET_PAGE                : 1;	// 0x%04X", (unsigned int) LH_BUCKET_PAGE);
	F("    LH_BITMAP_PAGE                : 1;	// 0x%04X", (unsigned int) LH_BITMAP_PAGE);
	F("    LH_META_PAGE                  : 1;	// 0x%04X", (unsigned int) LH_META_PAGE);
	F("    LH_BUCKET_BEING_POPULATED     : 1;	// 0x%04X", (unsigned int) LH_BUCKET_BEING_POPULATED);
	F("    LH_BUCKET_BEING_SPLIT         : 1;	// 0x%04X", (unsigned int) LH_BUCKET_BEING_SPLIT);
	F("    LH_BUCKET_NEEDS_SPLIT_CLEANUP : 1;	// 0x%04X", (unsigned int) LH_BUCKET_NEEDS_SPLIT_CLEANUP);
	F("    LH_PAGE_HAS_DEAD_TUPLES       : 1;	// 0x%04X", (unsigned int) LH_PAGE_HAS_DEAD_TUPLES);
	L("    padding                       : 8;");
	L("} [[format(\"fmt_hash_flags\")]];");
	L("");
	L("fn fmt_hash_flags(ref auto f) {");
	L("    str s = \"\";");
	L("    if (f.LH_OVERFLOW_PAGE)              s = s + \"LH_OVERFLOW_PAGE|\";");
	L("    if (f.LH_BUCKET_PAGE)                s = s + \"LH_BUCKET_PAGE|\";");
	L("    if (f.LH_BITMAP_PAGE)                s = s + \"LH_BITMAP_PAGE|\";");
	L("    if (f.LH_META_PAGE)                  s = s + \"LH_META_PAGE|\";");
	L("    if (f.LH_BUCKET_BEING_POPULATED)     s = s + \"LH_BUCKET_BEING_POPULATED|\";");
	L("    if (f.LH_BUCKET_BEING_SPLIT)         s = s + \"LH_BUCKET_BEING_SPLIT|\";");
	L("    if (f.LH_BUCKET_NEEDS_SPLIT_CLEANUP) s = s + \"LH_BUCKET_NEEDS_SPLIT_CLEANUP|\";");
	L("    if (f.LH_PAGE_HAS_DEAD_TUPLES)       s = s + \"LH_PAGE_HAS_DEAD_TUPLES|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct HashPageOpaqueData {");
	emitOff = 0;
	EFIELD("u32", "hasho_prevblkno",
		   (int) offsetof(HashPageOpaqueData, hasho_prevblkno), 4, COLOR_GREEN_BRIGHT);
	EFIELD("u32", "hasho_nextblkno",
		   (int) offsetof(HashPageOpaqueData, hasho_nextblkno), 4, ShadeColor(COLOR_GREEN_BRIGHT));
	EFIELD("u32", "hasho_bucket",
		   (int) offsetof(HashPageOpaqueData, hasho_bucket), 4, COLOR_GREEN_BRIGHT);
	EFIELD("HashPageFlags", "hasho_flag",
		   (int) offsetof(HashPageOpaqueData, hasho_flag), 2, ShadeColor(COLOR_GREEN_BRIGHT));
	EFIELD("u16", "hasho_page_id",
		   (int) offsetof(HashPageOpaqueData, hasho_page_id), 2, COLOR_GREEN_BRIGHT);
	L("} [[format(\"fmt_hash_special\")]];");
	L("");
	L("fn fmt_hash_special(ref auto s) {");
	L("    return std::format(\"bucket={} {}\", s.hasho_bucket, fmt_hash_flags(s.hasho_flag));");
	L("};");
	L("");
	L("struct HashMetaPageData {");
	emitOff = 0;
	EFIELD("u32", "hashm_magic", (int) offsetof(HashMetaPageData, hashm_magic), 4, COLOR_PINK);
	EFIELD("u32", "hashm_version", (int) offsetof(HashMetaPageData, hashm_version), 4, ShadeColor(COLOR_PINK));
	EFIELD("double", "hashm_ntuples", (int) offsetof(HashMetaPageData, hashm_ntuples), 8, COLOR_PINK);
	EFIELD("u16", "hashm_ffactor", (int) offsetof(HashMetaPageData, hashm_ffactor), 2, ShadeColor(COLOR_PINK));
	EFIELD("u16", "hashm_bsize", (int) offsetof(HashMetaPageData, hashm_bsize), 2, COLOR_PINK);
	EFIELD("u16", "hashm_bmsize", (int) offsetof(HashMetaPageData, hashm_bmsize), 2, ShadeColor(COLOR_PINK));
	EFIELD("u16", "hashm_bmshift", (int) offsetof(HashMetaPageData, hashm_bmshift), 2, COLOR_PINK);
	EFIELD("u32", "hashm_maxbucket", (int) offsetof(HashMetaPageData, hashm_maxbucket), 4, ShadeColor(COLOR_PINK));
	EFIELD("u32", "hashm_highmask", (int) offsetof(HashMetaPageData, hashm_highmask), 4, COLOR_PINK);
	EFIELD("u32", "hashm_lowmask", (int) offsetof(HashMetaPageData, hashm_lowmask), 4, ShadeColor(COLOR_PINK));
	EFIELD("u32", "hashm_ovflpoint", (int) offsetof(HashMetaPageData, hashm_ovflpoint), 4, COLOR_PINK);
	EFIELD("u32", "hashm_firstfree", (int) offsetof(HashMetaPageData, hashm_firstfree), 4, ShadeColor(COLOR_PINK));
	EFIELD("u32", "hashm_nmaps", (int) offsetof(HashMetaPageData, hashm_nmaps), 4, COLOR_PINK);
	EFIELD("u32", "hashm_procid", (int) offsetof(HashMetaPageData, hashm_procid), 4, ShadeColor(COLOR_PINK));
	F("    u32 hashm_spares[%d] [[sealed, color(\"%s\")]];",
	  (int) HASH_MAX_SPLITPOINTS, COLOR_PINK);
	F("    u32 hashm_mapp[%d] [[sealed, color(\"%s\")]];",
	  (int) HASH_MAX_BITMAPS, ShadeColor(COLOR_PINK));
	L("} [[format(\"fmt_hash_meta\")]];");
	L("");
	L("fn fmt_hash_meta(ref auto m) {");
	L("    return std::format(\"maxbucket={} ntuples={} ovflpoint={}\",");
	L("                       m.hashm_maxbucket, m.hashm_ntuples, m.hashm_ovflpoint);");
	L("};");
	L("");
	L("struct HashBitmapContent {");
	F("    u8 bitmap[g_content_len] [[sealed, color(\"%s\")]];", COLOR_YELLOW_DARK);
	L("};");
	L("");
	/* ---- GiST ---- */
	L("bitfield GistPageFlags {");
	F("    F_LEAF             : 1;	// 0x%04X", (unsigned int) F_LEAF);
	F("    F_DELETED          : 1;	// 0x%04X", (unsigned int) F_DELETED);
	F("    F_TUPLES_DELETED   : 1;	// 0x%04X", (unsigned int) F_TUPLES_DELETED);
	F("    F_FOLLOW_RIGHT     : 1;	// 0x%04X", (unsigned int) F_FOLLOW_RIGHT);
	F("    F_HAS_GARBAGE      : 1;	// 0x%04X", (unsigned int) F_HAS_GARBAGE);
	L("    padding            : 11;");
	L("} [[format(\"fmt_gist_flags\")]];");
	L("");
	L("fn fmt_gist_flags(ref auto f) {");
	L("    str s = \"\";");
	L("    if (f.F_LEAF)           s = s + \"F_LEAF|\";");
	L("    if (f.F_DELETED)        s = s + \"F_DELETED|\";");
	L("    if (f.F_TUPLES_DELETED) s = s + \"F_TUPLES_DELETED|\";");
	L("    if (f.F_FOLLOW_RIGHT)   s = s + \"F_FOLLOW_RIGHT|\";");
	L("    if (f.F_HAS_GARBAGE)    s = s + \"F_HAS_GARBAGE|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct GISTPageOpaqueData {");
	emitOff = 0;
	EFIELD("PageXLogRecPtr", "nsn", (int) offsetof(GISTPageOpaqueData, nsn), 8, COLOR_GREEN_BRIGHT);
	EFIELD("u32", "rightlink", (int) offsetof(GISTPageOpaqueData, rightlink), 4, ShadeColor(COLOR_GREEN_BRIGHT));
	EFIELD("GistPageFlags", "flags", (int) offsetof(GISTPageOpaqueData, flags), 2, COLOR_GREEN_BRIGHT);
	EFIELD("u16", "gist_page_id", (int) offsetof(GISTPageOpaqueData, gist_page_id), 2, ShadeColor(COLOR_GREEN_BRIGHT));
	L("} [[format(\"fmt_gist_special\")]];");
	L("");
	L("fn fmt_gist_special(ref auto s) {");
	L("    return std::format(\"rightlink={} {}\", s.rightlink, fmt_gist_flags(s.flags));");
	L("};");
	L("");
	/* ---- GIN ---- */
	L("bitfield GinPageFlags {");
	F("    GIN_DATA             : 1;	// 0x%04X", (unsigned int) GIN_DATA);
	F("    GIN_LEAF             : 1;	// 0x%04X", (unsigned int) GIN_LEAF);
	F("    GIN_DELETED          : 1;	// 0x%04X", (unsigned int) GIN_DELETED);
	F("    GIN_META             : 1;	// 0x%04X", (unsigned int) GIN_META);
	F("    GIN_LIST             : 1;	// 0x%04X", (unsigned int) GIN_LIST);
	F("    GIN_LIST_FULLROW     : 1;	// 0x%04X", (unsigned int) GIN_LIST_FULLROW);
	F("    GIN_INCOMPLETE_SPLIT : 1;	// 0x%04X", (unsigned int) GIN_INCOMPLETE_SPLIT);
	F("    GIN_COMPRESSED       : 1;	// 0x%04X", (unsigned int) GIN_COMPRESSED);
	L("    padding              : 8;");
	L("} [[format(\"fmt_gin_flags\")]];");
	L("");
	L("fn fmt_gin_flags(ref auto f) {");
	L("    str s = \"\";");
	L("    if (f.GIN_DATA)             s = s + \"GIN_DATA|\";");
	L("    if (f.GIN_LEAF)             s = s + \"GIN_LEAF|\";");
	L("    if (f.GIN_DELETED)          s = s + \"GIN_DELETED|\";");
	L("    if (f.GIN_META)             s = s + \"GIN_META|\";");
	L("    if (f.GIN_LIST)             s = s + \"GIN_LIST|\";");
	L("    if (f.GIN_LIST_FULLROW)     s = s + \"GIN_LIST_FULLROW|\";");
	L("    if (f.GIN_INCOMPLETE_SPLIT) s = s + \"GIN_INCOMPLETE_SPLIT|\";");
	L("    if (f.GIN_COMPRESSED)       s = s + \"GIN_COMPRESSED|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct GinPageOpaqueData {");
	emitOff = 0;
	EFIELD("u32", "rightlink", (int) offsetof(GinPageOpaqueData, rightlink), 4, COLOR_GREEN_BRIGHT);
	EFIELD("u16", "maxoff", (int) offsetof(GinPageOpaqueData, maxoff), 2, ShadeColor(COLOR_GREEN_BRIGHT));
	EFIELD("GinPageFlags", "flags", (int) offsetof(GinPageOpaqueData, flags), 2, COLOR_GREEN_BRIGHT);
	L("} [[format(\"fmt_gin_special\")]];");
	L("");
	L("fn fmt_gin_special(ref auto s) {");
	L("    return std::format(\"rightlink={} maxoff={} {}\",");
	L("                       s.rightlink, s.maxoff, fmt_gin_flags(s.flags));");
	L("};");
	L("");
	L("struct GinMetaPageData {");
	emitOff = 0;
	EFIELD("u32", "head", (int) offsetof(GinMetaPageData, head), 4, COLOR_PINK);
	EFIELD("u32", "tail", (int) offsetof(GinMetaPageData, tail), 4, ShadeColor(COLOR_PINK));
	EFIELD("u32", "tailFreeSize", (int) offsetof(GinMetaPageData, tailFreeSize), 4, COLOR_PINK);
	EFIELD("u32", "nPendingPages", (int) offsetof(GinMetaPageData, nPendingPages), 4, ShadeColor(COLOR_PINK));
	EFIELD("s64", "nPendingHeapTuples", (int) offsetof(GinMetaPageData, nPendingHeapTuples), 8, COLOR_PINK);
	EFIELD("u32", "nTotalPages", (int) offsetof(GinMetaPageData, nTotalPages), 4, ShadeColor(COLOR_PINK));
	EFIELD("u32", "nEntryPages", (int) offsetof(GinMetaPageData, nEntryPages), 4, COLOR_PINK);
	EFIELD("u32", "nDataPages", (int) offsetof(GinMetaPageData, nDataPages), 4, ShadeColor(COLOR_PINK));
	EFIELD("s64", "nEntries", (int) offsetof(GinMetaPageData, nEntries), 8, COLOR_PINK);
	EFIELD("s32", "ginVersion", (int) offsetof(GinMetaPageData, ginVersion), 4, ShadeColor(COLOR_PINK));
	L("} [[format(\"fmt_gin_meta\")]];");
	L("");
	L("fn fmt_gin_meta(ref auto m) {");
	L("    return std::format(\"entries={} entryPages={} dataPages={} version={}\",");
	L("                       m.nEntries, m.nEntryPages, m.nDataPages, m.ginVersion);");
	L("};");
	L("");
	L("// GIN posting tree internal pages: PostingItem array after the header");
	L("struct GinPostingItem {");
	F("    u16 child_bi_hi  [[color(\"%s\")]];", COLOR_BLUE_LIGHT);
	F("    u16 child_bi_lo  [[color(\"%s\")]];", ShadeColor(COLOR_BLUE_LIGHT));
	F("    u16 key_bi_hi    [[color(\"%s\")]];", COLOR_WHITE);
	F("    u16 key_bi_lo    [[color(\"%s\")]];", ShadeColor(COLOR_WHITE));
	F("    u16 key_posid    [[color(\"%s\")]];", COLOR_WHITE);
	L("} [[format(\"fmt_gin_posting_item\")]];");
	L("");
	L("fn fmt_gin_posting_item(ref auto i) {");
	L("    return std::format(\"child={} key=({},{})\",");
	L("                       (i.child_bi_hi << 16) | i.child_bi_lo,");
	L("                       (i.key_bi_hi << 16) | i.key_bi_lo, i.key_posid);");
	L("};");
	L("");
	L("// GIN posting tree leaf pages: chain of compressed posting list segments");
	L("struct GinPostingList {");
	L("    ItemPointerData first;");
	F("    u16 nbytes [[color(\"%s\")]];", COLOR_YELLOW_DARK);
	F("    u8 varbyte_tids[nbytes] [[sealed, color(\"%s\")]];", COLOR_ORANGE);
	L("    padding[(2 - ($ % 2)) % 2];	// SHORTALIGN between segments");
	L("} [[format(\"fmt_gin_posting_list\")]];");
	L("");
	L("fn fmt_gin_posting_list(ref auto l) {");
	L("    return std::format(\"first={} nbytes={}\", fmt_ctid(l.first), l.nbytes);");
	L("};");
	L("");
		/* ---- SP-GiST ---- */
	L("bitfield SpGistPageFlags {");
	F("    SPGIST_META     : 1;	// 0x%04X", (unsigned int) SPGIST_META);
	F("    SPGIST_DELETED  : 1;	// 0x%04X", (unsigned int) SPGIST_DELETED);
	F("    SPGIST_LEAF     : 1;	// 0x%04X", (unsigned int) SPGIST_LEAF);
	F("    SPGIST_NULLS    : 1;	// 0x%04X", (unsigned int) SPGIST_NULLS);
	L("    padding         : 12;");
	L("} [[format(\"fmt_spgist_flags\")]];");
	L("");
	L("fn fmt_spgist_flags(ref auto f) {");
	L("    str s = \"\";");
	L("    if (f.SPGIST_META)    s = s + \"SPGIST_META|\";");
	L("    if (f.SPGIST_DELETED) s = s + \"SPGIST_DELETED|\";");
	L("    if (f.SPGIST_LEAF)    s = s + \"SPGIST_LEAF|\";");
	L("    if (f.SPGIST_NULLS)   s = s + \"SPGIST_NULLS|\";");
	L("    if (std::string::length(s) == 0) return \"(none)\";");
	L("    return std::string::substr(s, 0, std::string::length(s) - 1);");
	L("};");
	L("");
	L("struct SpGistPageOpaqueData {");
	emitOff = 0;
	EFIELD("SpGistPageFlags", "flags",
		   (int) offsetof(SpGistPageOpaqueData, flags), 2, COLOR_GREEN_BRIGHT);
	EFIELD("u16", "nRedirection",
		   (int) offsetof(SpGistPageOpaqueData, nRedirection), 2, ShadeColor(COLOR_GREEN_BRIGHT));
	EFIELD("u16", "nPlaceholder",
		   (int) offsetof(SpGistPageOpaqueData, nPlaceholder), 2, COLOR_GREEN_BRIGHT);
	EFIELD("u16", "spgist_page_id",
		   (int) offsetof(SpGistPageOpaqueData, spgist_page_id), 2, ShadeColor(COLOR_GREEN_BRIGHT));
	L("} [[format(\"fmt_spgist_special\")]];");
	L("");
	L("fn fmt_spgist_special(ref auto s) {");
	L("    return std::format(\"nRedirection={} nPlaceholder={} {}\",");
	L("                       s.nRedirection, s.nPlaceholder, fmt_spgist_flags(s.flags));");
	L("};");
	L("");
	L("struct SpGistLastUsedPage {");
	F("    u32 blkno     [[color(\"%s\")]];", COLOR_PINK);
	F("    s32 freeSpace [[color(\"%s\")]];", ShadeColor(COLOR_PINK));
	L("} [[format(\"fmt_spgist_lup\")]];");
	L("");
	L("fn fmt_spgist_lup(ref auto l) {");
	L("    return std::format(\"blkno={} freeSpace={}\", l.blkno, l.freeSpace);");
	L("};");
	L("");
	L("struct SpGistMetaPageData {");
	F("    u32 magicNumber [[color(\"%s\")]];", COLOR_PINK);
	F("    SpGistLastUsedPage cachedPage[%d];", (int) SPGIST_CACHED_PAGES);
	L("};");
	L("");
	L("fn spgist_state_str(u8 st) {");
	F("    match (st) {");
	F("        (%d): return \"SPGIST_LIVE\";", (int) SPGIST_LIVE);
	F("        (%d): return \"SPGIST_REDIRECT\";", (int) SPGIST_REDIRECT);
	F("        (%d): return \"SPGIST_DEAD\";", (int) SPGIST_DEAD);
	F("        (%d): return \"SPGIST_PLACEHOLDER\";", (int) SPGIST_PLACEHOLDER);
	L("    }");
	L("    return \"?\";");
	L("};");
	L("");
	L("bitfield SpgInnerHead {");
	L("    tupstate   : 2;");
	L("    allTheSame : 1;");
	L("    nNodes     : 13;");
	L("    prefixSize : 16;");
	L("} [[format(\"fmt_spg_inner_head\")]];");
	L("");
	L("fn fmt_spg_inner_head(ref auto h) {");
	L("    return std::format(\"{} allTheSame={} nNodes={} prefixSize={}\",");
	L("                       spgist_state_str(h.tupstate), h.allTheSame, h.nNodes, h.prefixSize);");
	L("};");
	L("");
	L("// SP-GiST node tuples reuse the IndexTuple layout for downlink labels");
	L("struct SpgNodeTuple {");
	L("    ItemPointerData t_tid;");
	F("    IndexTInfo t_info [[color(\"%s\")]];", COLOR_YELLOW_DARK);
	L("    if (t_info.size >= 8) {");
	F("        u8 label[t_info.size - 8] [[sealed, color(\"%s\")]];", COLOR_WHITE);
	L("    }");
	L("} [[format(\"fmt_index_tuple\")]];");
	L("");
	L("struct SpGistInnerTuple {");
	L("    u64 stupstart = $;");
	F("    SpgInnerHead head [[color(\"%s\")]];", COLOR_YELLOW_LIGHT);
	F("    u16 size [[color(\"%s\")]];", ShadeColor(COLOR_YELLOW_LIGHT));
	F("    if (head.tupstate == %d) {", (int) SPGIST_LIVE);
	L("        if (head.prefixSize > 0) {");
	F("            u8 prefix[head.prefixSize] [[sealed, color(\"%s\")]];", COLOR_WHITE);
	L("        }");
	L("        SpgNodeTuple nodes[head.nNodes];");
	L("    } else if (g_item_end > $) {");
	F("        u8 contents[g_item_end - $] [[sealed, color(\"%s\")]];", COLOR_BROWN);
	L("    }");
	L("} [[format(\"fmt_spg_inner\")]];");
	L("");
	L("fn fmt_spg_inner(ref auto t) { return fmt_spg_inner_head(t.head); };");
	L("");
	L("bitfield SpgLeafHead {");
	L("    tupstate : 2;");
	L("    size     : 30;");
	L("} [[format(\"fmt_spg_leaf_head\")]];");
	L("");
	L("fn fmt_spg_leaf_head(ref auto h) {");
	L("    return std::format(\"{} size={}\", spgist_state_str(h.tupstate), h.size);");
	L("};");
	L("");
	L("struct SpGistLeafTuple {");
	L("    u64 stupstart = $;");
	F("    SpgLeafHead head [[color(\"%s\")]];", COLOR_YELLOW_LIGHT);
	F("    u16 nextOffset [[color(\"%s\"), format(\"fmt_spg_nextoffset\")]];", COLOR_YELLOW_DARK);
	L("    ItemPointerData heapPtr;");
	F("    if (head.tupstate == %d) {", (int) SPGIST_LIVE);
	L("        if (stupstart + head.size > $ && head.size <= 8192) {");
	F("            u8 contents[stupstart + head.size - $] [[sealed, color(\"%s\")]];", COLOR_WHITE);
	L("        }");
	L("    } else {");
	F("        u32 xid [[color(\"%s\")]];", COLOR_RED_LIGHT);
	L("    }");
	L("} [[format(\"fmt_spg_leaf\")]];");
	L("");
	L("fn fmt_spg_nextoffset(auto v) {");
	L("    return std::format(\"{}{}\", v & 0x7FFF, (v & 0x8000) != 0 ? \" (has nullmask)\" : \"\");");
	L("};");
	L("");
	L("fn fmt_spg_leaf(ref auto t) {");
	L("    return std::format(\"{} heapPtr={}\", fmt_spg_leaf_head(t.head), fmt_ctid(t.heapPtr));");
	L("};");
	L("");
	/* ---- BRIN ---- */
	L("bitfield BrinPageFlags {");
	F("    BRIN_EVACUATE_PAGE : 1;	// 0x%04X", (unsigned int) BRIN_EVACUATE_PAGE);
	L("    padding            : 15;");
	L("} [[format(\"fmt_brin_flags\")]];");
	L("");
	L("fn fmt_brin_flags(ref auto f) {");
	L("    if (f.BRIN_EVACUATE_PAGE) return \"BRIN_EVACUATE_PAGE\";");
	L("    return \"(none)\";");
	L("};");
	L("");
	L("fn brin_pagetype_str(u16 t) {");
	L("    match (t) {");
	F("        (0x%04X): return \"BRIN_PAGETYPE_META\";", (unsigned int) BRIN_PAGETYPE_META);
	F("        (0x%04X): return \"BRIN_PAGETYPE_REVMAP\";", (unsigned int) BRIN_PAGETYPE_REVMAP);
	F("        (0x%04X): return \"BRIN_PAGETYPE_REGULAR\";", (unsigned int) BRIN_PAGETYPE_REGULAR);
	L("    }");
	L("    return \"?\";");
	L("};");
	L("");
	L("struct BrinSpecial {");
	emitOff = 0;
	EFIELD("BrinPageFlags", "flags", (int) (MAXALIGN(1) - 2 * sizeof(uint16)), 2,
		   COLOR_GREEN_BRIGHT);
	EFIELD("u16", "pagetype", (int) (MAXALIGN(1) - sizeof(uint16)), 2,
		   ShadeColor(COLOR_GREEN_BRIGHT));
	L("} [[format(\"fmt_brin_special\")]];");
	L("");
	L("fn fmt_brin_special(ref auto s) {");
	L("    return std::format(\"{} {}\", brin_pagetype_str(s.pagetype), fmt_brin_flags(s.flags));");
	L("};");
	L("");
	L("struct BrinMetaPageData {");
	emitOff = 0;
	EFIELD("u32", "brinMagic", (int) offsetof(BrinMetaPageData, brinMagic), 4, COLOR_PINK);
	EFIELD("u32", "brinVersion", (int) offsetof(BrinMetaPageData, brinVersion), 4, ShadeColor(COLOR_PINK));
	EFIELD("u32", "pagesPerRange", (int) offsetof(BrinMetaPageData, pagesPerRange), 4, COLOR_PINK);
	EFIELD("u32", "lastRevmapPage", (int) offsetof(BrinMetaPageData, lastRevmapPage), 4, ShadeColor(COLOR_PINK));
	L("} [[format(\"fmt_brin_meta\")]];");
	L("");
	L("fn fmt_brin_meta(ref auto m) {");
	L("    return std::format(\"version={} pagesPerRange={} lastRevmapPage={}\",");
	L("                       m.brinVersion, m.pagesPerRange, m.lastRevmapPage);");
	L("};");
	L("");
	L("struct BrinRevmapContents {");
	F("    ItemPointerData rm_tids[%d];", (int) REVMAP_PAGE_MAXITEMS);
	L("};");
	L("");
	L("bitfield BrinInfo {");
	F("    offset               : %d;	// BRIN_OFFSET_MASK 0x%02X",
	  __builtin_popcount(BRIN_OFFSET_MASK), (unsigned int) BRIN_OFFSET_MASK);
	L("    padding              : 1;");
	F("    BRIN_PLACEHOLDER     : 1;	// 0x%02X", (unsigned int) BRIN_PLACEHOLDER_MASK);
	F("    BRIN_NULLS           : 1;	// 0x%02X", (unsigned int) BRIN_NULLS_MASK);
	L("} [[format(\"fmt_brin_info\")]];");
	L("");
	L("fn fmt_brin_info(ref auto i) {");
	L("    str s = \"\";");
	L("    if (i.BRIN_PLACEHOLDER) s = s + \"|BRIN_PLACEHOLDER_MASK\";");
	L("    if (i.BRIN_NULLS)       s = s + \"|BRIN_NULLS_MASK\";");
	L("    return std::format(\"BrinTupleDataOffset()={}{}\", i.offset, s);");
	L("};");
	L("");
	L("struct BrinTupleData {");
	L("    u64 btupstart = $;");
	F("    u32 bt_blkno [[color(\"%s\")]];", COLOR_BLUE_LIGHT);
	F("    BrinInfo bt_info [[color(\"%s\")]];", COLOR_YELLOW_DARK);
	L("    if (bt_info.BRIN_NULLS && btupstart + bt_info.offset > $) {");
	F("        u8 nullbits[btupstart + bt_info.offset - $] [[sealed, color(\"%s\"), name(\"IndexAttributeBitMapData\")]];",
	  COLOR_YELLOW_DARK);
	L("    }");
	L("    if (g_item_end > btupstart + bt_info.offset) {");
	L("        padding[btupstart + bt_info.offset - $];");
	F("        u8 contents[g_item_end - $] [[sealed, color(\"%s\")]];", COLOR_WHITE);
	L("    }");
	L("} [[format(\"fmt_brin_tuple\")]];");
	L("");
	L("fn fmt_brin_tuple(ref auto t) {");
	L("    return std::format(\"bt_blkno={} {}\", t.bt_blkno, fmt_brin_info(t.bt_info));");
	L("};");
	L("");
		L("struct RawSpecial {");
	F("    u8 opaque[%u - g_special_off] [[sealed, color(\"%s\")]];",
	  (unsigned int) BLCKSZ, COLOR_GREEN_BRIGHT);
	L("};");
	L("");
}

/*
 * The page itself: header, page-kind classification (a pattern-language
 * port of the old GetSpecialSectionType()), line pointer slots with
 * out-of-line tuple placement, metapages, and the special section.
 */
static void
EmitPageStruct(void)
{
	unsigned int alignedu32 = MAXALIGN(sizeof(uint32));

	L("// ---------------- line pointer slots ----------------");
	L("");
	L("// Slot structs walk the line pointer array (each element consumes one");
	L("// 4-byte ItemIdData) but present the TUPLE as the primary object: the");
	L("// placed tuple is the first child, and the line pointer itself is the");
	L("// last child, for drill-down.  Line pointer fields are peeked up front");
	L("// so placement can precede the sequential ItemIdData member.");
	L("struct HeapTupleSlot {");
	L("    u32 lpraw = std::mem::read_unsigned($, 4);");
	L("    u32 p_off = lpraw & 0x7FFF;");
	L("    u8  p_flags = (lpraw >> 15) & 3;");
	L("    u32 p_len = (lpraw >> 17) & 0x7FFF;");
	L("    // LP_DEAD heap items can retain storage until pruned; annotate them");
	L("    if ((p_flags == 1 || p_flags == 3) && p_len != 0) {");
	F("        g_item_end = (($ / %u) * %u) + p_off + p_len;",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	F("        HeapTupleHeaderData htup @ (($ / %u) * %u) + p_off [[no_unique_address]];",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("    }");
	L("    ItemIdData lp;");
	L("} [[format(\"fmt_slot\")]];");
	L("");
	L("struct IndexTupleSlot {");
	L("    u32 lpraw = std::mem::read_unsigned($, 4);");
	L("    u32 p_off = lpraw & 0x7FFF;");
	L("    u8  p_flags = (lpraw >> 15) & 3;");
	L("    u32 p_len = (lpraw >> 17) & 0x7FFF;");
	L("    // Index AMs use LP_DEAD for dead-but-still-present tuples, so both");
	L("    // LP_NORMAL and LP_DEAD items have storage (p_len != 0 guards it)");
	L("    if ((p_flags == 1 || p_flags == 3) && p_len != 0) {");
	F("        g_item_end = (($ / %u) * %u) + p_off + p_len;",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	F("        IndexTupleData itup @ (($ / %u) * %u) + p_off [[no_unique_address]];",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("    }");
	L("    ItemIdData lp;");
	L("} [[format(\"fmt_slot\")]];");
	L("");
	L("struct RawItemSlot {");
	L("    u32 lpraw = std::mem::read_unsigned($, 4);");
	L("    u32 p_off = lpraw & 0x7FFF;");
	L("    u8  p_flags = (lpraw >> 15) & 3;");
	L("    u32 p_len = (lpraw >> 17) & 0x7FFF;");
	L("    if (p_len != 0 && p_flags != 0) {");
	F("        u8 item[p_len] @ (($ / %u) * %u) + p_off [[sealed, no_unique_address, color(\"%s\")]];",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ, COLOR_WHITE);
	L("    }");
	L("    ItemIdData lp;");
	L("} [[format(\"fmt_slot\")]];");
	L("");
	L("struct SpgSlot {");
	L("    u32 lpraw = std::mem::read_unsigned($, 4);");
	L("    u32 p_off = lpraw & 0x7FFF;");
	L("    u8  p_flags = (lpraw >> 15) & 3;");
	L("    u32 p_len = (lpraw >> 17) & 0x7FFF;");
	L("    if (p_len != 0 && p_flags != 0) {");
	F("        g_item_end = (($ / %u) * %u) + p_off + p_len;",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("        if (g_isleaf) {");
	F("            SpGistLeafTuple sleaf @ (($ / %u) * %u) + p_off [[no_unique_address]];",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("        } else {");
	F("            SpGistInnerTuple sinner @ (($ / %u) * %u) + p_off [[no_unique_address]];",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("        }");
	L("    }");
	L("    ItemIdData lp;");
	L("} [[format(\"fmt_slot\")]];");
	L("");
	L("struct BrinSlot {");
	L("    u32 lpraw = std::mem::read_unsigned($, 4);");
	L("    u32 p_off = lpraw & 0x7FFF;");
	L("    u8  p_flags = (lpraw >> 15) & 3;");
	L("    u32 p_len = (lpraw >> 17) & 0x7FFF;");
	L("    if (p_len != 0 && p_flags == 1) {");
	F("        g_item_end = (($ / %u) * %u) + p_off + p_len;",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	F("        BrinTupleData btup @ (($ / %u) * %u) + p_off [[no_unique_address]];",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("    }");
	L("    ItemIdData lp;");
	L("} [[format(\"fmt_slot\")]];");
	L("");
	L("fn fmt_slot(ref auto s) { return fmt_itemid(s.lp); };");
	L("");
	L("// ---------------- pages ----------------");
	L("");
	L("fn page_kind_str(u8 kind) {");
	L("    match (kind) {");
	F("        (%d): return \"heap\";", KIND_HEAP);
	F("        (%d): return \"sequence\";", KIND_SEQUENCE);
	F("        (%d): return \"nbtree\";", KIND_BTREE);
	F("        (%d): return \"hash\";", KIND_HASH);
	F("        (%d): return \"GiST\";", KIND_GIST);
	F("        (%d): return \"GIN\";", KIND_GIN);
	F("        (%d): return \"SP-GiST\";", KIND_SPGIST);
	F("        (%d): return \"BRIN\";", KIND_BRIN);
	L("    }");
	L("    return \"unknown\";");
	L("};");
	L("");
	L("struct Page {");
	L("    u64 pagebase = $;");
	F("    g_blk = ($ / %u) + BASE_BLOCK;", (unsigned int) BLCKSZ);
	L("    PageHeaderData header;");
	L("");
	F("    bool intagrange = (pagebase / %u) >= FIRST_TAG_BLOCK && (pagebase / %u) <= LAST_TAG_BLOCK;",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("");
	L("    // ---- page kind classification (GetSpecialSectionType() port) ----");
	F("    g_kind = %d;", KIND_UNKNOWN);
	L("    g_special_off = header.pd_special;");
	F("    u32 specialsize = %u - header.pd_special;", (unsigned int) BLCKSZ);
	F("    u16 pagetype = std::mem::read_unsigned(pagebase + %u, 2);",
	  (unsigned int) (BLCKSZ - sizeof(uint16)));
	L("");
	F("    bool validpage = header.pd_pagesize_version != 0 && header.pd_special <= %u &&",
	  (unsigned int) BLCKSZ);
	F("        header.pd_lower >= %u && header.pd_lower <= %u;",
	  (unsigned int) sizeof(PageHeaderData), (unsigned int) BLCKSZ);
	L("    if (validpage) {");
	L("        if (specialsize == 0) {");
	F("            g_kind = %d;", KIND_HEAP);
	L("        }");
	F("        else if (specialsize == %u && std::mem::read_unsigned(pagebase + header.pd_special, 4) == 0x%04X) {",
	  alignedu32, (unsigned int) SEQUENCE_MAGIC);
	F("            g_kind = %d;", KIND_SEQUENCE);
	L("        }");
	F("        else if (specialsize == %u && pagetype == 0x%04X) {",
	  (unsigned int) MAXALIGN(sizeof(SpGistPageOpaqueData)),
	  (unsigned int) SPGIST_PAGE_ID);
	F("            g_kind = %d;", KIND_SPGIST);
	L("        }");
	F("        else if (specialsize == %u && (pagetype == 0x%04X || pagetype == 0x%04X || pagetype == 0x%04X)) {",
	  (unsigned int) MAXALIGN(sizeof(BrinSpecialSpace)),
	  (unsigned int) BRIN_PAGETYPE_META, (unsigned int) BRIN_PAGETYPE_REVMAP,
	  (unsigned int) BRIN_PAGETYPE_REGULAR);
	F("            g_kind = %d;", KIND_BRIN);
	L("        }");
	F("        else if (specialsize == %u) {",
	  (unsigned int) MAXALIGN(sizeof(GinPageOpaqueData)));
	F("            g_kind = %d;", KIND_GIN);
	L("        }");
	F("        else if (specialsize == %u && pagetype <= 0x%04X) {",
	  (unsigned int) MAXALIGN(sizeof(BTPageOpaqueData)),
	  (unsigned int) MAX_BT_CYCLE_ID);
	F("            g_kind = %d;", KIND_BTREE);
	L("        }");
	F("        else if (specialsize == %u && pagetype == 0x%04X) {",
	  (unsigned int) MAXALIGN(sizeof(HashPageOpaqueData)),
	  (unsigned int) HASHO_PAGE_ID);
	F("            g_kind = %d;", KIND_HASH);
	L("        }");
	F("        else if (specialsize == %u && pagetype == 0x%04X) {",
	  (unsigned int) MAXALIGN(sizeof(GISTPageOpaqueData)),
	  (unsigned int) GIST_PAGE_ID);
	F("            g_kind = %d;", KIND_GIST);
	L("        }");
	L("    }");
	L("");
	L("");
	L("    // Persist kind/validity as (calculated) members so the naming pass");
	L("    // can label pages accurately -- in particular so empty/pad pages");
	L("    // are not mislabeled as heap.");
	L("    u8 page_kind = g_kind;");
	L("    bool page_valid = validpage;");
	L("");
	L("    // ---- page body ----");
	L("    if (validpage && intagrange) {");
	F("        u32 nlinp = (header.pd_lower - %u) / %u;",
	  (unsigned int) offsetof(PageHeaderData, pd_linp),
	  (unsigned int) sizeof(ItemIdData));
	L("");
	L("        // Per-AM page state, peeked from the special section");
	F("        bool ismeta = g_blk == 0 && (g_kind == %d || g_kind == %d || g_kind == %d || g_kind == %d || g_kind == %d);",
	  KIND_BTREE, KIND_HASH, KIND_GIN, KIND_SPGIST, KIND_BRIN);
	L("        bool skiptuples = false;");
	L("        bool hashbitmap = false;");
	L("        bool gindata = false;");
	L("        u16 amflags = 0;");
	L("        g_isleaf = false;");
	F("        if (g_kind == %d) {", KIND_HASH);
	F("            amflags = std::mem::read_unsigned(pagebase + header.pd_special + %d, 2);",
	  (int) offsetof(HashPageOpaqueData, hasho_flag));
	F("            if ((amflags & 0x%04X) != 0) hashbitmap = true;", (unsigned int) LH_BITMAP_PAGE);
	L("        }");
	F("        if (g_kind == %d) {", KIND_GIST);
	F("            amflags = std::mem::read_unsigned(pagebase + header.pd_special + %d, 2);",
	  (int) offsetof(GISTPageOpaqueData, flags));
	F("            if ((amflags & 0x%04X) != 0) skiptuples = true;	// F_DELETED", (unsigned int) F_DELETED);
	L("        }");
	F("        if (g_kind == %d) {", KIND_SPGIST);
	F("            amflags = std::mem::read_unsigned(pagebase + header.pd_special + %d, 2);",
	  (int) offsetof(SpGistPageOpaqueData, flags));
	F("            if ((amflags & 0x%04X) != 0) skiptuples = true;	// SPGIST_DELETED", (unsigned int) SPGIST_DELETED);
	F("            if ((amflags & 0x%04X) != 0) g_isleaf = true;", (unsigned int) SPGIST_LEAF);
	L("        }");
	F("        if (g_kind == %d) {", KIND_GIN);
	F("            amflags = std::mem::read_unsigned(pagebase + header.pd_special + %d, 2);",
	  (int) offsetof(GinPageOpaqueData, flags));
	F("            if ((amflags & 0x%04X) != 0) skiptuples = true;	// GIN_DELETED", (unsigned int) GIN_DELETED);
	F("            if ((amflags & 0x%04X) != 0) g_isleaf = true;", (unsigned int) GIN_LEAF);
	F("            if ((amflags & 0x%04X) != 0) gindata = true;", (unsigned int) GIN_DATA);
	L("        }");
	L("");
	L("        if (ismeta) {");
	F("            if (g_kind == %d) {", KIND_BTREE);
	L("                BTMetaPageData btree_meta;");
	F("            } else if (g_kind == %d) {", KIND_HASH);
	L("                HashMetaPageData hash_meta;");
	F("            } else if (g_kind == %d) {", KIND_GIN);
	L("                GinMetaPageData gin_meta;");
	F("            } else if (g_kind == %d) {", KIND_SPGIST);
	L("                SpGistMetaPageData spgist_meta;");
	F("            } else if (g_kind == %d) {", KIND_BRIN);
	L("                BrinMetaPageData brin_meta;");
	L("            }");
	L("        }");
	F("        else if (g_kind == %d && hashbitmap) {", KIND_HASH);
	F("            g_content_len = header.pd_lower - %u;",
	  (unsigned int) MAXALIGN(sizeof(PageHeaderData)));
	L("            HashBitmapContent hash_bitmap;");
	L("        }");
	F("        else if (g_kind == %d && gindata && !skiptuples) {", KIND_GIN);
	L("            // Posting tree pages have no line pointer array");
	L("            if (!g_isleaf) {");
	F("                u16 ginmaxoff = std::mem::read_unsigned(pagebase + header.pd_special + %d, 2);",
	  (int) offsetof(GinPageOpaqueData, maxoff));
	F("                if (ginmaxoff * %d <= %u) {",
	  (int) sizeof(PostingItem), (unsigned int) BLCKSZ);
	L("                    GinPostingItem posting_items[ginmaxoff];");
	L("                }");
	F("            } else if ((amflags & 0x%04X) != 0) {", (unsigned int) GIN_COMPRESSED);
	L("                GinPostingList posting_lists[while($ < pagebase + header.pd_lower)];");
	L("            }");
	L("        }");
	F("        else if (skiptuples) {");
	L("            // deleted page: annotate header/special only");
	L("        }");
	F("        else if (g_kind == %d || g_kind == %d) {", KIND_HEAP, KIND_SEQUENCE);
	L("            HeapTupleSlot linp[nlinp];");
	L("        }");
	F("        else if (g_kind == %d && pagetype == 0x%04X) {",
	  KIND_BRIN, (unsigned int) BRIN_PAGETYPE_REVMAP);
	L("            BrinRevmapContents revmap;");
	L("        }");
	F("        else if (g_kind == %d || g_kind == %d || g_kind == %d || g_kind == %d) {",
	  KIND_BTREE, KIND_HASH, KIND_GIST, KIND_GIN);
	L("            IndexTupleSlot linp[nlinp];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_SPGIST);
	L("            SpgSlot linp[nlinp];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_BRIN);
	L("            BrinSlot linp[nlinp];");
	L("        }");
	F("        else if (g_kind != %d) {", KIND_UNKNOWN);
	L("            RawItemSlot linp[nlinp];");
	L("        }");
	L("");
	L("        // ---- special section ----");
	F("        if (g_kind == %d) {", KIND_BTREE);
	L("            BTPageOpaqueData btree_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_HASH);
	L("            HashPageOpaqueData hash_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_GIST);
	L("            GISTPageOpaqueData gist_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_GIN);
	L("            GinPageOpaqueData gin_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_SPGIST);
	L("            SpGistPageOpaqueData spgist_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_BRIN);
	L("            BrinSpecial brin_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind == %d) {", KIND_SEQUENCE);
	L("            SequenceMagic sequence_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	F("        else if (g_kind != %d && specialsize > 0) {", KIND_HEAP);
	L("            RawSpecial raw_special @ pagebase + header.pd_special [[no_unique_address]];");
	L("        }");
	L("    }");
	L("");
	F("    padding[%u - ($ %% %u)];", (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	L("} [[format(\"fmt_page\")]];");
	L("");
	L("fn fmt_page(ref auto p) {");
	L("    return std::format(\"lower={} upper={} special={}\",");
	L("                       p.header.pd_lower, p.header.pd_upper, p.header.pd_special);");
	L("};");
	L("");
	L("// ---------------- provider cache flush + live sizing ----------------");
	L("// ImHex's GDB Server provider caches reads (1024 direct-mapped 4KiB");
	L("// lines, no reload).  pg_hexedit_live_bridge advertises an 8MiB pad");
	L("// past the relation and tags the pad's last line with a magic value");
	L("// and the live relation size.  Each evaluation:");
	L("//   1. flushes the metadata line's cache slot via a pad alias,");
	L("//   2. reads the live relation size (so growth is tracked),");
	L("//   3. flushes exactly the slots that can hold relation lines, by");
	L("//      reading pad addresses that alias them (blockIndex mod 1024).");
	L("// On plain files there is no pad: every alias lands past the end of");
	L("// the file and the whole sweep self-disables at zero cost.");
	L("u64 cf_size = std::mem::size();");
	L("u64 cf_lines = cf_size / 4096;");
	L("");
	L("// (1) make the metadata line's slot fetch fresh");
	L("if (cf_lines >= 1025) {");
	L("    std::mem::read_unsigned((cf_lines - 1 - 1024) * 4096, 1);");
	L("}");
	L("");
	L("// (2) live relation size from the bridge tag, else file size");
	F("u64 g_npages = cf_size / %u;", (unsigned int) BLCKSZ);
	L("bool cf_bridged = false;");
	L("if (cf_size >= 4096 &&");
	L("    std::mem::read_unsigned(cf_size - 4096, 8) == 0x3147524258484750) {");
	F("    g_npages = std::mem::read_unsigned(cf_size - 4088, 8) / %u;",
	  (unsigned int) BLCKSZ);
	L("    cf_bridged = true;");
	L("}");
	L("");
	L("// (3) flush the relation's cache slots through their pad aliases");
	F("u64 cf_rel_lines = g_npages * %u;", (unsigned int) (BLCKSZ / 4096));
	L("u64 cf_nflush = cf_rel_lines;");
	L("if (cf_nflush > 1024) cf_nflush = 1024;");
	L("u64 cf_p0 = ((cf_rel_lines + 1023) / 1024) * 1024;");
	L("u64 cf_addr = 0;");
	L("if (cf_bridged) {");
	L("    for (u64 cf = 0, cf < cf_nflush, cf += 1) {");
	L("        cf_addr = (cf_p0 + cf) * 4096;");
	L("        if (cf_addr < cf_size) {");
	L("            std::mem::read_unsigned(cf_addr, 1);");
	L("        }");
	L("    }");
	L("} else if (cf_size < 0x800000) {");
	L("    // Bridge provider created without the padded size?  (Plain small");
	L("    // files also land here; the note is only relevant to the bridge.)");
	L("    std::print(\"pg_hexedit: no live-bridge tag found; if this is the GDB provider, re-create it with the padded size the bridge prints (File -> Recent has a pre-configured entry), or re-evaluation will show stale bytes\");");
	L("}");
	L("");
	L("// Page count.  g_npages (from the live bridge's size tag, else the");
	L("// data-source size) is the primary bound.  As a safety net against a");
	L("// live provider whose size does not match the bridge, stop at the");
	L("// bridge's flush-pad sentinel: the bridge fills pad pages with 0xFF, so");
	L("// pd_pagesize_version reads 0xFFFF there -- a value no real page has.");
	L("// Real pages, including all-zero uninitialized holes, are kept (0 is");
	L("// not the sentinel), and plain files never contain the sentinel.");
	F("Page pages[while(($ / %u) < g_npages && $ + %u <= std::mem::size() &&",
	  (unsigned int) BLCKSZ, (unsigned int) BLCKSZ);
	F("                std::mem::read_unsigned($ + %u, 2) != 0xFFFF)] @ 0x00;",
	  (unsigned int) offsetof(PageHeaderData, pd_pagesize_version));
	L("");
}

/*
 * Naming and coloring pass.  Runs from global scope, where pattern
 * references only bind to composite patterns via full paths (the stock
 * elf.hexpat idiom).  Gives every slot its 1-based offset number, every
 * tuple its own TID as display name and hover comment (like the old tool's
 * "(blkno,offset)" tag prefix), and line pointers their state colors, with
 * parity shading so adjacent line pointers stay visually separable.
 */
static void
EmitNamingPass(void)
{
	L("// ---------------- naming + coloring pass ----------------");
	L("");
	L("u32 lpcolor = 0;");
	L("u32 pageblk = 0;");
	L("str pagekind = \"\";");
	L("bool hastup = false;");
	L("");
	L("for (u32 p = 0, p < std::core::member_count(pages), p += 1) {");
	L("    pageblk = p + BASE_BLOCK;");
	L("    if (std::core::has_member(pages[p], \"linp\")) {");
	L("        for (u32 s = 0, s < std::core::member_count(pages[p].linp), s += 1) {");
	L("            // The slot row carries the tuple's identity; the line");
	L("            // pointer is the drill-down (its own row sits last inside)");
	L("            hastup = std::core::has_member(pages[p].linp[s], \"htup\") ||");
	L("                     std::core::has_member(pages[p].linp[s], \"itup\") ||");
	L("                     std::core::has_member(pages[p].linp[s], \"sleaf\") ||");
	L("                     std::core::has_member(pages[p].linp[s], \"sinner\") ||");
	L("                     std::core::has_member(pages[p].linp[s], \"btup\");");
	L("            if (hastup) {");
	L("                std::core::set_display_name(pages[p].linp[s],");
	L("                    std::format(\"tuple ({},{})\", pageblk, s + 1));");
	L("                std::core::set_pattern_comment(pages[p].linp[s],");
	L("                    std::format(\"lp {}\", s + 1));");
	L("            } else {");
	L("                std::core::set_display_name(pages[p].linp[s],");
	L("                    std::format(\"lp {}\", s + 1));");
	L("            }");
	L("");
	L("            // Line pointer state colors (parity-shaded within a state");
	L("            // so adjacent same-state line pointers remain separable)");
	L("            if (pages[p].linp[s].lp.lp_flags == 0)");
	F("                lpcolor = (s & 1) == 0 ? %s : %s;	// LP_UNUSED: muted gray",
	  AbgrLit(COLOR_GRAY), AbgrLit(ShadeColor(COLOR_GRAY)));
	L("            else if (pages[p].linp[s].lp.lp_flags == 2)");
	F("                lpcolor = (s & 1) == 0 ? %s : %s;	// BLUE_DARK",
	  AbgrLit(COLOR_BLUE_DARK), AbgrLit(ShadeColor(COLOR_BLUE_DARK)));
	L("            else if (pages[p].linp[s].lp.lp_flags == 3)");
	F("                lpcolor = (s & 1) == 0 ? %s : %s;	// BROWN",
	  AbgrLit(COLOR_BROWN), AbgrLit(ShadeColor(COLOR_BROWN)));
	L("            else");
	F("                lpcolor = (s & 1) == 0 ? %s : %s;	// BLUE_LIGHT",
	  AbgrLit(COLOR_BLUE_LIGHT), AbgrLit(ShadeColor(COLOR_BLUE_LIGHT)));
	L("            std::core::set_pattern_color(pages[p].linp[s].lp, lpcolor);");
	L("");
	L("            if (std::core::has_member(pages[p].linp[s], \"htup\"))");
	L("                std::core::set_pattern_comment(pages[p].linp[s].htup,");
	L("                    std::format(\"TID ({},{})\", pageblk, s + 1));");
	L("            if (std::core::has_member(pages[p].linp[s], \"sleaf\"))");
	L("                std::core::set_pattern_comment(pages[p].linp[s].sleaf,");
	L("                    std::format(\"leaf tuple ({},{})\", pageblk, s + 1));");
	L("            if (std::core::has_member(pages[p].linp[s], \"sinner\"))");
	L("                std::core::set_pattern_comment(pages[p].linp[s].sinner,");
	L("                    std::format(\"inner tuple ({},{})\", pageblk, s + 1));");
	L("            if (std::core::has_member(pages[p].linp[s], \"btup\"))");
	L("                std::core::set_pattern_comment(pages[p].linp[s].btup,");
	L("                    std::format(\"brin tuple ({},{})\", pageblk, s + 1));");
	L("            if (std::core::has_member(pages[p].linp[s], \"itup\")) {");
	L("                if (pages[p].linp[s].lp.lp_flags == 3)");
	L("                    std::core::set_pattern_comment(pages[p].linp[s].itup,");
	L("                        std::format(\"index tuple ({},{}) LP_DEAD\", pageblk, s + 1));");
	L("                else");
	L("                    std::core::set_pattern_comment(pages[p].linp[s].itup,");
	L("                        std::format(\"index tuple ({},{})\", pageblk, s + 1));");
	L("");
	L("                // nbtree posting list TIDs alternate colors per entry,");
	L("                // as the old tool did, so runs of TIDs stay readable");
	L("                if (std::core::has_member(pages[p].linp[s].itup, \"posting_tids\")) {");
	L("                    for (u32 t = 0, t < std::core::member_count(pages[p].linp[s].itup.posting_tids), t += 1) {");
	L("                        if (pages[p].linp[s].lp.lp_flags == 3)");
	F("                            lpcolor = %s;	// LP_DEAD: BROWN", AbgrLit(COLOR_BROWN));
	L("                        else if ((t & 1) == 0)");
	F("                            lpcolor = %s;	// RED_LIGHT", AbgrLit(COLOR_RED_LIGHT));
	L("                        else");
	F("                            lpcolor = %s;	// GREEN_LIGHT", AbgrLit(COLOR_GREEN_LIGHT));
	L("                        std::core::set_pattern_color(pages[p].linp[s].itup.posting_tids[t], lpcolor);");
	L("                    }");
	L("                }");
	L("");
	L("                // LP_DEAD index tuples keep storage; paint the whole");
	L("                // tuple in the line pointer's dead color, as the old");
	L("                // tool did with its black-tag/brown-font scheme");
	L("                if (pages[p].linp[s].lp.lp_flags == 3) {");
	F("                    std::core::set_pattern_color(pages[p].linp[s].itup, %s);",
	  AbgrLit(COLOR_BROWN));
	F("                    std::core::set_pattern_color(pages[p].linp[s].itup.t_tid, %s);",
	  AbgrLit(COLOR_BROWN));
	F("                    std::core::set_pattern_color(pages[p].linp[s].itup.t_info, %s);",
	  AbgrLit(COLOR_BROWN));
	L("                    if (std::core::has_member(pages[p].linp[s].itup, \"attrs\")) {");
	for (int k = 0; k < nrelatts; k++)
	{
		attClass	class = attclassrel[k];
		char	   *plname = attplnamerel[k];

		/*
		 * set_pattern_color binds only to struct and bitfield patterns;
		 * arrays and primitives decay to values.  Since scalar columns are
		 * bare members and name/cstring/opaque are bare arrays, only the
		 * struct-wrapped classes (point, varlena) can be recolored here.
		 * The rest keep their static hashed color on dead tuples -- the dead
		 * state is still carried by the line pointer and the tuple header
		 * (t_tid, t_info), which are recolored above.
		 */
		if (class != ATT_CLASS_POINT &&
			class != ATT_CLASS_VARLENA_TEXT &&
			class != ATT_CLASS_VARLENA_BIN)
			continue;

		F("                        if (std::core::has_member(pages[p].linp[s].itup.attrs, \"%s\"))",
		  plname);
		F("                            std::core::set_pattern_color(pages[p].linp[s].itup.attrs.%s, %s);",
		  plname, AbgrLit(COLOR_BROWN));
	}
	L("                    }");
	L("                }");
	L("            }");
	L("        }");
	L("    }");
	L("");
	L("    // Page label: derived from what the page actually is.  A page with");
	L("    // no recognized special section is only \"heap\" if it is a valid");
	L("    // page classified as heap; invalid/uninitialized pages (e.g. the");
	L("    // live bridge's flush pad) must not be mislabeled heap.");
	L("    pagekind = \"unrecognized\";");
	L("    if (!pages[p].page_valid)");
	L("        pagekind = \"empty/uninitialized\";");
	F("    else if (pages[p].page_kind == %d)", KIND_HEAP);
	L("        pagekind = \"heap\";");
	L("    else if (std::core::has_member(pages[p], \"btree_meta\"))");
	L("        pagekind = \"nbtree metapage\";");
	L("    else if (std::core::has_member(pages[p], \"btree_special\"))");
	L("        pagekind = std::format(\"nbtree level {}\", pages[p].btree_special.btpo_level);");
	L("    else if (std::core::has_member(pages[p], \"hash_meta\"))");
	L("        pagekind = \"hash metapage\";");
	L("    else if (std::core::has_member(pages[p], \"hash_bitmap\"))");
	L("        pagekind = \"hash bitmap\";");
	L("    else if (std::core::has_member(pages[p], \"hash_special\"))");
	L("        pagekind = std::format(\"hash bucket {}\", pages[p].hash_special.hasho_bucket);");
	L("    else if (std::core::has_member(pages[p], \"gin_meta\"))");
	L("        pagekind = \"GIN metapage\";");
	L("    else if (std::core::has_member(pages[p], \"posting_items\"))");
	L("        pagekind = \"GIN posting tree internal\";");
	L("    else if (std::core::has_member(pages[p], \"posting_lists\"))");
	L("        pagekind = \"GIN posting tree leaf\";");
	L("    else if (std::core::has_member(pages[p], \"gin_special\"))");
	L("        pagekind = \"GIN entry tree\";");
	L("    else if (std::core::has_member(pages[p], \"gist_special\"))");
	L("        pagekind = \"GiST\";");
	L("    else if (std::core::has_member(pages[p], \"spgist_meta\"))");
	L("        pagekind = \"SP-GiST metapage\";");
	L("    else if (std::core::has_member(pages[p], \"spgist_special\"))");
	L("        pagekind = \"SP-GiST\";");
	L("    else if (std::core::has_member(pages[p], \"brin_meta\"))");
	L("        pagekind = \"BRIN metapage\";");
	L("    else if (std::core::has_member(pages[p], \"revmap\"))");
	L("        pagekind = \"BRIN revmap\";");
	L("    else if (std::core::has_member(pages[p], \"brin_special\"))");
	L("        pagekind = \"BRIN regular\";");
	L("    else if (std::core::has_member(pages[p], \"sequence_special\"))");
	L("        pagekind = \"sequence\";");
	L("    else if (std::core::has_member(pages[p], \"raw_special\"))");
	L("        pagekind = \"index\";");
	L("    std::core::set_display_name(pages[p],");
	L("        std::format(\"block {} ({})\", pageblk, pagekind));");
	L("}");
	L("");
}

int
main(int argv, char **argc)
{
	if (!ConsumeOptions(argv, argc))
	{
		DisplayOptions();
		exit(exitCode != 0 ? exitCode : 1);
	}

	EmitProlog(argv, argc);
	EmitPageBasics();
	EmitTupleAttrs();
	EmitHeapStructs();
	EmitIndexStructs();
	EmitSpecialStructs();
	EmitPageStruct();
	EmitNamingPass();

	if (exitCode == 0)
		fprintf(stderr, "pg_hexedit notice: PostgreSQL frontend program return code is 0 (success)\n");
	else
		fprintf(stderr, "pg_hexedit notice: PostgreSQL frontend program return code is %d (failure)\n",
				exitCode);

	exit(exitCode);
}

/*
 * pg_hexedit.c - PostgreSQL file dump utility for
 *                viewing heap (data) and index files in wxHexEditor.
 *
 * Copyright (c) 2018-2021, Crunchy Data Solutions, Inc.
 * Copyright (c) 2017-2018, VMware, Inc.
 * Copyright (c) 2002-2010, Red Hat, Inc.
 * Copyright (c) 2011-2021, PostgreSQL Global Development Group
 *
 * This specialized fork of pg_filedump is modified to output XML that can be
 * used to annotate pages within the wxHexEditor hex editor.
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
#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#include "utils/pg_crc.h"

#define HEXEDIT_VERSION			"0.1"
#define SEQUENCE_MAGIC			0x1717	/* PostgreSQL defined magic number */
#define EOF_ENCOUNTERED 		(-1)	/* Indicator for partial read */

/* Postgres 12 removed WITH OIDS support.  Preserve compatibility. */
#if PG_VERSION_NUM >= 120000
#define HEAP_HASOID	HEAP_HASOID_OLD
#endif

/* Postgres 13 renamed BT_OFFSET_MASK.  Preserve compatibility. */
#if PG_VERSION_NUM < 130000
#define BT_OFFSET_MASK	BT_N_KEYS_OFFSET_MASK
#endif

#define COLOR_FONT_STANDARD		"#313739"

#define COLOR_BLACK				"#000000"
#define COLOR_BLUE_DARK			"#2980B9"
#define COLOR_BLUE_LIGHT		"#3498DB"
#define COLOR_BROWN				"#97333D"
#define COLOR_GREEN_BRIGHT		"#50E964"
#define COLOR_GREEN_DARK		"#16A085"
#define COLOR_GREEN_LIGHT		"#1ABC9C"
#define COLOR_MAROON			"#E96950"
#define COLOR_ORANGE			"#FF8C00"
#define COLOR_PINK				"#E949D1"
#define COLOR_RED_DARK			"#912C21"
#define COLOR_RED_LIGHT			"#E74C3C"
#define COLOR_WHITE				"#CCD1D1"
#define COLOR_YELLOW_DARK		"#F1C40F"
#define COLOR_YELLOW_LIGHT		"#E9E850"

typedef enum blockSwitches
{
	BLOCK_RANGE = 0x00000020,	/* -R: Specific block range to dump */
	BLOCK_CHECKSUMS = 0x00000040,	/* -k: verify block checksums */
	BLOCK_ZEROSUMS = 0x00000080,	/* -z: verify block checksums when
									 * non-zero */
	BLOCK_SKIP_LEAF = 0x00000100,	/* -l: Skip leaf pages (use whole page
									 * tag) */
	BLOCK_SKIP_LSN = 0x00000200,	/* -x: Skip pages before LSN */
	BLOCK_DECODE = 0x00000400	/* -D: Decode tuple attributes */
} blockSwitches;

typedef enum segmentSwitches
{
	SEGMENT_SIZE_FORCED = 0x00000001,	/* -s: Segment size forced */
	SEGMENT_NUMBER_FORCED = 0x00000002	/* -n: Segment number forced */
} segmentSwitches;

/* -R[start]:Block range start */
static int	blockStart = -1;

/* -R[end]:Block range end */
static int	blockEnd = -1;

/* -x:Skip pages whose LSN is before point */
static XLogRecPtr afterThreshold = 0;

/* Possible value types for the Special Section */
typedef enum specialSectionTypes
{
	SPEC_SECT_NONE,				/* No special section on block */
	SPEC_SECT_SEQUENCE,			/* Sequence info in special section */
	SPEC_SECT_INDEX_BTREE,		/* BTree index info in special section */
	SPEC_SECT_INDEX_HASH,		/* Hash index info in special section */
	SPEC_SECT_INDEX_GIST,		/* GIST index info in special section */
	SPEC_SECT_INDEX_GIN,		/* GIN index info in special section */
	SPEC_SECT_INDEX_SPGIST,		/* SP - GIST index info in special section */
	SPEC_SECT_INDEX_BRIN,		/* BRIN index info in special section */
	SPEC_SECT_ERROR_UNKNOWN,	/* Unknown error */
	SPEC_SECT_ERROR_BOUNDARY	/* Boundary error */
} specialSectionTypes;

/* Special section type that was encountered first */
static unsigned int firstType = SPEC_SECT_ERROR_UNKNOWN;

/* Current block special section type */
static unsigned int specialType = SPEC_SECT_NONE;

/*
 * Possible return codes from option validation routine.
 *
 * pg_hexedit doesn't do much with them now but maybe in the future...
 */
typedef enum optionReturnCodes
{
	OPT_RC_VALID,				/* All options are valid */
	OPT_RC_INVALID,				/* Improper option string */
	OPT_RC_FILE,				/* File problems */
	OPT_RC_DUPLICATE,			/* Duplicate option encountered */
	OPT_RC_COPYRIGHT			/* Copyright should be displayed */
} optionReturnCodes;

/*
 * Simple macro to check for duplicate options and then set an option flag for
 * later consumption
 */
#define SET_OPTION(_x,_y,_z) if (_x & _y)				\
							   {						\
								 rc = OPT_RC_DUPLICATE; \
								 duplicateSwitch = _z;	\
							   }						\
							 else						\
							   _x |= _y;

/*
 * Global variables for ease of use mostly
 */

/* Segment-related options */
static unsigned int segmentOptions = 0;

/*	Options for Block formatting operations */
static unsigned int blockOptions = 0;

/* File to dump or format */
static FILE *fp = NULL;

/* File name for display */
static char *fileName = NULL;

/* Cache for current block */
static char *buffer = NULL;

/* Current block size */
static unsigned int blockSize = 0;

/* Current block in file */
static unsigned int currentBlock = 0;

/* Segment size in bytes */
static unsigned int segmentSize = RELSEG_SIZE * BLCKSZ;

/* Relation-relative block offset to beginning of the segment (our file) */
static unsigned int segmentBlockDelta = 0;

/* Number of current segment */
static unsigned int segmentNumber = 0;

/* Current wxHexEditor output tag number */
static unsigned int tagNumber = 0;

/* Offset of current block (in bytes) */
static unsigned int pageOffset = 0;

/* Number of bytes to format */
static unsigned int bytesToFormat = 0;

/* Block version number */
static unsigned int blockVersion = 0;

/* Number of attributes (used when decoding) */
static int	nrelatts = 0;

/* attlen catalog metadata for relation (used when decoding) */
static int *attlenrel = NULL;

/* attnamerel catalog metadata for relation (used when decoding) */
static char **attnamerel = NULL;

/* attcolorrel attribute colors for relation (used when decoding) */
static char **attcolorrel = NULL;

/* attalign catalog metadata for relation (used when decoding) */
static char *attalignrel = NULL;

/* Program exit code */
static int	exitCode = 0;

typedef enum formatChoice
{
	ITEM_HEAP,					/* Blocks contain HeapTuple items */
	ITEM_INDEX,					/* Blocks contain IndexTuple items */
	ITEM_SPG_INN,				/* Blocks contain SpGistInnerTuple items */
	ITEM_SPG_LEAF,				/* Blocks contain SpGistLeafTuple items */
	ITEM_BRIN					/* Blocks contain BrinTuple items */
} formatChoice;

static void DisplayOptions(unsigned int validOptions);
static unsigned int GetSegmentNumberFromFileName(const char *fileName);
static uint32 sdbmhash(const unsigned char *elem, size_t len);
static char *GetColorFromAttrname(const char *attrName);
static unsigned int ConsumeOptions(int numOptions, char **options);
static int	GetOptionValue(char *optionString);
static XLogRecPtr GetOptionXlogRecPtr(char *optionString);
static bool ParseAttributeListString(const char *str);
static unsigned int GetBlockSize(void);
static unsigned int GetSpecialSectionType(Page page);
static const char *GetSpecialSectionString(unsigned int type);
static XLogRecPtr GetPageLsn(Page page);
static char *GetHeapTupleHeaderFlags(HeapTupleHeader htup, bool isInfomask2);
static char *GetIndexTupleFlags(IndexTuple itup);
static const char *GetSpGistStateString(unsigned int code);
static char *GetSpGistInnerTupleState(SpGistInnerTuple itup);
static char *GetSpGistLeafTupleState(SpGistLeafTuple itup);
static char *GetBrinTupleFlags(BrinTuple *itup);
static bool IsBrinPage(Page page);
static bool IsHashBitmapPage(Page page);
static bool IsLeafPage(Page page);
static void EmitXmlPage(BlockNumber blkno);
static void EmitXmlDocHeader(int numOptions, char **options);
static void EmitXmlFooter(void);
static void EmitXmlTag(BlockNumber blkno, uint32 level, const char *name,
					   const char *color, uint32 relfileOff,
					   uint32 relfileOffEnd);
static void EmitXmlItemId(BlockNumber blkno, OffsetNumber offset,
						  ItemId itemId, uint32 relfileOff,
						  const char *textFlags);
static inline void EmitXmlTupleTag(BlockNumber blkno, OffsetNumber offset,
								   const char *name, const char *color,
								   uint32 relfileOff, uint32 relfileOffEnd);
static void EmitXmlTupleTagFont(BlockNumber blkno, OffsetNumber offset,
								const char *name, const char *color,
								const char *fontColor, uint32 relfileOff,
								uint32 relfileOffEnd);
static void EmitXmlTupleTagFontTwoName(BlockNumber blkno, OffsetNumber offset,
									   const char *name1, const char *name2,
									   const char *color, const char *fontColor,
									   uint32 relfileOff, uint32 relfileOffEnd);
static void EmitXmlAttributesHeap(BlockNumber blkno, OffsetNumber offset,
								  uint32 relfileOff, HeapTupleHeader htup,
								  int itemSize);
static void EmitXmlAttributesIndex(BlockNumber blkno, OffsetNumber offset,
								   uint32 relfileOff, IndexTuple itup,
								   uint32 tupHeaderOff, int itemSize);
static void EmitXmlAttributesData(BlockNumber blkno, OffsetNumber offset,
								  uint32 relfileOff, unsigned char *tupdata,
								  bits8 *t_bits, int nattrs, int datalen);
static void EmitXmlHeapTuple(BlockNumber blkno, OffsetNumber offset,
							 HeapTupleHeader htup, uint32 relfileOff,
							 int itemSize);
static void EmitXmlIndexTuple(Page page, BlockNumber blkno,
							  OffsetNumber offset, IndexTuple tuple,
							  uint32 relfileOff, int itemSize, bool dead);
static void EmitXmlSpGistInnerTuple(Page page, BlockNumber blkno,
									OffsetNumber offset,
									SpGistInnerTuple tuple,
									uint32 relfileOff);
static void EmitXmlSpGistLeafTuple(Page page, BlockNumber blkno,
								   OffsetNumber offset,
								   SpGistLeafTuple tuple,
								   uint32 relfileOff);
static void EmitXmlBrinTuple(Page page, BlockNumber blkno,
							 OffsetNumber offset, BrinTuple *tuple,
							 uint32 relfileOff, int itemSize);
static int	EmitXmlPageHeader(Page page, BlockNumber blkno, uint32 level);
static void EmitXmlPageMeta(BlockNumber blkno, uint32 level);
static void EmitXmlPageItemIdArray(Page page, BlockNumber blkno);
static void EmitXmlTuples(Page page, BlockNumber blkno);
static void EmitXmlPostingTreeTids(Page page, BlockNumber blkno);
static void EmitXmlHashBitmap(Page page, BlockNumber blkno);
static void EmitXmlRevmap(Page page, BlockNumber blkno);
static void EmitXmlSpecial(BlockNumber blkno, uint32 level);
static void EmitXmlBody(void);


/*	Send properly formed usage information to the user. */
static void
DisplayOptions(unsigned int validOptions)
{
	if (validOptions == OPT_RC_COPYRIGHT)
		printf
			("pg_hexedit %s (for PostgreSQL %s)"
			 "\nCopyright (c) 2018-2021, Crunchy Data Solutions, Inc."
			 "\nCopyright (c) 2017-2018, VMware, Inc."
			 "\nCopyright (c) 2002-2010, Red Hat, Inc."
			 "\nCopyright (c) 2011-2021, PostgreSQL Global Development Group\n",
			 HEXEDIT_VERSION, PG_VERSION);

	printf
		("\nUsage: pg_hexedit [-hklz] [-D attrlist] [-n segnumber] [-R startblock [endblock]] [-s segsize] [-x lsn] file\n\n"
		 "Output contents of PostgreSQL relation file as wxHexEditor XML tags\n"
		 "  -D  Decode tuples using given comma separated list of attribute metadata\n"
		 "      See README.md for an explanation of the attrlist format\n"
		 "  -h  Display this information\n"
		 "  -k  Verify all block checksums\n"
		 "  -l  Skip leaf pages\n"
		 "  -n  Force segment number to [segnumber]\n"
		 "  -R  Display specific block ranges within the file (Blocks are\n"
		 "      indexed from 0)\n" "        [startblock]: block to start at\n"
		 "        [endblock]: block to end at\n"
		 "      A startblock without an endblock will format the single block\n"
		 "  -s  Force segment size to [segsize]\n"
		 "  -x  Skip pages whose LSN is before [lsn]\n"
		 "  -z  Verify block checksums when non-zero\n"
		 "\nReport bugs to <pg@bowt.ie>\n");
}

/*
 * Determine segment number by segment file name. For instance, if file
 * name is /path/to/xxxx.7 procedure returns 7. Default return value is 0.
 */
static unsigned int
GetSegmentNumberFromFileName(const char *fileName)
{
	int			segnumOffset = strlen(fileName) - 1;

	if (segnumOffset < 0)
		return 0;

	while (isdigit(fileName[segnumOffset]))
	{
		segnumOffset--;
		if (segnumOffset < 0)
			return 0;
	}

	if (fileName[segnumOffset] != '.')
		return 0;

	return atoi(&fileName[segnumOffset + 1]);
}

/*
 * Hash function is taken from sdbm, a public-domain reimplementation of the
 * ndbm database library.
 */
static uint32
sdbmhash(const unsigned char *elem, size_t len)
{
	uint32		hash = 0;
	int			i;

	for (i = 0; i < len; elem++, i++)
	{
		hash = (*elem) + (hash << 6) + (hash << 16) - hash;
	}

	return hash;
}

/*
 * Return a cstring of an html color that is a function of attrName argument.
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
	snprintf(colorStr, 8, "#%02X%02X%02X", red, green, blue);

	return colorStr;
}

/*
 * Iterate through the provided options and set the option flags.  An error
 * will result in a positive rc and will force a display of the usage
 * information.  This routine returns enum option ReturnCode values.
 */
static unsigned int
ConsumeOptions(int numOptions, char **options)
{
	unsigned int rc = OPT_RC_VALID;
	unsigned int x;
	unsigned int optionStringLength;
	char	   *optionString;
	char		duplicateSwitch = 0x00;

	for (x = 1; x < numOptions; x++)
	{
		optionString = options[x];
		optionStringLength = strlen(optionString);

		/*
		 * Range is a special case where we have to consume the next 1 or 2
		 * parameters to mark the range start and end
		 */
		if ((optionStringLength == 2) && (strcmp(optionString, "-R") == 0))
		{
			int			range = 0;

			SET_OPTION(blockOptions, BLOCK_RANGE, 'R');
			/* Only accept the range option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* Make sure there are options after the range identifier */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: missing range start identifier\n");
				exitCode = 1;
				break;
			}

			/*
			 * Mark that we have the range and advance the option to what
			 * should be the range start. Check the value of the next
			 * parameter.
			 */
			optionString = options[++x];
			if ((range = GetOptionValue(optionString)) < 0)
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: invalid range start identifier \"%s\"\n",
						optionString);
				exitCode = 1;
				break;
			}

			/* The default is to dump only one block */
			blockStart = blockEnd = (unsigned int) range;

			/*
			 * We have our range start marker, check if there is an end marker
			 * on the option line.  Assume that the last option is the file we
			 * are dumping, so check if there are options range start marker
			 * and the file.
			 */
			if (x <= (numOptions - 3))
			{
				if ((range = GetOptionValue(options[x + 1])) >= 0)
				{
					/* End range must be => start range */
					if (blockStart <= range)
					{
						blockEnd = (unsigned int) range;
						x++;
					}
					else
					{
						rc = OPT_RC_INVALID;
						fprintf(stderr, "pg_hexedit error: requested block range start %d is greater than end %d\n",
								blockStart, range);
						exitCode = 1;
						break;
					}
				}
			}
		}

		/* Check for the special case where the user requires tuple decoding */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-D") == 0))
		{
			SET_OPTION(blockOptions, BLOCK_DECODE, 'D');
			/* Only accept the decode option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/*
			 * The string immediately following -D is a list of attlen,
			 * attname, and attalign tokens separated by commas.  There is one
			 * set of each per attribute (i.e. there should be three times as
			 * many entries in the list as there are user attributes in the
			 * relation).
			 */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: missing attrlist string\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be attribute metadata string */
			optionString = options[++x];

			if (!ParseAttributeListString(optionString))
			{
				/* Give details of problem in ParseAttributeListString() */
				rc = OPT_RC_INVALID;
				fprintf(stderr, "invalid attrlist string %s\n",
						optionString);
				exitCode = 1;
				break;
			}
		}

		/*
		 * Check for the special case where the user only requires tags for
		 * pages whose LSN equals or exceeds a supplied threshold.
		 */
		else if ((optionStringLength == 2) && (strcmp(optionString, "-x") == 0))
		{
			SET_OPTION(blockOptions, BLOCK_SKIP_LSN, 'x');
			/* Only accept the LSN option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* Make sure that there is an LSN option */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: missing LSN\n");
				exitCode = 1;
				break;
			}

			/*
			 * Mark that we have the LSN and advance the option to what should
			 * be the LSN argument. Check the value of the next parameter.
			 */
			optionString = options[++x];
			if ((afterThreshold = GetOptionXlogRecPtr(optionString)) == InvalidXLogRecPtr)
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: invalid LSN identifier \"%s\"\n",
						optionString);
				exitCode = 1;
				break;
			}
		}
		/* Check for the special case where the user forces a segment size. */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-s") == 0))
		{
			int			localSegmentSize;

			SET_OPTION(segmentOptions, SEGMENT_SIZE_FORCED, 's');
			/* Only accept the forced size option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -s is the segment size */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: missing segment size identifier\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be forced segment size */
			optionString = options[++x];
			if ((localSegmentSize = GetOptionValue(optionString)) > 0)
				segmentSize = (unsigned int) localSegmentSize;
			else
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: invalid segment size requested \"%s\"\n",
						optionString);
				exitCode = 1;
				break;
			}
		}

		/*
		 * Check for the special case where the user forces a segment number
		 * instead of having the tool determine it by file name.
		 */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-n") == 0))
		{
			int			localSegmentNumber;

			SET_OPTION(segmentOptions, SEGMENT_NUMBER_FORCED, 'n');
			/* Only accept the forced segment number option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -n is the segment number */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: missing segment number identifier\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be forced segment number */
			optionString = options[++x];
			if ((localSegmentNumber = GetOptionValue(optionString)) > 0)
				segmentNumber = (unsigned int) localSegmentNumber;
			else
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: invalid segment number requested \"%s\"\n",
						optionString);
				exitCode = 1;
				break;
			}
		}
		/* The last option MUST be the file name */
		else if (x == (numOptions - 1))
		{
			/* Check to see if this looks like an option string before opening */
			if (optionString[0] != '-')
			{
				fp = fopen(optionString, "rb");
				if (fp)
				{
					fileName = options[x];
					if (!(segmentOptions & SEGMENT_NUMBER_FORCED))
						segmentNumber = GetSegmentNumberFromFileName(fileName);
				}
				else
				{
					rc = OPT_RC_FILE;
					fprintf(stderr, "pg_hexedit error: could not open file \"%s\"\n",
							optionString);
					exitCode = 1;
					break;
				}
			}
			else
			{
				/*
				 * Could be the case where the help flag is used without a
				 * filename. Otherwise, the last option isn't a file
				 */
				if (strcmp(optionString, "-h") == 0)
					rc = OPT_RC_COPYRIGHT;
				else
				{
					rc = OPT_RC_FILE;
					fprintf(stderr, "pg_hexedit error: missing file name to dump\n");
					exitCode = 1;
				}
				break;
			}
		}
		else
		{
			unsigned int y;

			/* Option strings must start with '-' and contain switches */
			if (optionString[0] != '-')
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: invalid option string \"%s\"\n",
						optionString);
				exitCode = 1;
				break;
			}

			/*
			 * Iterate through the singular option string, throw out garbage,
			 * duplicates and set flags to be used in formatting
			 */
			for (y = 1; y < optionStringLength; y++)
			{
				switch (optionString[y])
				{
						/* Display the usage screen */
					case 'h':
						rc = OPT_RC_COPYRIGHT;
						break;

						/* Verify all block checksums */
					case 'k':
						SET_OPTION(blockOptions, BLOCK_CHECKSUMS, 'k');
						break;

						/* Verify block checksums when non-zero */
					case 'z':
						SET_OPTION(blockOptions, BLOCK_ZEROSUMS, 'z');
						break;

						/* Skip non-root leaf pages */
					case 'l':
						SET_OPTION(blockOptions, BLOCK_SKIP_LEAF, 'l');
						break;

					default:
						rc = OPT_RC_INVALID;
						fprintf(stderr, "pg_hexedit error: unknown option '%c'\n",
								optionString[y]);
						exitCode = 1;
						break;
				}

				if (rc)
					break;
			}
		}
	}

	if (rc == OPT_RC_DUPLICATE)
	{
		fprintf(stderr, "pg_hexedit error: duplicate option listed '%c'\n",
				duplicateSwitch);
		exitCode = 1;
	}

	return (rc);
}

/*
 * Given the index into the parameter list, convert and return the current
 * string to a number if possible
 */
static int
GetOptionValue(char *optionString)
{
	unsigned int x;
	int			value = -1;
	int			optionStringLength = strlen(optionString);

	/* Verify the next option looks like a number */
	for (x = 0; x < optionStringLength; x++)
	{
		if (!isdigit((int) optionString[x]))
			break;
	}

	/* Convert the string to a number if it looks good */
	if (x == optionStringLength)
		value = atoi(optionString);

	return value;
}

/*
 * Given an alphanumeric LSN, convert and return it to an XLogRecPtr if
 * possible
 */
static XLogRecPtr
GetOptionXlogRecPtr(char *optionString)
{
	uint32		xlogid;
	uint32		xrecoff;
	XLogRecPtr	value = InvalidXLogRecPtr;

	if (sscanf(optionString, "%X/%X", &xlogid, &xrecoff) == 2)
		value = (uint64) xlogid << 32 | xrecoff;

	return value;
}

/*
 * Given an attrlist string (pg_hexedit -D argument string), deserialize into
 * data structures used by tuple decoding to create per-tuple, per-attribute
 * tags.
 *
 * Returns false on failure, allowing caller to set exitCode and print
 * supplemental information common to all parse failures.
 */
static bool
ParseAttributeListString(const char *arg)
{
	char	   *attrlist;
	int			lennamealign;
	char	   *curropt;
	char	   *nextopt;

	/* Allocate space for decoding state */
	attlenrel = pg_malloc(sizeof(int) * MaxTupleAttributeNumber);
	attnamerel = pg_malloc(sizeof(char *) * MaxTupleAttributeNumber);
	attcolorrel = pg_malloc(sizeof(char *) * MaxTupleAttributeNumber);
	attalignrel = pg_malloc(sizeof(char) * MaxTupleAttributeNumber);

	/* Create copy of argument string to scribble on */
	attrlist = pg_strdup(arg);

	nrelatts = 0;
	lennamealign = 0;
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

		if (lennamealign == 0)
		{
			int			attlen;

			if (sscanf(curropt, "%d", &attlen) != 1)
			{
				fprintf(stderr, "pg_hexedit error: could not parse attlen from attrlist argument\n");
				return false;
			}
			attlenrel[nrelatts] = attlen;
			/* Prepare for next item */
			lennamealign++;
		}
		else if (lennamealign == 1)
		{
			/*
			 * Copy attribute name, and dynamically generate color for
			 * attribute
			 */
			attnamerel[nrelatts] = pg_strdup(curropt);
			attcolorrel[nrelatts] = GetColorFromAttrname(curropt);
			/* Prepare for next item */
			lennamealign++;
		}
		else
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
			/* Prepare for next item, and next attribute in "descriptor" */
			if (nrelatts >= MaxTupleAttributeNumber)
			{
				fprintf(stderr, "pg_hexedit error: too many attributes represented in attrlist argument\n");
				return false;
			}
			lennamealign = 0;
			nrelatts++;
		}

		curropt = nextopt;
	}

	/* Be tidy */
	pg_free(attrlist);

	/*
	 * Should be at least one attribute, and should have attlen, attname, and
	 * attalign for each attribute
	 */
	return nrelatts > 0 && lennamealign == 0;
}

/*
 * Read the page header off of block 0 to determine the block size used in this
 * file.  Can be overridden using the -s option.  The returned value is the
 * block size of block 0 on disk.  If a valid page size could not be read,
 * assumes BLCKSZ.
 */
static unsigned int
GetBlockSize(void)
{
	unsigned int pageHeaderSize = sizeof(PageHeaderData);
	unsigned int localSize = BLCKSZ;
	int			bytesRead = 0;
	char		localCache[sizeof(PageHeaderData)];

	/* Read the first header off of block 0 to determine the block size */
	bytesRead = fread(&localCache, 1, pageHeaderSize, fp);
	rewind(fp);

	if (bytesRead == pageHeaderSize)
		localSize = (unsigned int) PageGetPageSize(&localCache);
	else
	{
		fprintf(stderr, "pg_hexedit error: unable to read full page header from first block\n"
				"read %u bytes\n", bytesRead);
		exitCode = 1;
	}

	if (localSize == 0 || ((localSize - 1) & localSize) != 0)
	{
		fprintf(stderr, "pg_hexedit error: invalid block size %u encountered in first block\n",
				localSize);
		exitCode = 1;
		localSize = BLCKSZ;
	}

	return localSize;
}

/*
 * Determine the LSN of page as an XLogRecPtr
 */
static XLogRecPtr
GetPageLsn(Page page)
{
	PageHeader	pageHeader = (PageHeader) page;

	return PageXLogRecPtrGet(pageHeader->pd_lsn);
}

/*
 * Determine the contents of the special section on the block and return this
 * enum value
 */
static unsigned int
GetSpecialSectionType(Page page)
{
	unsigned int rc;
	unsigned int specialOffset;
	unsigned int specialSize;
	unsigned int specialValue;
	PageHeader	pageHeader = (PageHeader) page;

	/*
	 * If this is not a partial header, check the validity of the  special
	 * section offset and contents
	 */
	if (bytesToFormat > sizeof(PageHeaderData))
	{
		specialOffset = (unsigned int) pageHeader->pd_special;

		/*
		 * Check that the special offset can remain on the block or the
		 * partial block
		 */
		if ((specialOffset == 0) ||
			(specialOffset > blockSize) || (specialOffset > bytesToFormat))
			rc = SPEC_SECT_ERROR_BOUNDARY;
		else
		{
			/* we may need to examine last 2 bytes of page to identify index */
			uint16	   *ptype = (uint16 *) (buffer + blockSize - sizeof(uint16));

			specialSize = blockSize - specialOffset;

			/*
			 * If there is a special section, use its size to guess its
			 * contents, checking the last 2 bytes of the page in cases that
			 * are ambiguous.  Note we don't attempt to dereference  the
			 * pointers without checking bytesToFormat == blockSize.
			 */
			if (specialSize == 0)
				rc = SPEC_SECT_NONE;
			else if (specialSize == MAXALIGN(sizeof(uint32)))
			{
				/*
				 * If MAXALIGN is 8, this could be either a sequence or
				 * SP-GiST or GIN.
				 */
				if (bytesToFormat == blockSize)
				{
					specialValue = *((int *) (buffer + specialOffset));
					if (specialValue == SEQUENCE_MAGIC)
						rc = SPEC_SECT_SEQUENCE;
					else if (specialSize == MAXALIGN(sizeof(SpGistPageOpaqueData)) &&
							 *ptype == SPGIST_PAGE_ID)
						rc = SPEC_SECT_INDEX_SPGIST;
					else if (specialSize == MAXALIGN(sizeof(BrinSpecialSpace)) &&
							 IsBrinPage(page))
						rc = SPEC_SECT_INDEX_BRIN;
					else if (specialSize == MAXALIGN(sizeof(GinPageOpaqueData)))
						rc = SPEC_SECT_INDEX_GIN;
					else
						rc = SPEC_SECT_ERROR_UNKNOWN;
				}
				else
					rc = SPEC_SECT_ERROR_UNKNOWN;
			}

			/*
			 * SP-GiST and GIN have same size special section, so check the
			 * page ID bytes first
			 */
			else if (specialSize == MAXALIGN(sizeof(SpGistPageOpaqueData)) &&
					 bytesToFormat == blockSize &&
					 *ptype == SPGIST_PAGE_ID)
				rc = SPEC_SECT_INDEX_SPGIST;
			else if (specialSize == MAXALIGN(sizeof(BrinSpecialSpace)) &&
					 IsBrinPage(page))
				rc = SPEC_SECT_INDEX_BRIN;
			else if (specialSize == MAXALIGN(sizeof(GinPageOpaqueData)))
				rc = SPEC_SECT_INDEX_GIN;
			else if (specialSize > 2 && bytesToFormat == blockSize)
			{
				/*
				 * As of 8.3, BTree, Hash, and GIST all have the same size
				 * special section, but the last two bytes of the section can
				 * be checked to determine what's what.
				 */
				if (*ptype <= MAX_BT_CYCLE_ID &&
					specialSize == MAXALIGN(sizeof(BTPageOpaqueData)))
					rc = SPEC_SECT_INDEX_BTREE;
				else if (*ptype == HASHO_PAGE_ID &&
						 specialSize == MAXALIGN(sizeof(HashPageOpaqueData)))
					rc = SPEC_SECT_INDEX_HASH;
				else if (*ptype == GIST_PAGE_ID &&
						 specialSize == MAXALIGN(sizeof(GISTPageOpaqueData)))
					rc = SPEC_SECT_INDEX_GIST;
				else
					rc = SPEC_SECT_ERROR_UNKNOWN;
			}
			else
				rc = SPEC_SECT_ERROR_UNKNOWN;
		}
	}
	else
		rc = SPEC_SECT_ERROR_UNKNOWN;

	return rc;
}

static const char *
GetSpecialSectionString(unsigned int type)
{
	switch (type)
	{
		case SPEC_SECT_NONE:
			return "SPEC_SECT_NONE";
		case SPEC_SECT_SEQUENCE:
			return "SPEC_SECT_SEQUENCE";
		case SPEC_SECT_INDEX_BTREE:
			return "SPEC_SECT_INDEX_BTREE";
		case SPEC_SECT_INDEX_HASH:
			return "SPEC_SECT_INDEX_HASH";
		case SPEC_SECT_INDEX_GIST:
			return "SPEC_SECT_INDEX_GIST";
		case SPEC_SECT_INDEX_GIN:
			return "SPEC_SECT_INDEX_GIN";
		case SPEC_SECT_INDEX_SPGIST:
			return "SPEC_SECT_INDEX_SPGIST";
		case SPEC_SECT_INDEX_BRIN:
			return "SPEC_SECT_INDEX_BRIN";
		case SPEC_SECT_ERROR_UNKNOWN:
			return "SPEC_SECT_ERROR_UNKNOWN";
		case SPEC_SECT_ERROR_BOUNDARY:
			return "SPEC_SECT_ERROR_BOUNDARY";
		default:
			return "???";
	}
}

/*
 * Given Heap tuple header, return string buffer with t_infomask or t_infomask2
 * flags.
 *
 * Note:  Caller is responsible for pg_free()'ing returned buffer.
 */
static char *
GetHeapTupleHeaderFlags(HeapTupleHeader htup, bool isInfomask2)
{
	unsigned int bitmapLength = 0;
	unsigned int oidLength = 0;
	unsigned int computedLength;
	unsigned int localHoff;
	unsigned int localBitOffset;
	char	   *flagString = NULL;

	flagString = pg_malloc(512);
	localHoff = htup->t_hoff;
	localBitOffset = offsetof(HeapTupleHeaderData, t_bits);

	/*
	 * Place readable versions of the tuple info mask into a buffer. Assume
	 * that the string can not expand beyond 512 bytes.
	 */
	flagString[0] = '\0';
	if (!isInfomask2)
	{
		strcat(flagString, "t_infomask (");

		if (htup->t_infomask & HEAP_HASNULL)
			strcat(flagString, "HEAP_HASNULL|");
		if (htup->t_infomask & HEAP_HASVARWIDTH)
			strcat(flagString, "HEAP_HASVARWIDTH|");
		if (htup->t_infomask & HEAP_HASEXTERNAL)
			strcat(flagString, "HEAP_HASEXTERNAL|");
		if (htup->t_infomask & HEAP_HASOID)
			strcat(flagString, "HEAP_HASOID|");
		if (htup->t_infomask & HEAP_XMAX_KEYSHR_LOCK)
			strcat(flagString, "HEAP_XMAX_KEYSHR_LOCK|");
		if (htup->t_infomask & HEAP_COMBOCID)
			strcat(flagString, "HEAP_COMBOCID|");
		if (htup->t_infomask & HEAP_XMAX_EXCL_LOCK)
			strcat(flagString, "HEAP_XMAX_EXCL_LOCK|");
		if (htup->t_infomask & HEAP_XMAX_LOCK_ONLY)
			strcat(flagString, "HEAP_XMAX_LOCK_ONLY|");
		if (htup->t_infomask & HEAP_XMIN_COMMITTED)
			strcat(flagString, "HEAP_XMIN_COMMITTED|");
		if (htup->t_infomask & HEAP_XMIN_INVALID)
			strcat(flagString, "HEAP_XMIN_INVALID|");
		if (htup->t_infomask & HEAP_XMAX_COMMITTED)
			strcat(flagString, "HEAP_XMAX_COMMITTED|");
		if (htup->t_infomask & HEAP_XMAX_INVALID)
			strcat(flagString, "HEAP_XMAX_INVALID|");
		if (htup->t_infomask & HEAP_XMAX_IS_MULTI)
			strcat(flagString, "HEAP_XMAX_IS_MULTI|");
		if (htup->t_infomask & HEAP_UPDATED)
			strcat(flagString, "HEAP_UPDATED|");
		if (htup->t_infomask & HEAP_MOVED_OFF)
			strcat(flagString, "HEAP_MOVED_OFF|");
		if (htup->t_infomask & HEAP_MOVED_IN)
			strcat(flagString, "HEAP_MOVED_IN|");

		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';
		strcat(flagString, ")");
	}
	else
	{
		sprintf(flagString, "t_infomask2 HeapTupleHeaderGetNatts(): %d ",
				HeapTupleHeaderGetNatts(htup));

		if (htup->t_infomask2 & ~HEAP_NATTS_MASK)
			strcat(flagString, "(");


		if (htup->t_infomask2 & HEAP_KEYS_UPDATED)
			strcat(flagString, "HEAP_KEYS_UPDATED|");
		if (htup->t_infomask2 & HEAP_HOT_UPDATED)
			strcat(flagString, "HEAP_HOT_UPDATED|");
		if (htup->t_infomask2 & HEAP_ONLY_TUPLE)
			strcat(flagString, "HEAP_ONLY_TUPLE|");

		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';
		if (htup->t_infomask2 & ~HEAP_NATTS_MASK)
			strcat(flagString, ")");
	}

	/*
	 * As t_bits is a variable length array, and may contain an Oid field,
	 * determine the length of the header proper as a sanity check.
	 */
	if (htup->t_infomask & HEAP_HASNULL)
		bitmapLength = BITMAPLEN(HeapTupleHeaderGetNatts(htup));
	else
		bitmapLength = 0;

	if (htup->t_infomask & HEAP_HASOID)
		oidLength += sizeof(Oid);

	computedLength =
		MAXALIGN(localBitOffset + bitmapLength + oidLength);

	/*
	 * Inform the user of a header size mismatch or dump the t_bits array
	 */
	if (computedLength != localHoff)
	{
		fprintf
			(stderr,
			 "pg_hexedit error: computed header length not equal to header size.\n"
			 "computed: %u header: %d\n", computedLength, localHoff);

		exitCode = 1;
	}

	return flagString;
}

/*
 * Given IndexTuple, return string buffer with t_info reported tuple length,
 * and flags.
 *
 * Note:  Caller is responsible for pg_free()'ing returned buffer.
 */
static char *
GetIndexTupleFlags(IndexTuple itup)
{
	char	   *flagString = NULL;

	flagString = pg_malloc(512);

	/*
	 * Place readable versions of the tuple info mask into a buffer.  Assume
	 * that the string can not expand beyond 128 bytes.
	 */
	flagString[0] = '\0';
	sprintf(flagString, "t_info IndexTupleSize(): %zu",
			IndexTupleSize(itup));

	if (itup->t_info & ~INDEX_SIZE_MASK)
		strcat(flagString, ", (");

	/*
	 * Bit 0x2000/INDEX_AM_RESERVED_BIT is reserved for AM-specific usage.
	 * The INDEX_AM_RESERVED_BIT flag was only added in Postgres v11, so
	 * 0x2000 is used to support earlier versions that lack the flag but still
	 * use the status bit.
	 *
	 * Theoretically, we should only find this status bit set within a hash or
	 * nbtree IndexTuple (and only on versions 10+ and 11+ respectively).
	 * However, it's easy to maintain forwards compatibility when pg_hexedit
	 * is built against earlier Postgres versions, so do so.
	 */
	if (itup->t_info & 0x2000)
	{
		if (specialType == SPEC_SECT_INDEX_HASH)
			strcat(flagString, "INDEX_MOVED_BY_SPLIT_MASK|");
		else if (specialType == SPEC_SECT_INDEX_BTREE)
			strcat(flagString, "INDEX_ALT_TID_MASK|");
		else
			strcat(flagString, "INDEX_AM_RESERVED_BIT|");
	}
	if (itup->t_info & INDEX_VAR_MASK)
		strcat(flagString, "INDEX_VAR_MASK|");
	if (itup->t_info & INDEX_NULL_MASK)
		strcat(flagString, "INDEX_NULL_MASK|");

	if (itup->t_info & ~INDEX_SIZE_MASK)
	{
		flagString[strlen(flagString) - 1] = '\0';
		strcat(flagString, ")");
	}

	return flagString;
}

static const char *
GetSpGistStateString(unsigned int code)
{
	switch (code)
	{
		case SPGIST_LIVE:
			return "SPGIST_LIVE";
		case SPGIST_REDIRECT:
			return "SPGIST_REDIRECT";
		case SPGIST_DEAD:
			return "SPGIST_DEAD";
		case SPGIST_PLACEHOLDER:
			return "SPGIST_PLACEHOLDER";
		default:
			return "???";
	}
}

/*
 * Given SpGistInnerTuple, return string buffer with tuple-reported state from
 * bitfields.
 *
 * Note:  Caller is responsible for pg_free()'ing returned buffer.
 */
static char *
GetSpGistInnerTupleState(SpGistInnerTuple itup)
{
	char	   *flagString = NULL;

	flagString = pg_malloc(128);

	sprintf(flagString, "tupstate: %s, allTheSame: %u, nNodes: %u, prefixSize: %u",
			GetSpGistStateString(itup->tupstate), itup->allTheSame, itup->nNodes, itup->prefixSize);

	return flagString;
}

/*
 * Given SpGistLeafTuple, return string buffer with tuple-reported state from
 * bitfields.
 *
 * Note:  Caller is responsible for pg_free()'ing returned buffer.
 */
static char *
GetSpGistLeafTupleState(SpGistLeafTuple itup)
{
	char	   *flagString = NULL;

	flagString = pg_malloc(128);

	sprintf(flagString, "tupstate: %s, size: %u",
			GetSpGistStateString(itup->tupstate), itup->size);

	return flagString;
}

/*
 * Given BrinTuple, return string buffer with bt_info reported data offset, and
 * tuple flags.
 *
 * Note:  Caller is responsible for pg_free()'ing returned buffer.
 */
static char *
GetBrinTupleFlags(BrinTuple *itup)
{
	char	   *flagString = NULL;

	flagString = pg_malloc(128);

	/*
	 * Place readable versions of the tuple info mask into a buffer.  Assume
	 * that the string can not expand beyond 128 bytes.
	 */
	flagString[0] = '\0';
	sprintf(flagString, "bt_info BrinTupleDataOffset(): %zu",
			BrinTupleDataOffset(itup));

	if (itup->bt_info & (BRIN_PLACEHOLDER_MASK | BRIN_NULLS_MASK))
		strcat(flagString, ", (");

	if (itup->bt_info & BRIN_PLACEHOLDER_MASK)
		strcat(flagString, "BRIN_PLACEHOLDER_MASK|");
	if (itup->bt_info & BRIN_NULLS_MASK)
		strcat(flagString, "BRIN_NULLS_MASK|");

	if (itup->bt_info & (BRIN_PLACEHOLDER_MASK | BRIN_NULLS_MASK))
	{
		flagString[strlen(flagString) - 1] = '\0';
		strcat(flagString, ")");
	}

	return flagString;
}

/*	Check whether page is a BRIN meta page */
static bool
IsBrinPage(Page page)
{
	if (bytesToFormat != blockSize)
		return false;

	if (BRIN_IS_META_PAGE(page) || BRIN_IS_REVMAP_PAGE(page) ||
		BRIN_IS_REGULAR_PAGE(page))
		return true;
	return false;
}

/*	Check whether page is a hash bitmap page */
static bool
IsHashBitmapPage(Page page)
{
	HashPageOpaque opaque;

	/* Defensive */
	if (bytesToFormat != blockSize)
		return false;
	if (specialType != SPEC_SECT_INDEX_HASH)
		return false;

	opaque = (HashPageOpaque) PageGetSpecialPointer(page);
	if (opaque->hasho_flag & LH_BITMAP_PAGE)
		return true;

	return false;
}

/* Check whether page is a leaf page */
static bool
IsLeafPage(Page page)
{
	if (specialType == SPEC_SECT_INDEX_BTREE)
	{
		BTPageOpaque btreeSection = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * Don't count a root page as a leaf (i.e.  the root before the first
		 * root page split).
		 */
		if ((btreeSection->btpo_flags & BTP_LEAF) &&
			!(btreeSection->btpo_flags & BTP_ROOT))
			return true;
	}
	else if (specialType == SPEC_SECT_INDEX_GIST)
	{
		if (GistPageIsLeaf(page))
			return true;
	}
	else if (specialType == SPEC_SECT_INDEX_GIN)
	{
		/* This works for both posting trees, and the main entry tree */
		if (GinPageIsLeaf(page))
			return true;
	}
	else if (specialType == SPEC_SECT_INDEX_SPGIST)
	{
		if (SpGistPageIsLeaf(page))
			return true;
	}

	return false;
}

/*
 * For each block, dump out formatted header and content information
 */
static void
EmitXmlPage(BlockNumber blkno)
{
	Page		page = (Page) buffer;
	uint32		level = UINT_MAX;
	int			rc;

	if (PageIsNew(page))
		return;

	/* Get details of page first */
	pageOffset = blockSize * currentBlock;
	specialType = GetSpecialSectionType(page);

	/*
	 * A Postgres segment file should consists of blocks that are all of the
	 * same special section reported type (excluding those blocks that have
	 * yet to be initialized by PageInit()).  Raise error when this
	 * expectation is not met.
	 */
	if (firstType == SPEC_SECT_ERROR_UNKNOWN)
		firstType = specialType;

	if (firstType != specialType)
	{
		fprintf(stderr, "pg_hexedit error: special section indicated type unexpectedly changed from \"%s\" to \"%s\" at file block %u\n",
				GetSpecialSectionString(firstType),
				GetSpecialSectionString(specialType), blkno);
		exitCode = 1;
	}

	/*
	 * Check to see if we must skip this block due to it falling behind LSN
	 * threshold
	 */
	if ((blockOptions & BLOCK_SKIP_LSN))
	{
		XLogRecPtr	pageLSN = GetPageLsn(page);

		if (pageLSN < afterThreshold)
		{
			rc = 0;
			return;
		}
	}

	/* Get "level" for page.  Only B-Tree tags get a "level" */
	if (specialType == SPEC_SECT_INDEX_BTREE)
	{
		BTPageOpaque btreeSection = (BTPageOpaque) PageGetSpecialPointer(page);

		level = btreeSection->btpo.level;
	}

	/*
	 * We optionally itemize leaf blocks as whole tags, in order to limit the
	 * size of tag files sharply.  Internal pages can be more interesting when
	 * debugging certain types of problems, such as problems with the balance
	 * of some tree structure.
	 */
	if ((blockOptions & BLOCK_SKIP_LEAF) && IsLeafPage(page))
	{
		EmitXmlTag(blkno, level, "leaf page", COLOR_GREEN_DARK,
				   pageOffset,
				   (pageOffset + BLCKSZ) - 1);
		rc = 0;
		return;
	}

	/*
	 * Every block that we aren't skipping will have header, items and
	 * possibly special section tags created.  Beware of partial block reads,
	 * though.
	 */
	rc = EmitXmlPageHeader(page, blkno, level);

	/* If we didn't encounter a partial read in header, carry on...  */
	if (rc != EOF_ENCOUNTERED)
	{
		/*
		 * All AMs have a single metapage at block zero of the first segment,
		 * with the exception of heapam and GiST. (Sequences more or less
		 * reuse the heap format, and so don't have a metapage.)
		 */
		if (blkno == 0 && segmentNumber == 0 &&
			specialType != SPEC_SECT_NONE &&
			specialType != SPEC_SECT_INDEX_GIST &&
			specialType != SPEC_SECT_SEQUENCE)
		{
			/* If it's a meta page, the meta block will have no tuples */
			EmitXmlPageMeta(blkno, level);
		}
		else if (specialType == SPEC_SECT_INDEX_HASH && IsHashBitmapPage(page))
		{
			/* Hash bitmap pages don't use IndexTuple or ItemId */
			EmitXmlHashBitmap(page, blkno);
		}
		else if (specialType == SPEC_SECT_INDEX_GIST && GistPageIsDeleted(page))
		{
			/*
			 * Deleted GiST pages only contain GISTDeletedPageContents on
			 * Postgres 12+ -- don't bother distinguishing deleted pages.
			 * Cannot trust maxoff from page.
			 */
		}
		else if (specialType == SPEC_SECT_INDEX_GIN && GinPageIsDeleted(page))
		{
			/*
			 * Unfortunately, GIN_DELETED pages don't have page state needed
			 * by GinPageIsData().  Don't attempt to emit tags for tuples on
			 * deleted GIN pages.
			 */
		}
		else if (specialType == SPEC_SECT_INDEX_GIN && GinPageIsData(page))
		{
			/* GIN data/posting tree pages don't use IndexTuple or ItemId */
			EmitXmlPostingTreeTids(page, blkno);
		}
		else if (specialType == SPEC_SECT_INDEX_BRIN && BRIN_IS_REVMAP_PAGE(page))
		{
			/* BRIN revmap pages don't use IndexTuple/BrinTuple or ItemId */
			EmitXmlRevmap(page, blkno);
		}
		else
		{
			/* Conventional heap/index page format */
			EmitXmlPageItemIdArray(page, blkno);
			EmitXmlTuples(page, blkno);
		}

		/* Only heapam doesn't have a special area (even sequences have one) */
		if (specialType != SPEC_SECT_NONE)
			EmitXmlSpecial(blkno, level);
	}
}

/*
 * Display a header for the dump so we know the file name, the options and the
 * time the dump was taken
 */
static void
EmitXmlDocHeader(int numOptions, char **options)
{
	unsigned int x;
	char		optionBuffer[52] = "\0";
	char		timeStr[1000];

	/* Format time without newline */
	time_t		rightNow = time(NULL);
	struct tm  *localNow = localtime(&rightNow);

	strftime(timeStr, sizeof(timeStr), "%H:%M:%S %A, %B %d %Y", localNow);

	/*
	 * Iterate through the options and cache them. The maximum we can display
	 * is 50 option characters + spaces.
	 */
	for (x = 1; x < (numOptions - 1); x++)
	{
		if ((strlen(optionBuffer) + strlen(options[x])) > 50)
			break;
		strcat(optionBuffer, options[x]);
		strcat(optionBuffer, " ");
	}

	printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	printf("<!-- Dump created on: %s -->\n", timeStr);
	printf("<!-- Options used: %s -->\n", (strlen(optionBuffer)) ? optionBuffer : "None");
	printf("<!-- Block size: %u -->\n", blockSize);
	printf("<!-- pg_hexedit version: %s -->\n", HEXEDIT_VERSION);
	printf("<!-- pg_hexedit build PostgreSQL version: %s -->\n", PG_VERSION);
	printf("<wxHexEditor_XML_TAG>\n");
	printf("  <filename path=\"%s\">\n", fileName);
}

static void
EmitXmlFooter(void)
{
	printf("  </filename>\n");
	printf("</wxHexEditor_XML_TAG>\n");
}

/*
 * Emit a generic wxHexEditor tag for tuple data.
 *
 * Note: endOffset is an offset to the last byte whose range the tag covers, so
 * callers generally pass (relfileOff + length) - 1.  This is slightly less
 * verbose than getting callers to pass length.
 *
 * B-Tree index callers may optionally pass a "level".  Passing
 * InvalidBlockNumber avoids emitting any block number.
 */
static void
EmitXmlTag(BlockNumber blkno, uint32 level, const char *name, const char *color,
		   uint32 relfileOff, uint32 relfileOffEnd)
{
	Assert(relfileOff <= relfileOffEnd);

	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	if (blkno == InvalidBlockNumber)
		printf("      <tag_text>%s</tag_text>\n", name);
	else if (level != UINT_MAX)
		printf("      <tag_text>block %u (level %u) %s</tag_text>\n",
			   blkno + segmentBlockDelta, level, name);
	else
		printf("      <tag_text>block %u %s</tag_text>\n",
			   blkno + segmentBlockDelta, name);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");
}

/*
 * Emit a wxHexEditor tag for a line pointer (ItemId).
 */
static void
EmitXmlItemId(BlockNumber blkno, OffsetNumber offset, ItemId itemId,
			  uint32 relfileOff, const char *textFlags)
{
	char	   *fontColor;
	char	   *itemIdColor;

	fontColor = COLOR_FONT_STANDARD;
	itemIdColor = COLOR_BLUE_LIGHT;

	/*
	 * The color of the tag and tag font is chosen to give a cue about line
	 * pointer details.  Unused line pointers (which are reusable) have a
	 * non-contrasting font color to deemphasize their importance.  LP_DEAD
	 * line pointers (which are dead but not necessarily reusable yet) don't
	 * stay around for long in most real world workloads, and so it seems
	 * useful to make them stick out.
	 */
	if (ItemIdIsRedirected(itemId))
		itemIdColor = COLOR_BLUE_DARK;
	else if (ItemIdIsDead(itemId))
		itemIdColor = COLOR_BROWN;
	else if (!ItemIdIsUsed(itemId))
		fontColor = COLOR_BLUE_DARK;

	/* Interpret the content of each ItemId separately */
	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%lu</end_offset>\n", (relfileOff + sizeof(ItemIdData)) - 1);
	printf("      <tag_text>(%u,%d) lp_len: %u, lp_off: %u, lp_flags: %s</tag_text>\n",
		   blkno + segmentBlockDelta, offset, ItemIdGetLength(itemId),
		   ItemIdGetOffset(itemId), textFlags);
	printf("      <font_colour>%s</font_colour>\n", fontColor);
	printf("      <note_colour>%s</note_colour>\n", itemIdColor);
	printf("    </TAG>\n");
}

/*
 * Emit a wxHexEditor tag for individual tuple, whose TID is to be represented
 * in the tag annotation.  Could be an IndexTuple, heap tuple, or special
 * tuple-like structure (e.g., GIN data/posting tree page item).
 *
 * Note: relfileOffEnd is an offset to the last byte whose range the tag
 * covers, so callers generally pass (relfileOff + length) - 1.  This is
 * slightly less verbose than getting callers to pass length.
 */
static inline void
EmitXmlTupleTag(BlockNumber blkno, OffsetNumber offset, const char *name,
				const char *color, uint32 relfileOff, uint32 relfileOffEnd)
{
	EmitXmlTupleTagFont(blkno, offset, name, color, COLOR_FONT_STANDARD,
						relfileOff, relfileOffEnd);
}

/*
 * Like EmitXmlTupleTag(), but lets caller specify font color
 */
static void
EmitXmlTupleTagFont(BlockNumber blkno, OffsetNumber offset, const char *name,
					const char *color, const char *fontColor,
					uint32 relfileOff, uint32 relfileOffEnd)
{
	if (relfileOff > relfileOffEnd)
	{
		fprintf(stderr, "pg_hexedit error: (%u,%u) tuple tag \"%s\" is malformed (%u > %u)\n",
				blkno + segmentBlockDelta, offset, name, relfileOff,
				relfileOffEnd);
		exitCode = 1;
		return;
	}

	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	printf("      <tag_text>(%u,%u) %s</tag_text>\n",
		   blkno + segmentBlockDelta, offset, name);
	printf("      <font_colour>%s</font_colour>\n", fontColor);
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");
}

/*
 * Like EmitXmlTupleTagFont(), but lets caller specify name in two parts
 */
static void
EmitXmlTupleTagFontTwoName(BlockNumber blkno, OffsetNumber offset,
						   const char *name1, const char *name2,
						   const char *color, const char *fontColor,
						   uint32 relfileOff, uint32 relfileOffEnd)
{
	char	   *combinednames;

	if (relfileOff > relfileOffEnd)
	{
		fprintf(stderr, "pg_hexedit error: (%u,%u) tuple tag \"%s - %s\" is malformed (%u > %u)\n",
				blkno + segmentBlockDelta, offset, name1, name2, relfileOff,
				relfileOffEnd);
		exitCode = 1;
		return;
	}

	combinednames = pg_malloc(strlen(name1) + strlen(name2) + 5);
	combinednames[0] = '\0';
	strcat(combinednames, name1);
	strcat(combinednames, " - ");
	strcat(combinednames, name2);

	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	printf("      <tag_text>(%u,%u) %s</tag_text>\n",
		   blkno + segmentBlockDelta, offset, combinednames);
	printf("      <font_colour>%s</font_colour>\n", fontColor);
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");

	pg_free(combinednames);
}

/*
 * Emit wxHexEditor tags for individual non-NULL attributes in heap tuple
 */
static void
EmitXmlAttributesHeap(BlockNumber blkno, OffsetNumber offset,
					  uint32 relfileOff, HeapTupleHeader htup, int itemSize)
{
	unsigned char *tupdata = (unsigned char *) htup + htup->t_hoff;
	bits8	   *t_bits;
	int			nattrs = HeapTupleHeaderGetNatts(htup);
	int			datalen = itemSize - htup->t_hoff;

	/*
	 * If an argument describing the relation's tuples was not provided, just
	 * create a single tag
	 */
	if (nrelatts == 0)
	{
		EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE, relfileOff,
						(relfileOff + datalen) - 1);
		return;
	}

	if (nattrs > nrelatts)
	{
		fprintf(stderr, "pg_hexedit error: %d attributes found in (%u,%u) exceeds the number inferred for relation from -D argument %d\n",
				nattrs, blkno, offset, nrelatts);
		exitCode = 1;
		nattrs = nrelatts;
	}

	t_bits = (htup->t_infomask & HEAP_HASNULL) != 0 ? htup->t_bits : NULL;

	EmitXmlAttributesData(blkno, offset, relfileOff, tupdata, t_bits, nattrs,
						  datalen);
}

/*
 * Emit wxHexEditor tags for individual non-NULL attributes in index tuple
 */
static void
EmitXmlAttributesIndex(BlockNumber blkno, OffsetNumber offset,
					   uint32 relfileOff, IndexTuple itup, uint32 tupHeaderOff,
					   int itemSize)
{
	unsigned char *tupdata;
	bits8	   *t_bits;
	int			datalen;
	int			nattrs = nrelatts;
	bool		haveargreltuple = true;

	/*
	 * If an argument describing the relation's tuples was not provided, just
	 * create a single tag, but show pivot tuple heap TID representation, or
	 * posting list representation, since we don't need -D metadata for that.
	 *
	 * Play it safe here -- don't attempt to emit attribute values when an
	 * error was already encountered.  We may be able to limp on for much
	 * longer this way, especially in the event of an access method that we
	 * know nothing about but follows the familiar bufpage.c conventions, such
	 * as zheap.
	 *
	 * Multi-column GIN entries have two attributes in main entry key tuples:
	 * an implicit int16 leading attribute that indicates which pg_attribute
	 * listing the entry relates to, as well as the actual value.  This isn't
	 * something that we currently support, though, so just treat this case as
	 * if we had no catalog metadata.
	 */
	if (nrelatts == 0 || exitCode != 0 ||
		(specialType == SPEC_SECT_INDEX_GIN && nrelatts > 1))
	{
		/* Won't be able to emit tags based on tuple metadata argument */
		haveargreltuple = false;
	}

	/* Set up state for emitting attributes */
	tupdata = (unsigned char *) itup + IndexInfoFindDataOffset(itup->t_info);
	t_bits = IndexTupleHasNulls(itup) ?
		(bits8 *) ((unsigned char *) itup + sizeof(IndexTupleData)) : NULL;
	datalen = itemSize - IndexInfoFindDataOffset(itup->t_info) - 1;

	/*
	 * On Postgres v11+, account for nbtree pivot tuples with truncated
	 * attributes.  INCLUDE attributes won't be present, and so must not have
	 * tags emitted.
	 *
	 * This is based on BTreeTupleGetNAtts(), which cannot be called from
	 * frontend code.
	 *
	 * On Postgres v12+, account for possible heap TID tiebreaker attribute in
	 * pivot tuples.  These TIDs are white because they are considered keys,
	 * not pointers.  This color scheme is based on the precedent set by GIN's
	 * internal posting tree pages.
	 */
#if PG_VERSION_NUM >= 110000

	if (specialType == SPEC_SECT_INDEX_BTREE &&
#if PG_VERSION_NUM < 130000
		(itup->t_info & INDEX_ALT_TID_MASK) != 0)
#else
		 BTreeTupleIsPivot(itup)) /* This inline function is only in Postgres 13 */
#endif /* PG_VERSION_NUM < 130000 */
	{
		nattrs =
			(ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) &
			 BT_OFFSET_MASK);

		if (haveargreltuple && nattrs > nrelatts)
		{
			fprintf(stderr, "pg_hexedit error: %d attributes found in (%u,%u) exceeds the number inferred for relation from -D argument %d\n",
					nattrs, blkno, offset, nrelatts);
			exitCode = 1;
			nattrs = nrelatts;
		}
#if PG_VERSION_NUM >= 120000
		if (BTreeTupleGetHeapTID(itup) != NULL)
		{
			/*
			 * Heap TID attribute is considered a column internally, but has
			 * no pg_attribute entry.
			 *
			 * Note that this only handles the special representation of heap
			 * TID that's used in pivot tuples (includes leaf page high key).
			 * Non-pivot tuples represent heap TID using IndexTupleData.t_tid,
			 * or by using a posting list.
			 */
			uint32		htidoffset;

			htidoffset = (tupHeaderOff + IndexTupleSize(itup)) -
				sizeof(ItemPointerData);
			datalen -= sizeof(ItemPointerData);

			EmitXmlTupleTag(blkno, offset, "BTreeTupleGetHeapTID()->bi_hi", COLOR_PINK,
							htidoffset, (htidoffset + sizeof(uint16)) - 1);
			htidoffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offset, "BTreeTupleGetHeapTID()->bi_lo", COLOR_PINK,
							htidoffset, (htidoffset + sizeof(uint16)) - 1);
			htidoffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offset, "BTreeTupleGetHeapTID()->offsetNumber", COLOR_PINK,
							htidoffset, (htidoffset + sizeof(uint16)) - 1);
		}
#endif /* PG_VERSION_NUM >= 120000 */
	}

#endif /* PG_VERSION_NUM >= 110000 */

	/*
	 * On Postgres v13+, account for nbtree posting list tuples
	 */
#if PG_VERSION_NUM >= 130000
	else if (specialType == SPEC_SECT_INDEX_BTREE && BTreeTupleIsPosting(itup))
	{
		uint32	postoffset = tupHeaderOff + BTreeTupleGetPostingOffset(itup);
		int		i;

		datalen = postoffset - relfileOff - 1;

		/*
		 * Annotate each posting list TID individually, while alternating the
		 * color to increase legibility.
		 *
		 * This deliberately doesn't look anything like pivot tuple heap TID
		 * annotations, and deliberately doesn't recreate the slight variation
		 * in color within TIDs from tuple headers.  Small posting list tuples
		 * should appear prominently among duplicates that aren't posting
		 * lists.
		 */
		for (i = 0; i < BTreeTupleGetNPosting(itup); i++)
		{
			char   *color = (i % 2 == 0 ? COLOR_RED_LIGHT : COLOR_GREEN_LIGHT);
			char	tidstr[30];

			sprintf(tidstr, "TID[%d] bi_hi", i);
			EmitXmlTupleTag(blkno, offset, tidstr, color, postoffset,
							(postoffset + sizeof(uint16)) - 1);
			postoffset += sizeof(uint16);
			sprintf(tidstr, "TID[%d] bi_lo", i);
			EmitXmlTupleTag(blkno, offset, tidstr, color, postoffset,
							(postoffset + sizeof(uint16)) - 1);
			postoffset += sizeof(uint16);
			sprintf(tidstr, "TID[%d] offsetNumber", i);
			EmitXmlTupleTag(blkno, offset, tidstr, color, postoffset,
							(postoffset + sizeof(uint16)) - 1);
			postoffset += sizeof(uint16);
		}
	}
#endif /* PG_VERSION_NUM >= 130000 */

	/*
	 * Finally, emit pg_attribute-wise columns, or plain gray tag that
	 * represents where the pg_attribute-wise tuple values would go if we had
	 * valid -D info
	 */
	if (haveargreltuple)
		EmitXmlAttributesData(blkno, offset, relfileOff, tupdata, t_bits,
							  nattrs, MAXALIGN(datalen));
	else
		EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE,
						relfileOff, relfileOff + datalen);

}

/*
 * Emit wxHexEditor tags for individual non-NULL attributes.
 *
 * This relies on catalog metadata passed by user, since frontend code cannot
 * use tuple descriptors or access system catalog metadata itself.
 *
 * This code is loosely based on nocachegetattr(), though it works with both
 * heap and index tuple data areas.
 */
static void
EmitXmlAttributesData(BlockNumber blkno, OffsetNumber offset,
					  uint32 relfileOff, unsigned char *tupdata, bits8 *t_bits,
					  int nattrs, int datalen)
{
	unsigned char *attptr = tupdata;
	int			off = 0;
	int			i;

	for (i = 0; i < nattrs; i++)
	{
		int			attlen = attlenrel[i];
		char	   *attname = attnamerel[i];
		char	   *attcolor = attcolorrel[i];
		char		attalign = attalignrel[i];
		int			truestartoff = 0;
		int			truelen;

		if (t_bits && att_isnull(i, t_bits))
			continue;

		if (attlen == -1)
		{
			char	   *hdrname = "";

			/* Varlena header receives its own minimal tag */
			off = att_align_pointer(off, attalign, -1, attptr);
			truelen = VARSIZE_ANY(tupdata + off);

			if (VARATT_IS_1B(attptr))
			{
				truestartoff = 1;
				truelen -= 1;
				if (VARATT_IS_1B_E(attptr))
					hdrname = "varattrib_1b_e";
				else
					hdrname = "varattrib_1b";

				EmitXmlTupleTagFontTwoName(blkno, offset, attname, hdrname,
										   attcolor, COLOR_BROWN,
										   relfileOff + off,
										   relfileOff + off);
			}
			else if (VARATT_IS_4B(attptr))
			{
				truestartoff = 4;
				truelen -= 4;
				if (VARATT_IS_4B_U(attptr))
					hdrname = "va_4byte";
				else if (VARATT_IS_4B_C(attptr))
					hdrname = "va_compressed";

				EmitXmlTupleTagFontTwoName(blkno, offset, attname, hdrname,
										   attcolor, COLOR_BROWN,
										   relfileOff + off,
										   relfileOff + off + 3);
			}
		}
		else if (attlen == -2)
		{
			off = att_align_nominal(off, attalign);
			truelen = strnlen((char *) attptr, datalen - off) + 1;
		}
		else
		{
			off = att_align_nominal(off, attalign);
			truelen = attlen;
		}

		if (datalen < off + attlen)
		{
			fprintf(stderr, "pg_hexedit error: unexpected out of bounds tuple data for attnum %d in (%u,%u)\n",
					i + 1, blkno, offset);
			exitCode = 1;
			return;
		}

		EmitXmlTupleTag(blkno, offset, attname, attcolor,
						relfileOff + off + truestartoff,
						relfileOff + off + truestartoff + truelen - 1);

		/* Get possible address of next attribute, handling alignment */
		off = att_addlength_pointer(off, attlen, tupdata + off);
		attptr = tupdata + off;
	}
}

/*
 * Emit a wxHexEditor tag for entire heap tuple.
 *
 * Note: Caller passes itemSize from ItemId because that's the only place that
 * it's available from.
 */
static void
EmitXmlHeapTuple(BlockNumber blkno, OffsetNumber offset,
				 HeapTupleHeader htup, uint32 relfileOff,
				 int itemSize)
{
	TransactionId rawXmin = HeapTupleHeaderGetRawXmin(htup);
	TransactionId rawXmax = HeapTupleHeaderGetRawXmax(htup);
	char		xmin[90];
	char		xmax[90];
	char	   *xminFontColor;
	char	   *xmaxFontColor;
	BlockNumber logBlock = blkno + segmentBlockDelta;
	char	   *blkFontColor;
	char	   *offsetFontColor;
	char	   *flagString;
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;

	/*
	 * Produce xmin and xmax tags for tuple.
	 *
	 * The choice of colors here is not completely arbitrary.  There is some
	 * attempt at analogy in the choice of colors.  For example, xmin and xmax
	 * are symmetric, and so are both COLOR_RED_LIGHT.
	 *
	 * Tuple freezing and MultiXacts leave us with a lot of potentially
	 * interesting information to represent here.  We use special font colors
	 * to represent special transaction IDs, and include certain special
	 * status information about the TransactionId/MultiXact contained within
	 * each field.  The extra information in the tags is redundant with the
	 * infomask tags (tags for where the information is actually represented),
	 * but it seems useful to add some visual cues.
	 */
	strcpy(xmin, "xmin");
	strcpy(xmax, "xmax");
	xminFontColor = COLOR_FONT_STANDARD;
	xmaxFontColor = COLOR_FONT_STANDARD;

	/*
	 * HeapTupleHeaderXminFrozen() doesn't actually consider pre-9.4
	 * pg_upgrad'ed tuples that have FrozenTransactionId as their raw xmin
	 */
	if (!HeapTupleHeaderXminFrozen(htup) && rawXmin != FrozenTransactionId)
	{
		if (rawXmin == BootstrapTransactionId)
		{
			strcat(xmin, " - BootstrapTransactionId");
			xminFontColor = COLOR_WHITE;
		}
		else if (rawXmin == InvalidTransactionId)
		{
			strcat(xmin, " - InvalidTransactionId");
			xminFontColor = COLOR_YELLOW_LIGHT;
		}
	}
	else
	{
		/*
		 * Raw xmin is frozen.  Frozen xmin XIDs are only preserved for
		 * forensic reasons (on Postgres 9.4+), so use low-contrast font to
		 * deemphasize the XID.  (Raw xmin could be FrozenTransactionId here
		 * in a pg_upgrade'd database, which is just as uninteresting.)
		 */
		strcat(xmin, " - Frozen");
		xminFontColor = COLOR_RED_DARK;
	}

	/*
	 * Deliberately tag InvalidTransactionId and HEAP_XMAX_INVALID separately
	 * for xmax annotation, since they can be set separately in a way that
	 * might be interesting.  Also indicate (redundantly) if the xmax is a
	 * MultiXactId, or is the XID on a non-updating locker xact.
	 *
	 * Representing HEAP_XMAX_IS_MULTI and HEAP_XMAX_LOCK_ONLY but not
	 * HEAP_XMAX_COMMITTED here is a bit arbitrary.  We do this because
	 * HEAP_XMAX_IS_MULTI and HEAP_XMAX_LOCK_ONLY are basic facts about the
	 * class of data that the xmax field contains, as opposed to status
	 * information for the tuple as a whole.
	 *
	 * The general idea is to make it as easy as possible for the user to get
	 * a sense of the structure of update chains on the page.
	 */
	if ((htup->t_infomask & HEAP_XMAX_IS_MULTI) != 0)
	{
		strcat(xmax, " - HEAP_XMAX_IS_MULTI");
		xmaxFontColor = COLOR_GREEN_DARK;
	}
	if (rawXmax == InvalidTransactionId)
	{
		strcat(xmax, " - InvalidTransactionId");
		xmaxFontColor = COLOR_YELLOW_LIGHT;
	}
	if ((htup->t_infomask & HEAP_XMAX_INVALID) != 0)
	{
		strcat(xmax, " - HEAP_XMAX_INVALID");
		/* Matches InvalidTransactionId case */
		xmaxFontColor = COLOR_YELLOW_LIGHT;
	}

	/*
	 * Handle HEAP_XMAX_LOCK_ONLY last, since the HEAP_XMAX_INVALID hint seems
	 * like it shouldn't affect font color.
	 */
	if ((htup->t_infomask & HEAP_XMAX_LOCK_ONLY) != 0)
	{
		/*
		 * This color is deliberately chosen to be similar to the special
		 * t_ctid font colors
		 */
		strcat(xmax, " - HEAP_XMAX_LOCK_ONLY");
		xmaxFontColor = COLOR_BLUE_DARK;
	}

	relfileOffNext = relfileOff + sizeof(TransactionId);
	EmitXmlTupleTagFont(blkno, offset, xmin, COLOR_RED_LIGHT, xminFontColor,
						relfileOff, relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(TransactionId);
	EmitXmlTupleTagFont(blkno, offset, xmax, COLOR_RED_LIGHT, xmaxFontColor,
						relfileOff, relfileOffNext - 1);
	relfileOff = relfileOffNext;

	if (!(htup->t_infomask & HEAP_MOVED))
	{
		/*
		 * t_cid is COLOR_RED_DARK in order to signal that it's associated
		 * with though somewhat different to xmin and xmax.
		 */
		relfileOffNext += sizeof(CommandId);
		EmitXmlTupleTag(blkno, offset, "t_cid", COLOR_RED_DARK, relfileOff,
						relfileOffNext - 1);
	}
	else
	{
		/*
		 * This must be a rare case where pg_upgrade has been run, and we're
		 * left with a tuple with a t_xvac field instead of a t_cid field,
		 * because at some point old-style VACUUM FULL was run. (This would
		 * have had to have been on or before version 9.0, which has been out
		 * of support for some time.)
		 *
		 * Make it COLOR_PINK, so that it sticks out like a sore thumb.
		 */
		StaticAssertStmt(sizeof(CommandId) == sizeof(TransactionId),
						 "t_cid width must match t_xvac");
		relfileOffNext += sizeof(TransactionId);
		EmitXmlTupleTag(blkno, offset, "t_xvac", COLOR_PINK, relfileOff,
						relfileOffNext - 1);
	}

	/*
	 * Don't use ItemPointerData directly, to avoid having apparent mix in
	 * endianness in these fields.  Delineate which subfield is which by using
	 * multiple tags.
	 *
	 * The block component of each TID is COLOR_BLUE_LIGHT.  The same color is
	 * used for ItemIds, since both are physical pointers.  offsetNumber is a
	 * logical pointer, though, and so we make that COLOR_BLUE_DARK to
	 * slightly distinguish it.
	 *
	 * It seems useful to provide a subtle cue about whether or not the tuple
	 * is the latest version within the t_ctid subfields, since this helps the
	 * user to notice update chains.  Check if tuple's t_ctid points to the
	 * tuple itself; if it does, use non-contrasting font colors to
	 * deemphasize.
	 */
	blkFontColor = COLOR_FONT_STANDARD;
	offsetFontColor = COLOR_FONT_STANDARD;
	if (ItemPointerGetBlockNumber(&htup->t_ctid) == logBlock &&
		ItemPointerGetOffsetNumber(&htup->t_ctid) == offset)
	{
		blkFontColor = COLOR_BLUE_DARK;
		offsetFontColor = COLOR_BLUE_LIGHT;
	}
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTagFont(blkno, offset, "t_ctid->bi_hi", COLOR_BLUE_LIGHT,
						blkFontColor, relfileOff, relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTagFont(blkno, offset, "t_ctid->bi_lo", COLOR_BLUE_LIGHT,
						blkFontColor, relfileOff, relfileOffNext - 1);

	/*
	 * Note: offsetNumber could be SpecTokenOffsetNumber, but we don't
	 * annotate that
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTagFont(blkno, offset, "t_ctid->offsetNumber", COLOR_BLUE_DARK,
						offsetFontColor, relfileOff, relfileOffNext - 1);

	flagString = GetHeapTupleHeaderFlags(htup, true);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_GREEN_LIGHT, relfileOff,
					relfileOffNext - 1);
	pg_free(flagString);
	flagString = GetHeapTupleHeaderFlags(htup, false);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_GREEN_DARK, relfileOff,
					relfileOffNext - 1);
	pg_free(flagString);

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, in line
	 * with general convention.  t_hoff is a fixed addressable field, so we
	 * make it COLOR_YELLOW_LIGHT to represent that it's associated but
	 * distinct.
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint8);
	EmitXmlTupleTag(blkno, offset, "t_hoff", COLOR_YELLOW_LIGHT, relfileOff,
					relfileOffNext - 1);

	/*
	 * We consider the size of the t_bits field (if any) to be what the
	 * att_isnull() macro requires.  There is one bit per attribute, which is
	 * rounded up to the nearest byte boundary.
	 *
	 * This often leaves a conspicuous empty space between the t_bits area and
	 * the first attribute/beginning of tuple contents.  This happens because
	 * the first attribute of the tuple must be accessed at a MAXALIGN()'d
	 * offset relative to the start of the tuple (often 32 bytes from the
	 * beginning of the tuple).  The empty space for alignment doesn't seem
	 * like it should count as t_bits overhead.
	 *
	 * (t_bits doesn't have any alignment requirements, which is why there can
	 * be a one byte t_bits array when there aren't so many attributes.  That
	 * can fit snugly before the first attribute, which will only have a 24
	 * byte offset from the beginning of the tuple provided there is no oid
	 * field.)
	 */
	relfileOff = relfileOffNext;
	relfileOffNext = relfileOffOrig + htup->t_hoff;
	if (htup->t_infomask & HEAP_HASOID)
		relfileOffNext -= sizeof(Oid);
	if (htup->t_infomask & HEAP_HASNULL)
		EmitXmlTupleTag(blkno, offset, "t_bits", COLOR_YELLOW_DARK, relfileOff,
						relfileOff + ((HeapTupleHeaderGetNatts(htup) + 0x07) >> 3) - 1);

	/*
	 * Represent Oid as a distinct field with the same color as t_bits, since
	 * it's also an optional heap tuple header.
	 */
	if (htup->t_infomask & HEAP_HASOID)
	{
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(Oid);
		EmitXmlTupleTag(blkno, offset, "HeapTupleHeaderGetOid()",
						COLOR_YELLOW_DARK, relfileOff, relfileOffNext - 1);
	}

	/*
	 * Handle rare edge case where tuple has no contents because it consists
	 * entirely of NULL attributes.  We trust lp_len to handle this, which is
	 * what caller passed us.
	 */
	if (itemSize == (relfileOffNext - relfileOffOrig))
		return;
	else if (itemSize < (relfileOffNext - relfileOffOrig))
	{
		fprintf(stderr, "pg_hexedit error: lp_len %d from (%u,%u) is undersized\n",
				itemSize, blkno + segmentBlockDelta, offset);
		exitCode = 1;
		return;
	}

	relfileOff = relfileOffNext;
	EmitXmlAttributesHeap(blkno, offset, relfileOff, htup, itemSize);
}

/*
 * Emit a wxHexEditor tag for entire index tuple.
 *
 * Function deals with B-Tree, GiST, and hash tuples in a generic way, because
 * they use the IndexTuple format without adornment.  These index AMs use line
 * pointer metadata to represent that index tuples are logically dead by
 * setting the LP_DEAD bit.  Unlike the heap LP_DEAD case, there will still be
 * a tuple on the page when this is set (the tuple "has storage").  Caller
 * passes an argument that has us redundantly use color to represent that the
 * index tuple is dead, meaning that the space it occupies will soon be
 * recycled.
 *
 * This function is also used for GIN tuples in pending list and main B-Tree
 * key pages, and so must deal with the various abuses of the IndexTuple format
 * that GIN makes to store posting lists in main B-Tree key pages (pending
 * pages don't compress TIDs, and posting tree pages are dealt with in special
 * GIN-only paths).
 *
 * This function is not used for BrinTuples, because they share nothing with
 * IndexTuples in terms of layout, despite being conceptually similar (BRIN
 * tuples store blocks, not exact TIDs).
 *
 * Note: Caller does not need to pass itemSize from ItemId, because that's
 * redundant in the case of IndexTuples, and because SP-GiST callers will not
 * be able to pass an lp_len for an inner-node-contained IndexTuple.  However,
 * most still pass it, since it's a useful cross-check in the event of a torn
 * page.
 */
static void
EmitXmlIndexTuple(Page page, BlockNumber blkno, OffsetNumber offset,
				  IndexTuple tuple, uint32 relfileOff, int itemSize, bool dead)
{
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;
	char	   *tagColor;
	char	   *fontColor;
	char	   *flagString;

	/* Make font color indicate if LP_DEAD bit is set */
	fontColor = dead ? COLOR_BROWN : COLOR_FONT_STANDARD;

	if (itemSize < 0)
		itemSize = IndexTupleSize(tuple);
	else if (itemSize != IndexTupleSize(tuple))
	{
		fprintf(stderr, "pg_hexedit error: (%u,%u) lp_len %u does not equal IndexTupleSize() %lu\n",
				blkno + segmentBlockDelta, offset, itemSize,
				IndexTupleSize(tuple));
		exitCode = 1;
		itemSize = Max(sizeof(IndexTupleData),
					   Min(itemSize, IndexTupleSize(tuple)));
	}

	if (specialType != SPEC_SECT_INDEX_GIN || !GinPageIsLeaf(page) ||
		GinIsPostingTree(tuple))
	{
		/*
		 * Emit t_tid tags.  TID tag style should be kept consistent with
		 * EmitXmlHeapTuple().
		 */
		relfileOffNext = relfileOff + sizeof(uint16);
		tagColor = dead ? COLOR_BLACK : COLOR_BLUE_LIGHT;
		EmitXmlTupleTagFont(blkno, offset, "t_tid->bi_hi",
							tagColor, fontColor,
							relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(uint16);
		EmitXmlTupleTagFont(blkno, offset, "t_tid->bi_lo",
							tagColor, fontColor,
							relfileOff, relfileOffNext - 1);

		/*
		 * Handle cases where item pointer offset is abused, but t_tid still
		 * contains a valid block number.  These cases are handled here
		 * because they involve IndexTuples that contain a t_tid that is still
		 * essentially a conventional TID.  These cases are:
		 *
		 * 1. GIN posting tree pointers (within the leaf level of the main
		 * entry tree).  The fact that it's a posting tree pointer (and not
		 * the start of a posting list) is indicated by using the magic offset
		 * number GIN_TREE_POSTING.  We use GinIsPostingTree() to test this.
		 *
		 * 2. nbtree pivot tuples that have undergone truncation (PostgreSQL
		 * v11+ only).  This is indicated by the INDEX_ALT_TID_MASK bit having
		 * been set.  The offset field holds the actual number of attributes.
		 * The nbtree code uses BTreeTupleGetNAtts() to test this.
		 */
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(uint16);
		tagColor = dead ? COLOR_BLACK : COLOR_BLUE_DARK;
		if (specialType == SPEC_SECT_INDEX_GIN && GinIsPostingTree(tuple))
			EmitXmlTupleTagFont(blkno, offset,
								"t_tid->offsetNumber/GinIsPostingTree()",
								tagColor, fontColor,
								relfileOff, relfileOffNext - 1);
#if PG_VERSION_NUM >= 110000
		else if (specialType == SPEC_SECT_INDEX_BTREE &&
#if PG_VERSION_NUM < 130000
				 (tuple->t_info & INDEX_ALT_TID_MASK) != 0)
#else
				 BTreeTupleIsPivot(tuple)) /* This macro is only in Postgres 13 */
#endif /* PG_VERSION_NUM < 130000 */
			EmitXmlTupleTagFont(blkno, offset,
								"t_tid->offsetNumber/BTreeTupleGetNAtts()",
								tagColor, fontColor,
								relfileOff, relfileOffNext - 1);
#if PG_VERSION_NUM >= 130000
		else if (specialType == SPEC_SECT_INDEX_BTREE &&
				 BTreeTupleIsPosting(tuple))
			EmitXmlTupleTagFont(blkno, offset,
								"t_tid->offsetNumber/BTreeTupleGetNPosting()",
								tagColor, fontColor,
								relfileOff, relfileOffNext - 1);
#endif /* PG_VERSION_NUM >= 130000 */
#endif /* PG_VERSION_NUM >= 110000 */

		/*
		 * Regular/common case, where offset number is actually intended to be
		 * accessed as a conventional offset (i.e. accessed using macros such
		 * as ItemPointerGetOffsetNumber())
		 */
		else
			EmitXmlTupleTagFont(blkno, offset, "t_tid->offsetNumber",
								tagColor, fontColor,
								relfileOff, relfileOffNext - 1);
	}
	else
	{
		/*
		 * GIN posting lists (within the leaf level of the main entry tree)
		 * abuse every item pointer field, so everything is handled here all
		 * at once.  Naturally, there are block numbers (as well as offset
		 * numbers) contained within posting lists, since a posting list is
		 * literally a list of TIDs.  However, none of this information is
		 * accessed in the conventional manner.
		 */
		relfileOffNext = relfileOff + sizeof(uint16);
		tagColor = dead ? COLOR_BLACK : COLOR_BLUE_LIGHT;
		EmitXmlTupleTagFont(blkno, offset, "t_tid->bi_hi/GinItupIsCompressed()",
							tagColor, fontColor,
							relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(uint16);
		EmitXmlTupleTagFont(blkno, offset,
							"t_tid->bi_lo/GinGetPostingOffset()",
							tagColor, fontColor,
							relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(uint16);
		tagColor = dead ? COLOR_BLACK : COLOR_BLUE_DARK;
		EmitXmlTupleTagFont(blkno, offset,
							"t_tid->offsetNumber/GinGetNPosting()",
							tagColor, fontColor,
							relfileOff, relfileOffNext - 1);
	}

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, which
	 * also matches EmitXmlHeapTuple()
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(unsigned short);
	flagString = GetIndexTupleFlags(tuple);
	tagColor = dead ? COLOR_BLACK : COLOR_YELLOW_DARK;
	EmitXmlTupleTagFont(blkno, offset, flagString,
						tagColor, fontColor,
						relfileOff, relfileOffNext - 1);
	pg_free(flagString);
	relfileOff = relfileOffNext;

	/*
	 * NULL bitmap, if any, is counted as a separate tag, and not an extension
	 * of t_info.  This is a little arbitrary, but makes more sense overall.
	 * This matches heap tuple header tags.
	 *
	 * GIN has special rules for multicolumn indexes.  We don't break down the
	 * structure of GIN's special representation of NULLness because doing so
	 * requires access to catalog metadata.  See the GIN README for details.
	 *
	 * SP-GiST node tuples (from internal SP-GiST pages) do not have a NULL
	 * bitmap, since there is implicitly only ever one attribute that could be
	 * NULL, so the bit alone suffices.  See comments above
	 * SpGistNodeTupleData.
	 */
	Assert(specialType != SPEC_SECT_INDEX_SPGIST || !SpGistPageIsLeaf(page));
	if (IndexTupleHasNulls(tuple) && specialType != SPEC_SECT_INDEX_SPGIST)
	{
		relfileOffNext +=
			(IndexInfoFindDataOffset(tuple->t_info) - (relfileOff - relfileOffOrig));

		tagColor = dead ? COLOR_BLACK : COLOR_YELLOW_DARK;
		EmitXmlTupleTagFont(blkno, offset, "IndexAttributeBitMapData array",
							tagColor, fontColor,
							relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
	}

	/*
	 * Tuple contents, plus possible posting list for GIN leaf pages.
	 *
	 * All-attributes-NULL IndexTuples will not have any contents here, so we
	 * avoid creating a tuple content tag entirely.  The same applies to
	 * "minus infinity" items from nbtree internal pages (though they don't
	 * have a NULL bitmap).
	 *
	 * Tuple contents is represented in the same way in the event of a dead
	 * tuple.
	 */
	relfileOffNext = relfileOffOrig + itemSize;
	if (relfileOff < relfileOffNext)
	{
		/*
		 * If this is a GIN page, we've already determined that this tuple is
		 * from the main key B-Tree (posting trees don't use IndexTuples at
		 * all).  We should only treat it as containing a posting list (in
		 * addition to tuple contents) if:
		 *
		 * 1. It does not point to a posting tree.  Pointers to posting trees
		 * are always simple block numbers (the posting tree root page block)
		 * with magic offset number GIN_TREE_POSTING.
		 *
		 * 2. The posting list consists of one or more items.  See
		 * ginReadTuple().
		 *
		 * 3. This isn't an internal page IndexTuple.  These use the item
		 * pointer representation in the conventional way.
		 *
		 * 4. This isn't a pending list page.  These also don't abuse the item
		 * pointer representation.
		 *
		 * The !GinPageIsLeaf() part of the test handles points 3 and 4.
		 */
		if (specialType != SPEC_SECT_INDEX_GIN || !GinPageIsLeaf(page) ||
			GinIsPostingTree(tuple) || GinGetNPosting(tuple) == 0)
			EmitXmlAttributesIndex(blkno, offset, relfileOff, tuple,
								   relfileOffOrig, itemSize);
		else
		{
			Size		postoffset = itemSize - GinGetPostingOffset(tuple);
			const char *color;

			EmitXmlAttributesIndex(blkno, offset, relfileOff, tuple, relfileOffOrig,
								   GinGetPostingOffset(tuple));
			relfileOff = relfileOffNext - postoffset;

			/*
			 * Compressed TIDs are orange.  Old Postgres versions have
			 * old-style uncompressed lists of TIDs in leaf pages, so their
			 * posting lists should be blue instead of orange, like regular
			 * block number item pointer fields.  See ginPostingListDecode()
			 * for details on how this representation is decompressed.
			 */
			color =
			GinItupIsCompressed(tuple) ? COLOR_ORANGE : COLOR_BLUE_LIGHT;

			EmitXmlTupleTag(blkno, offset, "posting list", color, relfileOff,
							relfileOffNext - 1);
		}
	}
}

/*
 * Emit a wxHexEditor tag for internal SP-GiST page tuple.
 *
 * This is similar to EmitXmlIndexTuple(), but SP-GiST never directly uses
 * IndexTuple representation, so requires this custom tuple formatting
 * function.  (Actually, SP-GiST internal/inner tuples end up reusing the
 * IndexTuple representation internally, which we must deal with here.)
 *
 * We are prepared for the possibility that tuple is actually SpGistDeadTuple.
 *
 * Note that lp_len isn't needed here, since it's redundant, just as it is with
 * the IndexTuple representation.
 */
static void
EmitXmlSpGistInnerTuple(Page page, BlockNumber blkno, OffsetNumber offset,
						SpGistInnerTuple tuple, uint32 relfileOff)
{
	SpGistNodeTuple node;
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;
	char	   *flagString;
	bool		dead = (tuple->tupstate != SPGIST_LIVE);
	int			i;

	Assert(!SpGistPageIsLeaf(page));
	Assert(!SpGistPageIsMeta(page));

	/*
	 * Reuse dead tuple handling within leaf tuple routine.  This correctly
	 * indicates "inner page leaf tuple" size by interpreting the leaf tuple
	 * "size" field, and there are no other fields that could be interpreted
	 * incorrectly, so this seems to be the right approach.
	 */
	if (dead)
	{
		EmitXmlSpGistLeafTuple(page, blkno, offset, (SpGistLeafTuple) tuple,
							   relfileOff);
		return;
	}

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_LIGHT, to
	 * indicate that the field is metadata, but to create a contrast with
	 * IndexTuple metadata fields (which are COLOR_YELLOW_DARK).
	 */
	flagString = GetSpGistInnerTupleState(tuple);
	relfileOffNext = relfileOff + sizeof(unsigned int);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_YELLOW_LIGHT, relfileOff,
					relfileOffNext - 1);
	pg_free(flagString);

	relfileOff = relfileOffNext;
	relfileOffNext = relfileOff + sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "SpGistInnerTuple size", COLOR_YELLOW_LIGHT,
					relfileOff, relfileOffNext - 1);

	/*
	 * Emit node tuple's contents (prefix).  The prefix value is optional.
	 * Some SP-GiST operator classes never use them.  See spgist/README.
	 */
	relfileOff = relfileOffOrig + SGNTHDRSZ;
	relfileOffNext = relfileOffOrig + SGNTHDRSZ + tuple->prefixSize;
	if (relfileOff < relfileOffNext)
		EmitXmlTupleTag(blkno, offset, "contents (prefix)", COLOR_WHITE,
						relfileOff, relfileOffNext - 1);

	/*
	 * Print all SpGistNodeTuple entries, which actually share IndexTuple
	 * representation
	 */
	SGITITERATE(tuple, i, node)
	{
		EmitXmlIndexTuple(page, blkno, offset, node,
						  relfileOffOrig + ((char *) node - (char *) tuple),
						  -1, false);
	}
}

/*
 * Emit a wxHexEditor tag for leaf SP-GiST page tuple.
 *
 * We are prepared for the possibility that tuple is actually SpGistDeadTuple.
 */
static void
EmitXmlSpGistLeafTuple(Page page, BlockNumber blkno, OffsetNumber offset,
					   SpGistLeafTuple tuple, uint32 relfileOff)
{
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;
	char	   *flagString;
	bool		dead = (tuple->tupstate != SPGIST_LIVE);

	Assert(!SpGistPageIsMeta(page));

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_LIGHT, to
	 * indicate that the field is metadata, but to create a contrast with
	 * IndexTuple metadata fields (which are COLOR_YELLOW_DARK).
	 */
	flagString = GetSpGistLeafTupleState(tuple);
	relfileOffNext = relfileOff + sizeof(unsigned int);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_YELLOW_LIGHT, relfileOff,
					relfileOffNext - 1);
	pg_free(flagString);

	relfileOff = relfileOffNext;
	relfileOffNext = relfileOff + sizeof(OffsetNumber);
	EmitXmlTupleTag(blkno, offset, "nextOffset", COLOR_YELLOW_DARK,
					relfileOff, relfileOffNext - 1);

	/* Heap pointer */
	relfileOff = relfileOffNext;
	relfileOffNext = relfileOff + sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, dead ? "pointer->bi_hi" : "heapPtr->bi_hi",
					COLOR_BLUE_LIGHT, relfileOff, relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, dead ? "pointer->bi_lo" : "heapPtr->bi_lo",
					COLOR_BLUE_LIGHT, relfileOff, relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, dead ? "pointer->offsetNumber" : "heapPtr->offsetNumber",
					COLOR_BLUE_DARK, relfileOff, relfileOffNext - 1);

	if (!dead)
	{
		/* Must not be inner tuple if not dead */
		Assert(SpGistPageIsLeaf(page));

		/* Emit tuple contents */
		relfileOff = relfileOffOrig + SGLTHDRSZ;
		relfileOffNext = relfileOffOrig + tuple->size;
		if (relfileOff < relfileOffNext)
			EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE,
							relfileOff, relfileOffNext - 1);
	}
	else
	{
		/* XID */
		relfileOff = relfileOffNext;
		relfileOffNext = relfileOff + sizeof(TransactionId);
		EmitXmlTupleTag(blkno, offset, "xid", COLOR_RED_LIGHT,
						relfileOff, relfileOffNext - 1);
	}
}

/*
 * Emit a wxHexEditor tag for entire BRIN regular page tuple.
 *
 * This is similar to EmitXmlIndexTuple(), but BRIN never uses IndexTuple
 * representation, so requires this custom tuple formatting function.
 */
static void
EmitXmlBrinTuple(Page page, BlockNumber blkno, OffsetNumber offset,
				 BrinTuple *tuple, uint32 relfileOff, int itemSize)
{
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;
	char	   *flagString;

	if (!BRIN_IS_REGULAR_PAGE(page))
	{
		fprintf(stderr, "pg_hexedit error: non-regular BRIN page formatted as regular");
		exitCode = 1;
	}

	/*
	 * Emit bt_info tags.  A straight block number is used here, in contrast
	 * to the legacy bi_hi/bi_lo representation used everywhere else.  We
	 * still match color/style, though.
	 */
	relfileOffNext = relfileOff + sizeof(BlockNumber);
	EmitXmlTupleTag(blkno, offset, "bt_blkno", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, which
	 * also matches EmitXmlHeapTuple()
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(unsigned short);
	flagString = GetBrinTupleFlags(tuple);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);
	pg_free(flagString);
	relfileOff = relfileOffNext;

	/*
	 * NULL bitmap, if any, is counted as a separate tag, and not an extension
	 * of bt_info.  This matches index tuples.
	 */
	if (BrinTupleHasNulls(tuple))
	{
		relfileOffNext +=
			(BrinTupleDataOffset(tuple) - (relfileOff - relfileOffOrig));

		EmitXmlTupleTag(blkno, offset, "IndexAttributeBitMapData array",
						COLOR_YELLOW_DARK, relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
	}

	/*
	 * Tuple contents.
	 *
	 * All-attributes-NULL BrinTuples will not have any contents here, so we
	 * avoid creating a tuple content tag entirely.
	 *
	 * We use the lp_len value here, since there is no IndexTupleSize()
	 * equivalent -- lp_len is really all we have to go on in the case of BRIN
	 * tuples.
	 *
	 * Tagging individual attributes is hard for BRIN indexes, so we don't do
	 * that here.  We'd need to know how many columns are stored for each
	 * attribute, which isn't stored in the system catalogs or in the on-disk
	 * representation.  BrinOpcInfo.oi_nstored stores this information, which
	 * is returned by one of the opclass methods, so full backend access is
	 * truly necessary (in practice it's 2 for minmax operator classes, and 3
	 * for inclusion operator classes).
	 */
	relfileOffNext = relfileOffOrig + itemSize;
	if (relfileOff < relfileOffNext)
		EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE, relfileOff,
						relfileOffNext - 1);
}

/*
 * Dump out a formatted block header for the requested block.
 */
static int
EmitXmlPageHeader(Page page, BlockNumber blkno, uint32 level)
{
	int			rc = 0;
	unsigned int headerBytes;

	/*
	 * Only attempt to format the header if the entire header (minus the item
	 * array) is available
	 */
	if (bytesToFormat < offsetof(PageHeaderData, pd_linp[0]))
	{
		headerBytes = bytesToFormat;
		rc = EOF_ENCOUNTERED;
	}
	else
	{
		/* Interpret the contents of the header */
		PageHeader	pageHeader = (PageHeader) page;
		XLogRecPtr	pageLSN = GetPageLsn(page);
		int			maxOffset = PageGetMaxOffsetNumber(page);
		char	   *flagString;

		headerBytes = offsetof(PageHeaderData, pd_linp[0]);
		blockVersion = (unsigned int) PageGetPageLayoutVersion(page);

		/* We don't count itemidarray as header */
		if (maxOffset > 0)
		{
			unsigned int itemsLength = maxOffset * sizeof(ItemIdData);

			if (bytesToFormat < (headerBytes + itemsLength))
			{
				headerBytes = bytesToFormat;
				rc = EOF_ENCOUNTERED;
			}
		}

		/*
		 * For historical reasons, the 64-bit page header LSN value is stored
		 * as two 32-bit values.  This makes interpreting what is really just
		 * a 64-bit unsigned int confusing on little-endian systems, because
		 * the bytes are "in big endian order" across its two 32-bit halves,
		 * but are in the expected little-endian order *within* each half.
		 *
		 * This is rather similar to the situation with t_ctid.  Unlike in
		 * that case, we choose to make LSN a single field here, because we
		 * don't want to have two tooltips with the format value for each
		 * field.
		 */
		flagString = pg_malloc(128);
		sprintf(flagString, "LSN: %X/%08X", (uint32) (pageLSN >> 32), (uint32) pageLSN);
		EmitXmlTag(blkno, level, flagString, COLOR_YELLOW_LIGHT, pageOffset,
				   (pageOffset + sizeof(PageXLogRecPtr)) - 1);
		EmitXmlTag(blkno, level, "checksum", COLOR_GREEN_DARK,
				   pageOffset + offsetof(PageHeaderData, pd_checksum),
				   (pageOffset + offsetof(PageHeaderData, pd_flags)) - 1);

		/* Generate generic page header flags (reuse buffer) */
		flagString[0] = '\0';
		strcat(flagString, "pd_flags - ");
		if (pageHeader->pd_flags & PD_HAS_FREE_LINES)
			strcat(flagString, "PD_HAS_FREE_LINES|");
		if (pageHeader->pd_flags & PD_PAGE_FULL)
			strcat(flagString, "PD_PAGE_FULL|");
		if (pageHeader->pd_flags & PD_ALL_VISIBLE)
			strcat(flagString, "PD_ALL_VISIBLE|");
		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';

		EmitXmlTag(blkno, level, flagString, COLOR_YELLOW_DARK,
				   pageOffset + offsetof(PageHeaderData, pd_flags),
				   (pageOffset + offsetof(PageHeaderData, pd_lower)) - 1);
		pg_free(flagString);
		EmitXmlTag(blkno, level, "pd_lower", COLOR_MAROON,
				   pageOffset + offsetof(PageHeaderData, pd_lower),
				   (pageOffset + offsetof(PageHeaderData, pd_upper)) - 1);
		EmitXmlTag(blkno, level, "pd_upper", COLOR_MAROON,
				   pageOffset + offsetof(PageHeaderData, pd_upper),
				   (pageOffset + offsetof(PageHeaderData, pd_special)) - 1);
		EmitXmlTag(blkno, level, "pd_special", COLOR_GREEN_BRIGHT,
				   pageOffset + offsetof(PageHeaderData, pd_special),
				   (pageOffset + offsetof(PageHeaderData, pd_pagesize_version)) - 1);
		EmitXmlTag(blkno, level, "pd_pagesize_version", COLOR_BROWN,
				   pageOffset + offsetof(PageHeaderData, pd_pagesize_version),
				   (pageOffset + offsetof(PageHeaderData, pd_prune_xid)) - 1);
		EmitXmlTag(blkno, level, "pd_prune_xid", COLOR_RED_LIGHT,
				   pageOffset + offsetof(PageHeaderData, pd_prune_xid),
				   (pageOffset + offsetof(PageHeaderData, pd_linp[0])) - 1);

		/*
		 * Eye the contents of the header and alert the user to possible
		 * problems
		 */
		if ((maxOffset < 0) ||
			(maxOffset > blockSize) ||
			(blockVersion != PG_PAGE_LAYOUT_VERSION) || /* only one we support */
			(pageHeader->pd_upper > blockSize) ||
			(pageHeader->pd_upper > pageHeader->pd_special) ||
			(pageHeader->pd_lower <
			 (sizeof(PageHeaderData) - sizeof(ItemIdData)))
			|| (pageHeader->pd_lower > blockSize)
			|| (pageHeader->pd_upper < pageHeader->pd_lower)
			|| (pageHeader->pd_special > blockSize))
		{
			fprintf(stderr, "pg_hexedit error: invalid header information\n");
			exitCode = 1;
		}

		/*
		 * Verify checksums as valid if requested.
		 *
		 * Caller may want us to skip zero checksums.
		 */
		if (blockOptions & BLOCK_CHECKSUMS ||
			((blockOptions & BLOCK_ZEROSUMS) && pageHeader->pd_checksum != 0))
		{
			uint16		calc_checksum;

			calc_checksum = pg_checksum_page(page, blkno + segmentBlockDelta);
			if (calc_checksum != pageHeader->pd_checksum)
			{
				fprintf(stderr, "pg_hexedit error: checksum failure in block %u (calculated 0x%04x)\n",
						blkno, calc_checksum);
				exitCode = 1;
			}
		}
	}

	/*
	 * If we have reached the end of file while interpreting the header, give
	 * up
	 */
	if (rc == EOF_ENCOUNTERED)
	{
		fprintf(stderr, "pg_hexedit error: end of block encountered within page header with bytes read: %4u\n",
				bytesToFormat);
		exitCode = 1;
	}

	return rc;
}

/*
 * Dump out a formatted metapage tags for metapage block.
 */
static void
EmitXmlPageMeta(BlockNumber blkno, uint32 level)
{
	uint32		metaStartOffset = pageOffset + MAXALIGN(SizeOfPageHeaderData);

	if (specialType == SPEC_SECT_INDEX_BTREE && blkno == BTREE_METAPAGE)
	{
		EmitXmlTag(InvalidBlockNumber, level, "btm_magic", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_magic),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_version) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "btm_version", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_version),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_root) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "btm_root", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_root),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_level) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "btm_level", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_level),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_fastroot) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "btm_fastroot", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_fastroot),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "btm_fastlevel", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel),
#if PG_VERSION_NUM < 110000
				   (metaStartOffset + sizeof(BTMetaPageData) - 1));
#else
				   (metaStartOffset + offsetof(BTMetaPageData, btm_oldest_btpo_xact) - 1));

		/*
		 * These fields are only actually active when btm_version >= 3 (which
		 * is v11's standard BTREE_VERSION)
		 */
		EmitXmlTag(InvalidBlockNumber, level, "btm_oldest_btpo_xact", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_oldest_btpo_xact),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_last_cleanup_num_heap_tuples) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "btm_last_cleanup_num_heap_tuples", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_last_cleanup_num_heap_tuples),
#if PG_VERSION_NUM < 130000
				   (metaStartOffset + sizeof(BTMetaPageData) - 1));
#else
				   (metaStartOffset + offsetof(BTMetaPageData, btm_allequalimage) - 1));

		/* New metapage field added in Postgres 13: */
		EmitXmlTag(InvalidBlockNumber, level, "btm_allequalimage", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_allequalimage),
				   (metaStartOffset + sizeof(BTMetaPageData) - 1));
#endif /* PG_VERSION_NUM < 130000 */
#endif /* PG_VERSION_NUM < 110000 */
	}
	else if (specialType == SPEC_SECT_INDEX_HASH && blkno == HASH_METAPAGE)
	{
		EmitXmlTag(InvalidBlockNumber, level, "hashm_magic", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_magic),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_version) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_version", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_version),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_ntuples) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_ntuples", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_ntuples),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_ffactor) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_ffactor", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_ffactor),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_bsize) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_bsize", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_bsize),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_bmsize) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_bmsize", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_bmsize),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_bmshift) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_bmshift", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_bmshift),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_maxbucket) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_maxbucket", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_maxbucket),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_highmask) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_highmask", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_highmask),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_lowmask) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_lowmask", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_lowmask),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_ovflpoint) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_ovflpoint", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_ovflpoint),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_firstfree) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_firstfree", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_firstfree),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_nmaps) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_nmaps", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_nmaps),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_procid) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_procid", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_procid),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_spares) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_spares", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_spares),
				   (metaStartOffset + offsetof(HashMetaPageData, hashm_mapp) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "hashm_mapp", COLOR_PINK,
				   metaStartOffset + offsetof(HashMetaPageData, hashm_mapp),
				   (metaStartOffset + sizeof(HashMetaPageData)) - 1);
	}
	else if (specialType == SPEC_SECT_INDEX_GIN && blkno == GIN_METAPAGE_BLKNO)
	{
		EmitXmlTag(InvalidBlockNumber, level, "head", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, head),
				   (metaStartOffset + offsetof(GinMetaPageData, tail) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "tail", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, tail),
				   (metaStartOffset + offsetof(GinMetaPageData, tailFreeSize) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "tailFreeSize", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, tailFreeSize),
				   (metaStartOffset + offsetof(GinMetaPageData, nPendingPages) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "nPendingPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nPendingPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nPendingHeapTuples) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "nPendingHeapTuples", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nPendingHeapTuples),
				   (metaStartOffset + offsetof(GinMetaPageData, nTotalPages) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "nTotalPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nTotalPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nEntryPages) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "nEntryPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nEntryPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nDataPages) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "nDataPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nDataPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nEntries) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "nEntries", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nEntries),
				   (metaStartOffset + offsetof(GinMetaPageData, ginVersion) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "ginVersion", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, ginVersion),
				   (metaStartOffset + sizeof(GinMetaPageData)) - 1);
	}
	else if (specialType == SPEC_SECT_INDEX_SPGIST && blkno == SPGIST_METAPAGE_BLKNO)
	{
		uint32		cachedOffset = metaStartOffset;
		int			i;

		EmitXmlTag(InvalidBlockNumber, level, "magicNumber", COLOR_PINK,
				   metaStartOffset + offsetof(SpGistMetaPageData, magicNumber),
				   (metaStartOffset + offsetof(SpGistMetaPageData, lastUsedPages) - 1));

		cachedOffset += offsetof(SpGistMetaPageData, lastUsedPages);

		for (i = 0; i < SPGIST_CACHED_PAGES; i++)
		{
			EmitXmlTag(InvalidBlockNumber, level, "lastUsedPages.blkno", COLOR_PINK,
					   cachedOffset,
					   (cachedOffset + offsetof(SpGistLastUsedPage, freeSpace)) - 1);
			cachedOffset += offsetof(SpGistLastUsedPage, freeSpace);
			EmitXmlTag(InvalidBlockNumber, level, "lastUsedPages.freeSpace", COLOR_PINK,
					   cachedOffset,
					   (cachedOffset + sizeof(int) - 1));
			cachedOffset += sizeof(int);
		}
	}
	else if (specialType == SPEC_SECT_INDEX_BRIN && blkno == BRIN_METAPAGE_BLKNO)
	{
		EmitXmlTag(InvalidBlockNumber, level, "brinMagic", COLOR_PINK,
				   metaStartOffset + offsetof(BrinMetaPageData, brinMagic),
				   (metaStartOffset + offsetof(BrinMetaPageData, brinVersion) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "brinVersion", COLOR_PINK,
				   metaStartOffset + offsetof(BrinMetaPageData, brinVersion),
				   (metaStartOffset + offsetof(BrinMetaPageData, pagesPerRange) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "pagesPerRange", COLOR_PINK,
				   metaStartOffset + offsetof(BrinMetaPageData, pagesPerRange),
				   (metaStartOffset + offsetof(BrinMetaPageData, lastRevmapPage) - 1));
		EmitXmlTag(InvalidBlockNumber, level, "lastRevmapPage", COLOR_PINK,
				   metaStartOffset + offsetof(BrinMetaPageData, lastRevmapPage),
				   (metaStartOffset + sizeof(BrinMetaPageData)) - 1);
	}
	else
	{
		fprintf(stderr, "pg_hexedit error: unsupported metapage special section type \"%s\"\n",
				GetSpecialSectionString(specialType));
		exitCode = 1;
	}
}

/*
 * Emit formatted ItemId tags for tuples that reside on this block.
 */
static void
EmitXmlPageItemIdArray(Page page, BlockNumber blkno)
{
	int			maxOffset = PageGetMaxOffsetNumber(page);
	OffsetNumber offset;
	unsigned int headerBytes;

	headerBytes = offsetof(PageHeaderData, pd_linp[0]);

	/*
	 * It's either a non-meta index page, or a heap page.  Create tags for all
	 * ItemId entries/line pointers on page.
	 */
	for (offset = FirstOffsetNumber;
		 offset <= maxOffset;
		 offset = OffsetNumberNext(offset))
	{
		ItemId		itemId;
		unsigned int itemFlags;
		char		textFlags[16];

		itemId = PageGetItemId(page, offset);

		itemFlags = (unsigned int) ItemIdGetFlags(itemId);

		switch (itemFlags)
		{
			case LP_UNUSED:
				strcpy(textFlags, "LP_UNUSED");
				break;
			case LP_NORMAL:
				strcpy(textFlags, "LP_NORMAL");
				break;
			case LP_REDIRECT:
				strcpy(textFlags, "LP_REDIRECT");
				break;
			case LP_DEAD:
				strcpy(textFlags, "LP_DEAD");
				break;
			default:
				sprintf(textFlags, "0x%02x", itemFlags);
				fprintf(stderr, "pg_hexedit error: invalid line pointer flags for (%u,%u): %s\n",
						blkno + segmentBlockDelta, offset, textFlags);
				exitCode = 1;
				break;
		}

		EmitXmlItemId(blkno, offset, itemId,
					  pageOffset + headerBytes + (sizeof(ItemIdData) * (offset - 1)),
					  textFlags);
	}
}

/*
 * Emit formatted tuples that reside on this block.
 *
 * This is responsible for emitting tuples from all pages that use an ItemId
 * array.  This includes heapam, sequences, B-Tree, GiST, hash, regular BRIN
 * pages, and GIN pages for the main B-Tree over key values (not data/posting
 * tree pages).  It's also responsible for pending list GIN pages, which are
 * similar to GIN pages for the main B-Tree.
 */
static void
EmitXmlTuples(Page page, BlockNumber blkno)
{
	OffsetNumber offset;
	int			itemSize;
	int			itemOffset;
	unsigned int itemFlags;
	ItemId		itemId;
	int			formatAs;
	int			maxOffset = PageGetMaxOffsetNumber(page);

	/* Loop through the items on the block */
	if (maxOffset == 0)
		return;
	else if ((maxOffset < 0) || (maxOffset > blockSize))
	{
		fprintf(stderr, "pg_hexedit error: corrupt PageGetMaxOffsetNumber() offset %d found on file block %u\n",
				maxOffset, blkno);
		exitCode = 1;
		return;
	}

	/* Use the special section to determine the format style */
	switch (specialType)
	{
		case SPEC_SECT_NONE:
		case SPEC_SECT_SEQUENCE:
			formatAs = ITEM_HEAP;
			break;
		case SPEC_SECT_INDEX_BTREE:
		case SPEC_SECT_INDEX_HASH:
		case SPEC_SECT_INDEX_GIST:
		case SPEC_SECT_INDEX_GIN:
			formatAs = ITEM_INDEX;
			break;
		case SPEC_SECT_INDEX_SPGIST:
			if (!SpGistPageIsLeaf(page))
				formatAs = ITEM_SPG_INN;
			else
				formatAs = ITEM_SPG_LEAF;
			break;
		case SPEC_SECT_INDEX_BRIN:
			formatAs = ITEM_BRIN;
			break;
		default:
			/* Only complain the first time an error like this is seen */
			if (exitCode == 0)
				fprintf(stderr, "pg_hexedit error: unsupported special section type \"%s\"\n",
						GetSpecialSectionString(specialType));
			formatAs = ITEM_INDEX;
			exitCode = 1;
	}

	for (offset = FirstOffsetNumber;
		 offset <= maxOffset;
		 offset = OffsetNumberNext(offset))
	{
		itemId = PageGetItemId(page, offset);
		itemSize = (int) ItemIdGetLength(itemId);
		itemOffset = (int) ItemIdGetOffset(itemId);
		itemFlags = (unsigned int) ItemIdGetFlags(itemId);

		/* LD_DEAD items may have storage, so we go by lp_len alone */
		if (itemSize == 0)
		{
			if (itemFlags == LP_NORMAL)
			{
				fprintf(stderr, "pg_hexedit error: (%u,%u) LP_NORMAL item has lp_len 0\n",
						blkno + segmentBlockDelta, offset);
				exitCode = 1;
			}
			continue;
		}
		/* Sanitize */
		if (itemFlags == LP_REDIRECT || itemFlags == LP_UNUSED)
		{
			fprintf(stderr, "pg_hexedit error: (%u,%u) LP_REDIRECT or LP_UNUSED item has lp_len %u\n",
					blkno + segmentBlockDelta, offset, itemSize);
			exitCode = 1;
			continue;
		}

		/*
		 * Make sure the item can physically fit on this block before
		 * formatting
		 */
		if (itemOffset + itemSize > blockSize ||
			itemOffset + itemSize > bytesToFormat)
		{
			fprintf(stderr, "pg_hexedit error: (%u,%u) item contents extend beyond block.\n"
					"blocksize %d bytes, read %d bytes, item start offset %d.\n",
					blkno + segmentBlockDelta, offset, blockSize,
					bytesToFormat, itemOffset + itemSize);
			exitCode = 1;
			continue;
		}

		if (formatAs == ITEM_HEAP)
		{
			HeapTupleHeader htup;

			htup = (HeapTupleHeader) PageGetItem(page, itemId);

			EmitXmlHeapTuple(blkno, offset, htup,
							 pageOffset + itemOffset, itemSize);
		}
		else if (formatAs == ITEM_INDEX)
		{
			IndexTuple	tuple;
			bool		dead;

			tuple = (IndexTuple) PageGetItem(page, itemId);
			dead = ItemIdIsDead(itemId);

			EmitXmlIndexTuple(page, blkno, offset, tuple,
							  pageOffset + itemOffset, itemSize, dead);
		}
		else if (formatAs == ITEM_SPG_INN)
		{
			SpGistInnerTuple tuple;

			tuple = (SpGistInnerTuple) PageGetItem(page, itemId);

			EmitXmlSpGistInnerTuple(page, blkno, offset, tuple,
									pageOffset + itemOffset);
		}
		else if (formatAs == ITEM_SPG_LEAF)
		{
			SpGistLeafTuple tuple;

			tuple = (SpGistLeafTuple) PageGetItem(page, itemId);

			EmitXmlSpGistLeafTuple(page, blkno, offset, tuple,
								   pageOffset + itemOffset);
		}
		else if (formatAs == ITEM_BRIN)
		{
			BrinTuple  *tuple;

			tuple = (BrinTuple *) PageGetItem(page, itemId);

			EmitXmlBrinTuple(page, blkno, offset, tuple,
							 pageOffset + itemOffset, itemSize);
		}
	}
}

/*
 * Emit posting tree page pseudo-tuples.
 *
 * Posting tree pages don't store regular tuples. Non-leaf pages contain
 * PostingItems, which are pairs of ItemPointers and child block numbers.  Leaf
 * pages contain GinPostingLists and an uncompressed array of item pointers.
 *
 * In a leaf page, the compressed posting lists are stored after the regular
 * page header, one after each other.  Although GIN does not store regular
 * tuples, pd_lower is used to indicate the end of the posting lists. After
 * that, free space follows.
 */
static void
EmitXmlPostingTreeTids(Page page, BlockNumber blkno)
{
	OffsetNumber offsetnum;
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	unsigned int itemOffset;
	unsigned int itemOffsetNext;

	Assert(GinPageIsData(page));

	if (!GinPageIsLeaf(page))
	{
		itemOffset = GinDataPageGetData(page) - page;

		for (offsetnum = FirstOffsetNumber;
			 offsetnum <= maxoff;
			 offsetnum = OffsetNumberNext(offsetnum))
		{
			EmitXmlTupleTag(blkno, offsetnum, "PostingItem->child_blkno->bi_hi", COLOR_BLUE_LIGHT,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offsetnum, "PostingItem->child_blkno->bi_lo", COLOR_BLUE_LIGHT,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);

			/*
			 * These TIDs are white because within this page (an internal
			 * posting tree page) they are keys, not pointers.  This is
			 * similar to nbtree internal pages (pivot tuples).
			 */
			EmitXmlTupleTag(blkno, offsetnum, "PostingItem->key->bi_hi", COLOR_WHITE,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offsetnum, "PostingItem->key->bi_lo", COLOR_WHITE,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offsetnum, "PostingItem->key->offsetNumber", COLOR_WHITE,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
		}
	}
	else
	{
		GinPostingList *seg,
				   *nextseg;
		Pointer		endptr;

		/*
		 * See description of posting page/data page format at top of
		 * ginpostlinglist.c for more information.
		 *
		 * We don't emit anything for pre-9.4 uncompressed data pages
		 * versions, since those versions are unsupported by pg_hexedit.  They
		 * could still be encountered if the database underwent pg_upgrade,
		 * but that should be rare.
		 */
		if (!GinPageIsCompressed(page))
			return;

		itemOffset = GinDataPageGetData(page) - page;
		offsetnum = FirstOffsetNumber;
		seg = GinDataLeafPageGetPostingList(page);
		nextseg = GinNextPostingListSegment(seg);
		itemOffsetNext = itemOffset + ((Pointer) nextseg - (Pointer) seg);

		endptr = ((Pointer) seg) + GinDataLeafPageGetPostingListSize(page);
		do
		{
			EmitXmlTupleTag(blkno, offsetnum, "GinPostingList->first->bi_hi", COLOR_BLUE_LIGHT,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offsetnum, "GinPostingList->first->bi_lo", COLOR_BLUE_LIGHT,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
			EmitXmlTupleTag(blkno, offsetnum, "GinPostingList->first->offsetNumber", COLOR_BLUE_DARK,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);

			/* Make nbytes dark yellow, to match similar IndexTuple metadata */
			EmitXmlTupleTag(blkno, offsetnum, "GinPostingList->nbytes", COLOR_YELLOW_DARK,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + sizeof(uint16)) - 1);
			itemOffset += sizeof(uint16);
			/* Compressed TIDs are orange */
			EmitXmlTupleTag(blkno, offsetnum, "varbyte encoded TIDs", COLOR_ORANGE,
							pageOffset + itemOffset,
							(pageOffset + itemOffset + seg->nbytes) - 1);
			itemOffset = itemOffsetNext;
			seg = nextseg;
			nextseg = GinNextPostingListSegment(seg);
			itemOffsetNext = itemOffset + ((Pointer) nextseg - (Pointer) seg);
			offsetnum = OffsetNumberNext(offsetnum);
		}
		while ((Pointer) nextseg <= endptr);
	}
}

/*
 * Emit hash bitmap page.
 *
 * This is just a matter of emitting a single tag for everything after the page
 * header, but before pd_lower.
 */
static void
EmitXmlHashBitmap(Page page, BlockNumber blkno)
{
	uint32		relfileOff = pageOffset + (PageGetContents(page) - page);
	uint32		relfileOffNext = pageOffset + ((PageHeader) (page))->pd_lower;

	EmitXmlTag(blkno, UINT_MAX, "hash bitmap", COLOR_YELLOW_DARK, relfileOff,
			   relfileOffNext - 1);
}

/*
 * Emit BRIN revmap TIDs as pseudo-tuples.
 *
 * BRIN revmap pages don't store an ItemId array, or regular tuples.  Instead,
 * the page contains an array of straight TIDs.
 *
 * We don't look at pd_upper, in keeping with pageinspect's brin_revmap_data(),
 * which also always emits REVMAP_PAGE_MAXITEMS entries.
 */
static void
EmitXmlRevmap(Page page, BlockNumber blkno)
{
	OffsetNumber offsetnum;
	uint32		relfileOff = pageOffset + (PageGetContents(page) - page);
	uint32		relfileOffNext;

	for (offsetnum = FirstOffsetNumber;
		 offsetnum <= REVMAP_PAGE_MAXITEMS;
		 offsetnum = OffsetNumberNext(offsetnum))
	{
		/*
		 * Emit t_tid tags.  TID tag style should be kept consistent with
		 * EmitXmlHeapTuple().
		 *
		 * Note: We use pseudo offset numbers here.  This is a somewhat
		 * subjective interpretation of revmap page contents.  We want to
		 * impose some conventions, even if they aren't particularly well
		 * justified by struct definitions.
		 */
		relfileOffNext = relfileOff + sizeof(uint16);
		EmitXmlTupleTag(blkno, offsetnum, "rm_tids[i]->bi_hi", COLOR_BLUE_LIGHT, relfileOff,
						relfileOffNext - 1);
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(uint16);
		EmitXmlTupleTag(blkno, offsetnum, "rm_tids[i]->bi_lo", COLOR_BLUE_LIGHT, relfileOff,
						relfileOffNext - 1);
		relfileOff = relfileOffNext;
		relfileOffNext += sizeof(uint16);
		EmitXmlTupleTag(blkno, offsetnum, "rm_tids[i]->offsetNumber", COLOR_BLUE_DARK,
						relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
	}
}

/*
 * On blocks that have special sections, print the contents according to
 * previously determined special section type.
 *
 * Color of special section fields should be COLOR_GREEN_BRIGHT, to match
 * pd_special field in page header.
 */
static void
EmitXmlSpecial(BlockNumber blkno, uint32 level)
{
	PageHeader	pageHeader = (PageHeader) buffer;
	unsigned int specialOffset = pageHeader->pd_special;
	char	   *flagString;

	flagString = pg_malloc(256);
	flagString[0] = '\0';

	switch (specialType)
	{
		case SPEC_SECT_NONE:
		case SPEC_SECT_ERROR_UNKNOWN:
		case SPEC_SECT_ERROR_BOUNDARY:
			fprintf(stderr, "pg_hexedit error: invalid special section type \"%s\"\n",
					GetSpecialSectionString(specialType));
			exitCode = 1;
			break;

		case SPEC_SECT_SEQUENCE:
			{
				/* Special area consists of a single uint32, "magic" */
				EmitXmlTag(blkno, level, "magic", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset,
						   (pageOffset + specialOffset + sizeof(uint32) - 1));
			}
			break;

		case SPEC_SECT_INDEX_BTREE:
			{
				BTPageOpaque btreeSection = (BTPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "btpo_prev", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_prev),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_next)) - 1);
				EmitXmlTag(blkno, level, "btpo_next", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_next),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo)) - 1);
				EmitXmlTag(blkno, level, "btpo.level", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_flags)) - 1);

				/* Generate B-Tree special area flags */
				strcat(flagString, "btpo_flags - ");
				if (btreeSection->btpo_flags & BTP_LEAF)
					strcat(flagString, "BTP_LEAF|");
				if (btreeSection->btpo_flags & BTP_ROOT)
					strcat(flagString, "BTP_ROOT|");
				if (btreeSection->btpo_flags & BTP_DELETED)
					strcat(flagString, "BTP_DELETED|");
				if (btreeSection->btpo_flags & BTP_META)
					strcat(flagString, "BTP_META|");
				if (btreeSection->btpo_flags & BTP_HALF_DEAD)
					strcat(flagString, "BTP_HALF_DEAD|");
				if (btreeSection->btpo_flags & BTP_SPLIT_END)
					strcat(flagString, "BTP_SPLIT_END|");
				if (btreeSection->btpo_flags & BTP_HAS_GARBAGE)
					strcat(flagString, "BTP_HAS_GARBAGE|");
				if (btreeSection->btpo_flags & BTP_INCOMPLETE_SPLIT)
					strcat(flagString, "BTP_INCOMPLETE_SPLIT|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_flags),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid)) - 1);
				EmitXmlTag(blkno, level, "btpo_cycleid", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid),
						   (pageOffset + specialOffset + sizeof(BTPageOpaqueData) - 1));
			}
			break;

		case SPEC_SECT_INDEX_HASH:
			{
				HashPageOpaque hashSection = (HashPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "hasho_prevblkno", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_prevblkno),
						   (pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_nextblkno)) - 1);
				EmitXmlTag(blkno, level, "hasho_nextblkno", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_nextblkno),
						   (pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_bucket)) - 1);
				EmitXmlTag(blkno, level, "hasho_bucket", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_bucket),
						   (pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_flag)) - 1);

				/* Generate hash special area flags */
				strcat(flagString, "hasho_flag - ");
				if (hashSection->hasho_flag & LH_OVERFLOW_PAGE)
					strcat(flagString, "LH_OVERFLOW_PAGE|");
				if (hashSection->hasho_flag & LH_BUCKET_PAGE)
					strcat(flagString, "LH_BUCKET_PAGE|");
				if (hashSection->hasho_flag & LH_BITMAP_PAGE)
					strcat(flagString, "LH_BITMAP_PAGE|");
				if (hashSection->hasho_flag & LH_META_PAGE)
					strcat(flagString, "LH_META_PAGE|");
#if PG_VERSION_NUM >= 100000
				if (hashSection->hasho_flag & LH_BUCKET_BEING_POPULATED)
					strcat(flagString, "LH_BUCKET_BEING_POPULATED|");
				if (hashSection->hasho_flag & LH_BUCKET_BEING_SPLIT)
					strcat(flagString, "LH_BUCKET_BEING_SPLIT|");
				if (hashSection->hasho_flag & LH_BUCKET_NEEDS_SPLIT_CLEANUP)
					strcat(flagString, "LH_BUCKET_NEEDS_SPLIT_CLEANUP|");
				if (hashSection->hasho_flag & LH_PAGE_HAS_DEAD_TUPLES)
					strcat(flagString, "LH_PAGE_HAS_DEAD_TUPLES|");
#endif
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_flag),
						   (pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_page_id)) - 1);
				EmitXmlTag(blkno, level, "hasho_page_id", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(HashPageOpaqueData, hasho_page_id),
						   (pageOffset + specialOffset + sizeof(HashPageOpaqueData) - 1));
			}
			break;

		case SPEC_SECT_INDEX_GIST:
			{
				GISTPageOpaque gistSection = (GISTPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "nsn", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GISTPageOpaqueData, nsn),
						   (pageOffset + specialOffset + offsetof(GISTPageOpaqueData, rightlink)) - 1);
				EmitXmlTag(blkno, level, "rightlink", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GISTPageOpaqueData, rightlink),
						   (pageOffset + specialOffset + offsetof(GISTPageOpaqueData, flags)) - 1);

				/* Generate GiST special area flags */
				strcat(flagString, "flags - ");
				if (gistSection->flags & F_LEAF)
					strcat(flagString, "F_LEAF|");
				if (gistSection->flags & F_DELETED)
					strcat(flagString, "F_DELETED|");
				if (gistSection->flags & F_TUPLES_DELETED)
					strcat(flagString, "F_TUPLES_DELETED|");
				if (gistSection->flags & F_FOLLOW_RIGHT)
					strcat(flagString, "F_FOLLOW_RIGHT|");
#if PG_VERSION_NUM >= 90600
				if (gistSection->flags & F_HAS_GARBAGE)
					strcat(flagString, "F_HAS_GARBAGE|");
#endif
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GISTPageOpaqueData, flags),
						   (pageOffset + specialOffset + offsetof(GISTPageOpaqueData, gist_page_id)) - 1);
				EmitXmlTag(blkno, level, "gist_page_id", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GISTPageOpaqueData, gist_page_id),
						   (pageOffset + specialOffset + sizeof(GISTPageOpaqueData) - 1));
			}
			break;

		case SPEC_SECT_INDEX_GIN:
			{
				GinPageOpaque ginSection = (GinPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "rightlink", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GinPageOpaqueData, rightlink),
						   (pageOffset + specialOffset + offsetof(GinPageOpaqueData, maxoff)) - 1);
				EmitXmlTag(blkno, level, "maxoff", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GinPageOpaqueData, maxoff),
						   (pageOffset + specialOffset + offsetof(GinPageOpaqueData, flags)) - 1);

				/* Generate GIN special area flags */
				strcat(flagString, "flags - ");
				if (ginSection->flags & GIN_DATA)
					strcat(flagString, "GIN_DATA|");
				if (ginSection->flags & GIN_LEAF)
					strcat(flagString, "GIN_LEAF|");
				if (ginSection->flags & GIN_DELETED)
					strcat(flagString, "GIN_DELETED|");
				if (ginSection->flags & GIN_META)
					strcat(flagString, "GIN_META|");
				if (ginSection->flags & GIN_LIST)
					strcat(flagString, "GIN_LIST|");
				if (ginSection->flags & GIN_LIST_FULLROW)
					strcat(flagString, "GIN_LIST_FULLROW|");
				if (ginSection->flags & GIN_INCOMPLETE_SPLIT)
					strcat(flagString, "GIN_INCOMPLETE_SPLIT|");
				if (ginSection->flags & GIN_COMPRESSED)
					strcat(flagString, "GIN_COMPRESSED|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(GinPageOpaqueData, flags),
						   (pageOffset + specialOffset + sizeof(GinPageOpaqueData) - 1));
			}
			break;

		case SPEC_SECT_INDEX_SPGIST:
			{
				SpGistPageOpaque spGistSection = (SpGistPageOpaque) (buffer + specialOffset);

				/* Generate SP-GiST special area flags */
				strcat(flagString, "flags - ");
				if (spGistSection->flags & SPGIST_META)
					strcat(flagString, "SPGIST_META|");
				if (spGistSection->flags & SPGIST_DELETED)
					strcat(flagString, "SPGIST_DELETED|");
				if (spGistSection->flags & SPGIST_LEAF)
					strcat(flagString, "SPGIST_LEAF|");
				if (spGistSection->flags & SPGIST_NULLS)
					strcat(flagString, "SPGIST_NULLS|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, flags),
						   (pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, nRedirection)) - 1);

				EmitXmlTag(blkno, level, "nRedirection", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, nRedirection),
						   (pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, nPlaceholder)) - 1);
				EmitXmlTag(blkno, level, "nPlaceholder", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, nPlaceholder),
						   (pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, spgist_page_id)) - 1);
				EmitXmlTag(blkno, level, "spgist_page_id", COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(SpGistPageOpaqueData, spgist_page_id),
						   (pageOffset + specialOffset + sizeof(SpGistPageOpaqueData) - 1));
			}
			break;

		case SPEC_SECT_INDEX_BRIN:
			{
				/*
				 * Details of array subscription are taken from
				 * BrinPageFlags() and BringPageTupe() macros
				 */
				BrinSpecialSpace *brinSection = (BrinSpecialSpace *) (buffer + specialOffset);

				/* Flags, of which there is currently only one, come first. */
				strcat(flagString, "BrinPageFlags() - ");
				if (brinSection->vector[MAXALIGN(1) / sizeof(uint16) - 2] & BRIN_EVACUATE_PAGE)
					strcat(flagString, "BRIN_EVACUATE_PAGE|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';
				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BrinSpecialSpace, vector[MAXALIGN(1) / sizeof(uint16) - 2]),
						   (pageOffset + specialOffset + offsetof(BrinSpecialSpace, vector[MAXALIGN(1) / sizeof(uint16) - 1])) - 1);

				/* Generate BRIN special page type */
				flagString[0] = '\0';
				strcat(flagString, "BrinPageType() - ");
				if (brinSection->vector[MAXALIGN(1) / sizeof(uint16) - 1] == BRIN_PAGETYPE_META)
					strcat(flagString, "BRIN_PAGETYPE_META|");
				else if (brinSection->vector[MAXALIGN(1) / sizeof(uint16) - 1] == BRIN_PAGETYPE_REVMAP)
					strcat(flagString, "BRIN_PAGETYPE_REVMAP|");
				else if (brinSection->vector[MAXALIGN(1) / sizeof(uint16) - 1] == BRIN_PAGETYPE_REGULAR)
					strcat(flagString, "BRIN_PAGETYPE_REGULAR|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				EmitXmlTag(blkno, level, flagString, COLOR_GREEN_BRIGHT,
						   pageOffset + specialOffset + offsetof(BrinSpecialSpace, vector[MAXALIGN(1) / sizeof(uint16) - 1]),
						   (pageOffset + specialOffset + sizeof(BrinSpecialSpace) - 1));
			}
			break;

		default:
			/* Only complain the first time an error like this is seen */
			if (exitCode == 0)
				fprintf(stderr, "pg_hexedit error: unsupported special section type \"%s\"\n",
						GetSpecialSectionString(specialType));
			exitCode = 1;
	}

	pg_free(flagString);
}

/*
 * Dump the main body of XML tags (does not include header, header comments, or
 * footer.)
 */
static void
EmitXmlBody(void)
{
	unsigned int initialRead = 1;
	unsigned int contentsToDump = 1;

	/*
	 * Calculate an offset in blocks to the segment file, from the start of
	 * the logical relation (or from the start of segment 0, if you prefer).
	 * This is needed so that annotations and error messages do not emit
	 * file-relative block numbers within TIDs.
	 *
	 * Relation-relative block numbers should always be used in annotations,
	 * including when a raw block number is required, but should only be used
	 * for TIDs in error messages.  If an error message references a block
	 * number, then it is naturally file-relative; otherwise, a TID would have
	 * been used.  The distinction between relation-relative and file-relative
	 * block numbers is not just an implementation detail, since input options
	 * like BLOCK_RANGE are always in terms of file-relative block numbers.
	 */
	segmentBlockDelta = (segmentSize / blockSize) * segmentNumber;

	/*
	 * If the user requested a block range, seek to the correct position
	 * within the file for the start block.
	 */
	if (blockOptions & BLOCK_RANGE)
	{
		unsigned int position = blockSize * blockStart;

		if (fseek(fp, position, SEEK_SET) != 0)
		{
			fprintf(stderr, "pg_hexedit error: seek error encountered before requested start block %d\n",
					blockStart);
			contentsToDump = 0;
			exitCode = 1;
		}
		else
			currentBlock = blockStart;
	}

	/*
	 * Iterate through the blocks in the file until you reach the end or the
	 * requested range end
	 */
	while (contentsToDump)
	{
		bytesToFormat = fread(buffer, 1, blockSize, fp);

		if (bytesToFormat == 0)
		{
			/*
			 * fseek() won't pop an error if you seek passed eof.  The next
			 * subsequent read gets the error.
			 */
			if (initialRead)
			{
				fprintf(stderr, "pg_hexedit error: premature end of file encountered\n");
				exitCode = 1;
			}
			contentsToDump = 0;
		}
		else
			EmitXmlPage(currentBlock);

		/* Check to see if we are at the end of the requested range. */
		if ((blockOptions & BLOCK_RANGE) &&
			(currentBlock >= blockEnd) && (contentsToDump))
		{
			contentsToDump = 0;
		}
		else
			currentBlock++;

		initialRead = 0;
	}
}

/*
 * Consume the options and iterate through the given file, formatting as
 * requested.
 */
int
main(int argv, char **argc)
{
	/* If there is a parameter list, validate the options */
	unsigned int validOptions;

	validOptions = (argv < 2) ? OPT_RC_COPYRIGHT : ConsumeOptions(argv, argc);

	/*
	 * Display valid options if no parameters are received or invalid options
	 * where encountered
	 */
	if (validOptions != OPT_RC_VALID)
		DisplayOptions(validOptions);
	else
	{
		blockSize = GetBlockSize();

		/*
		 * On a positive block size, allocate a local buffer to store the
		 * subsequent blocks, and generate main body of XML tags.
		 */
		EmitXmlDocHeader(argv, argc);
		if (blockSize > 0)
		{
			buffer = (char *) pg_malloc(blockSize);
			EmitXmlBody();
		}
		EmitXmlFooter();
	}

	/* Close out the file and get rid of the allocated block buffer */
	if (fp)
		fclose(fp);

	if (buffer)
		pg_free(buffer);

	exit(exitCode);
}

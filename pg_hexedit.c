/*
 * pg_hexedit.c - PostgreSQL file dump utility for
 *                viewing heap (data) and index files in wxHexEditor.
 *
 * Copyright (c) 2017, VMware, Inc.
 * Copyright (c) 2002-2010 Red Hat, Inc.
 * Copyright (c) 2011-2016, PostgreSQL Global Development Group
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

/*
 * We must #undef frontend because certain headers are not really supposed to
 * be included in frontend utilities because they include atomics.h.
 */
#undef FRONTEND

#include <time.h>

#if PG_VERSION_NUM >= 90500
#include "access/brin_page.h"
#endif
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

#define SEQUENCE_MAGIC			0x1717	/* PostgreSQL defined magic number */
#define EOF_ENCOUNTERED 		(-1)	/* Indicator for partial read */

#define COLOR_FONT_STANDARD		"#313739"

#define COLOR_BLACK				"#515A5A"
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
	BLOCK_SKIP_LEAF = 0x00000080,	/* -l: Skip leaf pages (use whole page tag) */
	BLOCK_SKIP_LSN = 0x00000010		/* -x: Skip pages before LSN */
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

/* Program exit code */
static int	exitCode = 0;

typedef enum formatChoice
{
	ITEM_HEAP = 0x00000001,		/* Blocks contain HeapTuple items */
	ITEM_INDEX = 0x00000002		/* Blocks contain IndexTuple items */
} formatChoice;

static void DisplayOptions(unsigned int validOptions);
static unsigned int GetSegmentNumberFromFileName(const char *fileName);
static unsigned int ConsumeOptions(int numOptions, char **options);
static int GetOptionValue(char *optionString);
static XLogRecPtr GetOptionXlogRecPtr(char *optionString);
static unsigned int GetBlockSize(void);
static unsigned int GetSpecialSectionType(Page page);
static const char *GetSpecialSectionString(unsigned int type);
static XLogRecPtr GetPageLsn(Page page);
static char *GetHeapTupleHeaderFlags(HeapTupleHeader htup, bool isInfomask2);
static char *GetIndexTupleFlags(IndexTuple itup);
static bool IsBrinMetaPage(Page page);
static void EmitXmlPage(BlockNumber blkno);
static void EmitXmlDocHeader(int numOptions, char **options);
static void EmitXmlFooter(void);
static void EmitXmlTag(BlockNumber blkno, uint32 level, const char *name,
					   const char *color, uint32 relfileOff,
					   uint32 relfileOffEnd);
static void EmitXmlItemId(BlockNumber blkno, OffsetNumber offset,
						  ItemId itemId, uint32 relfileOff,
						  const char *textFlags);
static void EmitXmlTupleTag(BlockNumber blkno, OffsetNumber offset,
							const char *name, const char *color,
							uint32 relfileOff,
							uint32 relfileOffEnd);
static void EmitXmlHeapTuple(BlockNumber blkno, OffsetNumber offset,
							 HeapTupleHeader htup, uint32 relfileOff,
							 unsigned int itemSize);
static void EmitXmlIndexTuple(BlockNumber blkno, OffsetNumber offset,
							  IndexTuple tuple, uint32 relfileOff);
static int EmitXmlPageHeader(Page page, BlockNumber blkno, uint32 level);
static void EmitXmlPageMeta(BlockNumber blkno, uint32 level);
static void EmitXmlPageItemIdArray(Page page, BlockNumber blkno);
static void EmitXmlTuples(Page page, BlockNumber blkno);
static void EmitXmlPostingTreeTids(Page page, BlockNumber blkno);
static void EmitXmlSpecial(BlockNumber blkno, uint32 level);
static void EmitXmlFile(void);


/*	Send properly formed usage information to the user. */
static void
DisplayOptions(unsigned int validOptions)
{
	if (validOptions == OPT_RC_COPYRIGHT)
		printf
			("\npg_hexedit (for %s)"
			 "\nCopyright (c) 2017, VMware, Inc."
			 "\nCopyright (c) 2002-2010 Red Hat, Inc."
			 "\nCopyright (c) 2011-2016, PostgreSQL Global Development Group\n",
			 PG_VERSION);

	printf
		("\nUsage: pg_hexedit [-hkl] [-R startblock [endblock]] [-s segsize] [-n segnumber] file\n\n"
		 "Display formatted contents of a PostgreSQL heap/index/control file\n"
		 "Defaults are: relative addressing, range of the entire file, block\n"
		 "               size as listed on block 0 in the file\n\n"
		 "The following options are valid for heap and index files:\n"
		 "  -h  Display this information\n"
		 "  -k  Verify block checksums\n"
		 "  -l  Skip non-root B-Tree leaf pages\n"
		 "  -x  Skip pages whose LSN is before [lsn]\n"
		 "  -R  Display specific block ranges within the file (Blocks are\n"
		 "      indexed from 0)\n" "        [startblock]: block to start at\n"
		 "        [endblock]: block to end at\n"
		 "      A startblock without an endblock will format the single block\n"
		 "  -s  Force segment size to [segsize]\n"
		 "  -n  Force segment number to [segnumber]\n"
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
				fprintf(stderr, "pg_hexedit error: missing range start identifier.\n");
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
				fprintf(stderr,
						"pg_hexedit error: invalid range start identifier \"%s\".\n",
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
						fprintf(stderr, "pg_hexedit error: requested block range start %d is "
							   "greater than end %d.\n", blockStart, range);
						exitCode = 1;
						break;
					}
				}
			}
		}

		/*
		 * Check for the special case where the user only requires tags for
		 * pages whose LSN equals or exceeds a supplied threshold.
		 */
		else if ((optionStringLength == 2) && (strcmp(optionString, "-x") == 0))
		{
			SET_OPTION(blockOptions, BLOCK_SKIP_LSN , 'x');
			/* Only accept the LSN option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* Make sure that there is an LSN option */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				fprintf(stderr, "pg_hexedit error: missing lsn.\n");
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
				fprintf(stderr, "pg_hexedit error: invalid lsn identifier \"%s\".\n",
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
				fprintf(stderr, "pg_hexedit error: missing segment size identifier.\n");
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
				fprintf(stderr, "pg_hexedit error: invalid segment size requested \"%s\".\n",
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
				fprintf(stderr, "pg_hexedit error: missing segment number identifier.\n");
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
				fprintf(stderr, "pg_hexedit error: invalid segment number requested \"%s\".\n",
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
					fprintf(stderr, "pg_hexedit error: could not open file \"%s\".\n", optionString);
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
					fprintf(stderr, "pg_hexedit error: missing file name to dump.\n");
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
				fprintf(stderr, "pg_hexedit error: invalid option string \"%s\".\n", optionString);
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

						/* Verify block checksums */
					case 'k':
						SET_OPTION(blockOptions, BLOCK_CHECKSUMS, 'k');
						break;

						/* Skip non-root leaf pages */
					case 'l':
						SET_OPTION(blockOptions, BLOCK_SKIP_LEAF, 'l');
						break;

					default:
						rc = OPT_RC_INVALID;
						fprintf(stderr, "pg_hexedit error: unknown option '%c'.\n", optionString[y]);
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
		fprintf(stderr, "pg_hexedit error: duplicate option listed '%c'.\n", duplicateSwitch);
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
 * Read the page header off of block 0 to determine the block size used in this
 * file.  Can be overridden using the -S option.  The returned value is the
 * block size of block 0 on disk.
 */
static unsigned int
GetBlockSize(void)
{
	unsigned int pageHeaderSize = sizeof(PageHeaderData);
	unsigned int localSize = 0;
	int			bytesRead = 0;
	char		localCache[pageHeaderSize];

	/* Read the first header off of block 0 to determine the block size */
	bytesRead = fread(&localCache, 1, pageHeaderSize, fp);
	rewind(fp);

	if (bytesRead == pageHeaderSize)
		localSize = (unsigned int) PageGetPageSize(&localCache);
	else
	{
		fprintf(stderr, "pg_hexedit error: unable to read full page header from block 0\n"
			   "read %u bytes\n", bytesRead);
		exitCode = 1;
	}

	return (localSize);
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
							 IsBrinMetaPage(page))
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
 * Note:  Caller is responsible for free()ing returned buffer.
 */
static char *
GetHeapTupleHeaderFlags(HeapTupleHeader htup, bool isInfomask2)
{
	unsigned int	bitmapLength = 0;
	unsigned int	oidLength = 0;
	unsigned int	computedLength;
	unsigned int	localHoff;
	unsigned int	localBitOffset;
	char		   *flagString = NULL;

	flagString = malloc(512);

	if (!flagString)
	{
		fprintf(stderr, "pg_hexedit error: unable to create buffer of size 512.\n");
		/* Call exit() immediately, so caller doesn't have to handle failure */
		exit(1);
	}

	localHoff = htup->t_hoff;
	localBitOffset = offsetof(HeapTupleHeaderData, t_bits);

	/*
	 * Place readable versions of the tuple info mask into a buffer.
	 * Assume that the string can not expand beyond 512 bytes.
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
 * Note:  Caller is responsible for free()ing returned buffer.
 */
static char *
GetIndexTupleFlags(IndexTuple itup)
{
	char		   *flagString = NULL;

	flagString = malloc(128);

	if (!flagString)
	{
		fprintf(stderr, "pg_hexedit error: unable to create buffer of size 128.\n");
		/* Call exit() immediately, so caller doesn't have to handle failure */
		exit(1);
	}

	/*
	 * Place readable versions of the tuple info mask into a buffer.  Assume
	 * that the string can not expand beyond 128 bytes.
	 */
	flagString[0] = '\0';
	sprintf(flagString, "t_info IndexTupleSize(): %zu",
			IndexTupleSize(itup));

	if (itup->t_info & (INDEX_VAR_MASK | INDEX_NULL_MASK))
		strcat(flagString, ", (");

	if (itup->t_info & INDEX_VAR_MASK)
		strcat(flagString, "INDEX_VAR_MASK|");
	if (itup->t_info & INDEX_NULL_MASK)
		strcat(flagString, "INDEX_NULL_MASK|");

	if (itup->t_info & (INDEX_VAR_MASK | INDEX_NULL_MASK))
	{
		flagString[strlen(flagString) - 1] = '\0';
		strcat(flagString, ")");
	}

	return flagString;
}

/*	Check whether page is a BRIN meta page */
static bool
IsBrinMetaPage(Page page)
{
#if PG_VERSION_NUM >= 90500
	 BrinMetaPageData   *meta;

	if (bytesToFormat != blockSize || !BRIN_IS_META_PAGE(page))
		return false;

	 meta = ((BrinMetaPageData *) PageGetContents(page));

	 if (meta->brinMagic == BRIN_META_MAGIC)
		 return true;
#endif
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

	pageOffset = blockSize * currentBlock;

	/*
	 * Check to see if we must skip this block due to it falling behind
	 * LSN threshold.
	 */
	if ((blockOptions & BLOCK_SKIP_LSN))
	{
		XLogRecPtr pageLSN = GetPageLsn(page);

		if (pageLSN < afterThreshold)
		{
			rc = 0;
			return;
		}
	}

	specialType = GetSpecialSectionType(page);

	/*
	 * We optionally itemize leaf blocks as whole tags, in order to limit the
	 * size of tag files sharply (nbtree only).  Internal pages can be more
	 * interesting when debugging certain types of problems.
	 */
	if (specialType == SPEC_SECT_INDEX_BTREE)
	{
		BTPageOpaque btreeSection = (BTPageOpaque) PageGetSpecialPointer(page);

		/* Only B-Tree tags get a "level" */
		level = btreeSection->btpo.level;

		/*
		 * Always display the root page when it happens to be a leaf (i.e.  the
		 * root before the first root page split)
		 */
		if ((btreeSection->btpo_flags & BTP_LEAF) &&
			!(btreeSection->btpo_flags & BTP_ROOT) &&
			(blockOptions & BLOCK_SKIP_LEAF))
		{
			EmitXmlTag(blkno, level, "leaf page", COLOR_GREEN_DARK,
					   pageOffset,
					   (pageOffset + BLCKSZ) - 1);
			rc = 0;
			return;
		}
	}

	/*
	 * Every block that we aren't skipping will have header, items and possibly
	 * special section tags created.  Beware of partial block reads, though.
	 */
	rc = EmitXmlPageHeader(page, blkno, level);

	/* If we didn't encounter a partial read in header, carry on...  */
	if (rc != EOF_ENCOUNTERED)
	{
		if ((specialType == SPEC_SECT_INDEX_BTREE && blkno == BTREE_METAPAGE) ||
			(specialType == SPEC_SECT_INDEX_GIN && blkno == GIN_METAPAGE_BLKNO))
		{
			/* If it's a meta page, the meta block will have no tuples */
			EmitXmlPageMeta(blkno, level);
		}
		else if (specialType != SPEC_SECT_INDEX_GIN || !GinPageIsData(page))
		{
			/* Conventional page format */
			EmitXmlPageItemIdArray(page, blkno);
			EmitXmlTuples(page, blkno);
		}
		else
		{
			/* GIN data/posting tree pages don't use IndexTuple or ItemId */
			EmitXmlPostingTreeTids(page, blkno);
		}

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
 * B-Tree index callers may optionally pass a "level"
 */
static void
EmitXmlTag(BlockNumber blkno, uint32 level, const char *name, const char *color,
		   uint32 relfileOff, uint32 relfileOffEnd)
{
	Assert(relfileOff <= relfileOffEnd);

	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	if (level != UINT_MAX)
		printf("      <tag_text>block %u (level %u) %s</tag_text>\n", blkno, level, name);
	else
		printf("      <tag_text>block %u %s</tag_text>\n", blkno, name);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");
}

/*
 * Emit a wxHexEditor tag for an item pointer (ItemId).
 */
static void
EmitXmlItemId(BlockNumber blkno, OffsetNumber offset, ItemId itemId,
			  uint32 relfileOff, const char *textFlags)
{
	/* Interpret the content of each ItemId separately */
	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%lu</end_offset>\n", (relfileOff + sizeof(ItemIdData)) - 1);
	printf("      <tag_text>(%u,%d) lp_len: %u, lp_off: %u, lp_flags: %s</tag_text>\n",
		   blkno, offset, ItemIdGetLength(itemId), ItemIdGetOffset(itemId), textFlags);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>" COLOR_BLUE_LIGHT "</note_colour>\n");
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
static void
EmitXmlTupleTag(BlockNumber blkno, OffsetNumber offset, const char *name,
				const char *color, uint32 relfileOff, uint32 relfileOffEnd)
{
	Assert(relfileOff <= relfileOffEnd);

	printf("    <TAG id=\"%u\">\n", tagNumber++);
	printf("      <start_offset>%u</start_offset>\n", relfileOff);
	printf("      <end_offset>%u</end_offset>\n", relfileOffEnd);
	printf("      <tag_text>(%u,%u) %s</tag_text>\n", blkno, offset, name);
	printf("      <font_colour>" COLOR_FONT_STANDARD "</font_colour>\n");
	printf("      <note_colour>%s</note_colour>\n", color);
	printf("    </TAG>\n");
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
				 unsigned int itemSize)
{
	char	   *flagString;
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;

	/*
	 * The choice of colors here is not completely arbitrary, or based on
	 * aesthetic preferences.  There is some attempt at analogy in the choice
	 * of colors.  For example, xmin and xmax are symmetric, and so are both
	 * COLOR_RED_LIGHT.
	 */
	relfileOffNext = relfileOff + sizeof(TransactionId);
	EmitXmlTupleTag(blkno, offset, "xmin", COLOR_RED_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(TransactionId);
	EmitXmlTupleTag(blkno, offset, "xmax", COLOR_RED_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;

	if (!(htup->t_infomask & HEAP_MOVED))
	{
		/*
		 * t_cid is COLOR_RED_DARK in order to signal that it's associated with
		 * though somewhat different to xmin and xmax.
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
	 * Don't use ItemPointerData directly, to avoid having
	 * apparent mix in endianness in these fields.  Delineate
	 * which subfield is which by using multiple tags.
	 *
	 * The block component of each TID is COLOR_BLUE_LIGHT.  The same color is
	 * used for ItemIds, since both are physical pointers.  offsetNumber is a
	 * logical pointer, though, and so we make that COLOR_BLUE_DARK to slightly
	 * distinguish it.
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_ctid->bi_hi", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_ctid->bi_lo", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	/*
	 * Note: offsetNumber could be SpecTokenOffsetNumber, but we don't annotate
	 * that
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_ctid->offsetNumber", COLOR_BLUE_DARK,
					relfileOff, relfileOffNext - 1);

	flagString = GetHeapTupleHeaderFlags(htup, true);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_GREEN_LIGHT, relfileOff,
					relfileOffNext - 1);
	free(flagString);
	flagString = GetHeapTupleHeaderFlags(htup, false);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_GREEN_DARK, relfileOff,
					relfileOffNext - 1);
	free(flagString);

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, in line
	 * with general convention
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint8);
	EmitXmlTupleTag(blkno, offset, "t_hoff", COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);
	/*
	 * Whatever follows must be null bitmap (until t_hoff).
	 *
	 * Note that an Oid field will appear as the final 4 bytes of t_bits when
	 * (t_infomask & HEAP_HASOID).  This seems like the most faithful
	 * representation, because there is never any distinct field to t_hoff in
	 * the heap tuple struct: macros like HeapTupleHeaderGetOid() are built
	 * around working backwards from t_hoff knowing that the last sizeof(Oid)/4
	 * bytes must be an Oid when Oids are in use (just like this code).  Oid is
	 * a fixed size field that hides at the end of the variable sized t_bits
	 * array.
	 */
	relfileOff = relfileOffNext;
	relfileOffNext = relfileOffOrig + htup->t_hoff;
	EmitXmlTupleTag(blkno, offset, "t_bits", COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);

	/*
	 * Handle rare edge case where tuple has no contents because it consists
	 * entirely of NULL attributes.  We trust lp_len to handle this, which is
	 * what caller passed us.
	 */
	if (itemSize == (relfileOffNext - relfileOffOrig))
		return;
	else if (itemSize < (relfileOffNext - relfileOffOrig))
	{
		fprintf(stderr, "pg_hexedit error: lp_len %d from (%u,%u) is undersized.\n",
				itemSize, blkno, offset);
		exitCode = 1;
		return;
	}

	/*
	 * Tuple contents (all attributes/columns) is slightly off-white, to
	 * suggest that we can't parse it due to not having access to catalog
	 * metadata, but consider it to be "payload", in constrast to the plain
	 * white area in the "hole" between the upper and lower sections of each
	 * page.
	 */
	relfileOff = relfileOffNext;
	EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE, relfileOff,
					(relfileOffOrig + itemSize) - 1);
}

/*
 * Emit a wxHexEditor tag for entire B-Tree/GIN index tuple.
 *
 * Note: Caller does not need to pass itemSize from ItemId, because that's
 * redundant in the case of IndexTuples.
 */
static void
EmitXmlIndexTuple(BlockNumber blkno, OffsetNumber offset, IndexTuple tuple,
				  uint32 relfileOff)
{
	uint32		relfileOffNext = 0;
	uint32		relfileOffOrig = relfileOff;
	char	   *flagString;

	/*
	 * Emit t_tid tags.  TID tag style should be kept consistent with
	 * EmitXmlHeapTuple().
	 *
	 * Note: Leaf-level GIN pages are rather similar to nbtree leaf pages (and
	 * nbtree internal pages) in that they consist of keys of a cataloged type
	 * (which pg_hexedit, as a front-end utility, cannot reason about), plus a
	 * simple t_tid pointer.  However, posting tree entries, non-leaf entries,
	 * and pending list entries perform special punning of t_tid within
	 * IndexTuples, which we currently don't highlight in any way.  See
	 * GinFormTuple() and its callers.
	 *
	 * TODO: We should decode the meaning of t_tid when this GIN-private t_tid
	 * offset number punning has taken place, and cut this information into
	 * finer detail.  There is quite a bit of discoverable information we could
	 * tag/annotate directly, to show details of posting list compression, etc.
	 */
	relfileOffNext = relfileOff + sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_tid->bi_hi", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_tid->bi_lo", COLOR_BLUE_LIGHT, relfileOff,
					relfileOffNext - 1);
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(uint16);
	EmitXmlTupleTag(blkno, offset, "t_tid->offsetNumber", COLOR_BLUE_DARK,
					relfileOff, relfileOffNext - 1);

	/*
	 * Metadata about the tuple shape and width is COLOR_YELLOW_DARK, which
	 * also matches EmitXmlHeapTuple()
	 */
	relfileOff = relfileOffNext;
	relfileOffNext += sizeof(unsigned short);
	flagString = GetIndexTupleFlags(tuple);
	EmitXmlTupleTag(blkno, offset, flagString, COLOR_YELLOW_DARK, relfileOff,
					relfileOffNext - 1);
	free(flagString);
	relfileOff = relfileOffNext;

	/*
	 * NULL bitmap, if any, is counted as a separate tag, and not an extension
	 * of t_info.  This is a little arbitrary, but makes more sense overall.
	 * This matches heap tuple header tags.
	 *
	 * GIN has special rules for multicolumn indexes.  We don't break down the
	 * structure of GIN's special representation of NULLness because doing so
	 * requires access to catalog metadata.  See the GIN README for details.
	 */
	if (IndexTupleHasNulls(tuple))
	{
		relfileOffNext +=
			(IndexInfoFindDataOffset(tuple->t_info) - (relfileOff - relfileOffOrig));

		EmitXmlTupleTag(blkno, offset, "IndexAttributeBitMapData array",
						COLOR_YELLOW_DARK, relfileOff, relfileOffNext - 1);
		relfileOff = relfileOffNext;
	}

	/*
	 * Tuple contents.
	 *
	 * All-attributes-NULL IndexTuples will not have any contents here, so we
	 * avoid creating a tuple content tag entirely.  The same applies to "minus
	 * infinity" items from internal pages (though they don't have a NULL
	 * bitmap).
	 *
	 * We don't use the lp_len value here, though we could do it that way
	 * instead (we do use lp_len at the same point within EmitXmlHeapTuple()).
	 * The lp_len field is redundant for B-Tree indexes, and somebody might
	 * take advantage of that fact in the future, so this seems more
	 * future-proof.
	 */
	relfileOffNext = relfileOffOrig + IndexTupleSize(tuple);
	if (relfileOff < relfileOffNext)
	{
		if (specialType != SPEC_SECT_INDEX_GIN || !GinItupIsCompressed(tuple))
			EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE,
							relfileOff, relfileOffNext - 1);
		else
		{
			Size postoffset = IndexTupleSize(tuple) - GinGetPostingOffset(tuple);

			EmitXmlTupleTag(blkno, offset, "contents", COLOR_WHITE,
							relfileOff, (relfileOffNext - postoffset) - 1);
			relfileOff = relfileOffNext - postoffset;
			/* Compressed TIDs are orange */
			EmitXmlTupleTag(blkno, offset, "posting list", COLOR_ORANGE,
							relfileOff, relfileOffNext - 1);
		}
	}
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
		/* Interpret the content of the header */
		PageHeader	pageHeader = (PageHeader) page;
		XLogRecPtr	pageLSN = GetPageLsn(page);
		int			maxOffset = PageGetMaxOffsetNumber(page);
		char		flagString[100];


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
		 * as two 32-bit values.  This makes interpreting what is really just a
		 * 64-bit unsigned int confusing on little-endian systems, because the
		 * bytes are "in big endian order" across its two 32-bit halves, but
		 * are in the expected little-endian order *within* each half.
		 *
		 * This is rather similar to the situation with t_ctid.  Unlike in that
		 * case, we choose to make LSN a single field here, because we don't
		 * want to have two tooltips with the format value for each field.
		 */
		sprintf(flagString, "LSN: %X/%08X", (uint32) (pageLSN >> 32), (uint32) pageLSN);
		EmitXmlTag(blkno, level, flagString, COLOR_YELLOW_LIGHT, pageOffset,
				   (pageOffset + sizeof(PageXLogRecPtr)) - 1);
		EmitXmlTag(blkno, level, "checksum", COLOR_GREEN_BRIGHT,
				   pageOffset + offsetof(PageHeaderData, pd_checksum),
				   (pageOffset + offsetof(PageHeaderData, pd_flags)) - 1);

		/* Generate generic page header flags */
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
			fprintf(stderr, "pg_hexedit error: invalid header information.\n");
			exitCode = 1;
		}

		/*
		 * Verify checksums if requested
		 */
		if (blockOptions & BLOCK_CHECKSUMS)
		{
			uint32		delta = (segmentSize / blockSize) * segmentNumber;
			uint16		calc_checksum = pg_checksum_page(page, delta + blkno);

			if (calc_checksum != pageHeader->pd_checksum)
			{
				fprintf(stderr, "pg_hexedit error: checksum failure: calculated 0x%04x.\n",
					   calc_checksum);
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
		fprintf
			(stderr,
			 "pg_hexedit error: end of block encountered within the header."
			 "bytes read: %4u.\n", bytesToFormat);
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
		EmitXmlTag(blkno, level, "btm_magic", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_magic),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_version) - 1));
		EmitXmlTag(blkno, level, "btm_version", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_version),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_root) - 1));
		EmitXmlTag(blkno, level, "btm_root", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_root),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_level) - 1));
		EmitXmlTag(blkno, level, "btm_level", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_level),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_fastroot) - 1));
		EmitXmlTag(blkno, level, "btm_fastroot", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_fastroot),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel) - 1));
		EmitXmlTag(blkno, level, "btm_fastlevel", COLOR_PINK,
				   metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel),
				   (metaStartOffset + offsetof(BTMetaPageData, btm_fastlevel) + sizeof(uint32) - 1));
	}
	else if (specialType == SPEC_SECT_INDEX_GIN && blkno == GIN_METAPAGE_BLKNO)
	{
		EmitXmlTag(blkno, level, "head", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, head),
				   (metaStartOffset + offsetof(GinMetaPageData, tail) - 1));
		EmitXmlTag(blkno, level, "tail", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, tail),
				   (metaStartOffset + offsetof(GinMetaPageData, tailFreeSize) - 1));
		EmitXmlTag(blkno, level, "tailFreeSize", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, tailFreeSize),
				   (metaStartOffset + offsetof(GinMetaPageData, nPendingPages) - 1));
		EmitXmlTag(blkno, level, "nPendingPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nPendingPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nPendingHeapTuples) - 1));
		EmitXmlTag(blkno, level, "nPendingHeapTuples", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nPendingHeapTuples),
				   (metaStartOffset + offsetof(GinMetaPageData, nTotalPages) - 1));
		EmitXmlTag(blkno, level, "nTotalPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nTotalPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nEntryPages) - 1));
		EmitXmlTag(blkno, level, "nEntryPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nEntryPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nDataPages) - 1));
		EmitXmlTag(blkno, level, "nDataPages", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nDataPages),
				   (metaStartOffset + offsetof(GinMetaPageData, nEntries) - 1));
		EmitXmlTag(blkno, level, "nEntries", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, nEntries),
				   (metaStartOffset + offsetof(GinMetaPageData, ginVersion) - 1));
		EmitXmlTag(blkno, level, "ginVersion", COLOR_PINK,
				   metaStartOffset + offsetof(GinMetaPageData, ginVersion),
				   ((metaStartOffset + offsetof(GinMetaPageData, ginVersion) + sizeof(int32)) - 1));
	}
	else
	{
		fprintf(stderr, "pg_hexedit error: unsupported metapage special section type \"%s\".\n",
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
	int				maxOffset = PageGetMaxOffsetNumber(page);
	OffsetNumber	offset;
	unsigned int	headerBytes;
	headerBytes = offsetof(PageHeaderData, pd_linp[0]);

	/*
	 * It's either a non-meta index page, or a heap page.  Create tags
	 * for all ItemId entries/item pointers on page.
	 */
	for (offset = FirstOffsetNumber;
		 offset <= maxOffset;
		 offset = OffsetNumberNext(offset))
	{
		ItemId			itemId;
		unsigned int	itemFlags;
		char			textFlags[16];

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
				/* shouldn't be possible */
				sprintf(textFlags, "0x%02x", itemFlags);
				break;
		}

		EmitXmlItemId(blkno, offset, itemId,
					  pageOffset + headerBytes + (sizeof(ItemIdData) * (offset - 1)),
					  textFlags);
	}
}

/*
 * Emit formatted tuples that reside on this block.
 */
static void
EmitXmlTuples(Page page, BlockNumber blkno)
{
	OffsetNumber offset;
	unsigned int itemSize;
	unsigned int itemOffset;
	unsigned int itemFlags;
	ItemId		itemId;
	int			formatAs;
	int			maxOffset = PageGetMaxOffsetNumber(page);

	/*
	 * Loop through the items on the block.  Check if the block is empty and
	 * has a sensible item array listed before running through each item
	 */
	if (maxOffset == 0)
	{
		/* This can happen within GIN when only pending list full */
		if (specialType == SPEC_SECT_INDEX_GIN)
			return;

		fprintf(stderr, "pg_hexedit error: empty block %u - no items listed.\n",
				blkno);
		exitCode = 1;
		return;
	}
	else if ((maxOffset < 0) || (maxOffset > blockSize))
	{
		fprintf(stderr, "pg_hexedit error: item index corrupt on block %u. offset: %d\n",
			   blkno, maxOffset);
		exitCode = 1;
		return;
	}

	/* Use the special section to determine the format style */
	switch (specialType)
	{
		case SPEC_SECT_NONE:
			formatAs = ITEM_HEAP;
			break;
		case SPEC_SECT_INDEX_BTREE:
#ifdef UNIMPLEMENTED
		case SPEC_SECT_INDEX_HASH:
		case SPEC_SECT_INDEX_GIST:
#endif
		case SPEC_SECT_INDEX_GIN:
#ifdef UNIMPLEMENTED
		case SPEC_SECT_INDEX_SPGIST:
		case SPEC_SECT_INDEX_BRIN:
#endif
			formatAs = ITEM_INDEX;
			break;
		default:
			fprintf(stderr, "pg_hexedit error: unsupported special section type \"%s\".\n",
					GetSpecialSectionString(specialType));
			exitCode = 1;
	}

	for (offset = FirstOffsetNumber;
		 offset <= maxOffset;
		 offset = OffsetNumberNext(offset))
	{
		itemId = PageGetItemId(page, offset);
		itemSize = (unsigned int) ItemIdGetLength(itemId);
		itemOffset = (unsigned int) ItemIdGetOffset(itemId);
		itemFlags = (unsigned int) ItemIdGetFlags(itemId);

		/* LD_DEAD items may have storage, so we go by lp_len alone */
		if (itemSize == 0)
		{
			if (itemFlags == LP_NORMAL)
			{
				fprintf(stderr, "pg_hexedit error: (%u,%u) LP_NORMAL item has lp_len 0.\n",
						blkno, offset);
				exitCode = 1;
			}
			continue;
		}
		/* Sanitize */
		if (itemFlags == LP_REDIRECT || itemFlags == LP_UNUSED)
		{
			fprintf(stderr, "pg_hexedit error: (%u,%u) LP_REDIRECT or LP_UNUSED item has lp_len %u",
					blkno, offset, itemSize);
			exitCode = 1;
			continue;
		}

		/*
		 * Make sure the item can physically fit on this block before
		 * formatting.  Since in a future pg version lp_len might be used
		 * for abbreviated keys in indexes, only insist on this for heap
		 * pages
		 */
		if (formatAs == ITEM_HEAP &&
			((itemOffset + itemSize > blockSize) ||
			 (itemOffset + itemSize > bytesToFormat)))
		{
			fprintf(stderr, "pg_hexedit error: (%u,%u) item contents extend beyond block.\n"
				   "blocksize %d  bytes read  %d  item start %d .\n",
				   blkno, offset, blockSize, bytesToFormat,
				   itemOffset + itemSize);
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

			tuple = (IndexTuple) PageGetItem(page, itemId);

			EmitXmlIndexTuple(blkno, offset, tuple,
							  pageOffset + itemOffset);
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
			 * posting tree page) they are keys, not pointers
			 */
			EmitXmlTupleTag(blkno, offsetnum, "PostingItem->key->hi_lo", COLOR_WHITE,
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
		GinPostingList *seg, *nextseg;
		Pointer			endptr;

		/*
		 * See description of posting page/data page format at top of
		 * ginpostlinglist.c for more information.
		 *
		 * TODO: Add support for pre-9.4 uncompressed data pages.  We don't
		 * support those versions, but arguably we should at least support the
		 * format across pg_upgrade.
		 */
		if (!GinPageIsCompressed(page))
		{
			fprintf(stderr, "pg_hexedit error: uncompressed GIN leaf pages unsupported.\n");
			exitCode = 1;
			return;
		}

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
 * On blocks that have special sections, print the contents according to
 * previously determined special section type
 */
static void
EmitXmlSpecial(BlockNumber blkno, uint32 level)
{
	PageHeader	pageHeader = (PageHeader) buffer;
	char		flagString[100] = "\0";
	unsigned int specialOffset = pageHeader->pd_special;

	switch (specialType)
	{
		case SPEC_SECT_ERROR_UNKNOWN:
		case SPEC_SECT_ERROR_BOUNDARY:
			fprintf(stderr, "pg_hexedit error: invalid special section type \"%s\".\n",
					GetSpecialSectionString(specialType));
			exitCode = 1;
			break;

		case SPEC_SECT_INDEX_BTREE:
			{
				BTPageOpaque btreeSection = (BTPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "btpo_prev", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_prev),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_next)) - 1);
				EmitXmlTag(blkno, level, "btpo_next", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_next),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo)) - 1);
				EmitXmlTag(blkno, level, "btpo.level", COLOR_BLACK,
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

				EmitXmlTag(blkno, level, flagString, COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_flags),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid)) - 1);
				EmitXmlTag(blkno, level, "btpo_cycleid", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid),
						   (pageOffset + specialOffset + offsetof(BTPageOpaqueData, btpo_cycleid) + sizeof(BTCycleId)) - 1);
			}
			break;

		case SPEC_SECT_INDEX_GIN:
			{
				GinPageOpaque ginSection = (GinPageOpaque) (buffer + specialOffset);

				EmitXmlTag(blkno, level, "rightlink", COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(GinPageOpaqueData, rightlink),
						   (pageOffset + specialOffset + offsetof(GinPageOpaqueData, maxoff)) - 1);
				EmitXmlTag(blkno, level, "maxoff", COLOR_BLACK,
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

				EmitXmlTag(blkno, level, flagString, COLOR_BLACK,
						   pageOffset + specialOffset + offsetof(GinPageOpaqueData, flags),
						   (pageOffset + specialOffset + offsetof(GinPageOpaqueData, flags) + sizeof(uint16)) - 1);
			}
			break;

		default:
			fprintf(stderr, "pg_hexedit error: unsupported special section type \"%s\".\n",
					GetSpecialSectionString(specialType));
			exitCode = 1;
	}
}

/*
 * Control the dumping of the blocks within the file
 */
static void
EmitXmlFile(void)
{
	unsigned int initialRead = 1;
	unsigned int contentsToDump = 1;

	/*
	 * If the user requested a block range, seek to the correct position
	 * within the file for the start block.
	 */
	if (blockOptions & BLOCK_RANGE)
	{
		unsigned int position = blockSize * blockStart;

		if (fseek(fp, position, SEEK_SET) != 0)
		{
			fprintf(stderr, "pg_hexedit error: seek error encountered before requested "
				   "start block %d.\n", blockStart);
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
				fprintf(stderr, "pg_hexedit error: premature end of file encountered.\n");
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

	EmitXmlFooter();
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
		EmitXmlDocHeader(argv, argc);
		blockSize = GetBlockSize();

		/*
		 * On a positive block size, allocate a local buffer to store the
		 * subsequent blocks
		 */
		if (blockSize > 0)
		{
			buffer = (char *) malloc(blockSize);
			if (buffer)
				EmitXmlFile();
			else
			{
				fprintf(stderr, "pg_hexedit error: unable to create buffer of size %d.\n",
					   blockSize);
				exitCode = 1;
			}
		}
	}

	/* Close out the file and get rid of the allocated block buffer */
	if (fp)
		fclose(fp);

	if (buffer)
		free(buffer);

	exit(exitCode);
}

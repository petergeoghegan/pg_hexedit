/*
 * pg_relfilenodemapdata.c - PostgreSQL utility
 *                           that reads the data from pg_filenode.map files.
 *
 * Copyright (c) 2018, VMware, Inc.
 * Copyright (c) 2018, PostgreSQL Global Development Group
 *
 * This is a standalone utlity for displaying the mappings within either a
 * global or per-database pg_filenode.map.
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
 * pg_relfilenodemapdata author: Peter Geoghegan <pg@bowt.ie>
 */
#include "postgres.h"

#include "catalog/indexing.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_pltemplate.h"
#include "catalog/pg_proc.h"
#if PG_VERSION_NUM >= 90500
#include "catalog/pg_replication_origin.h"
#endif
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#if PG_VERSION_NUM >= 100000
#include "catalog/pg_subscription.h"
#endif
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"

#define HEXEDIT_VERSION					"0.1"

/* Use macros to handle move from CRC-32 to CRC-32C in Postgres 9.5 */
#if PG_VERSION_NUM >= 90500
#include "port/pg_crc32c.h"
typedef pg_crc32c port_crc32;
#define	PORT_INIT_CRC32(crc)			INIT_CRC32C((crc))
#define	PORT_COMP_CRC32(crc,data,len)	COMP_CRC32C((crc), (data), (len))
#define	PORT_FIN_CRC32(crc)				FIN_CRC32C((crc))
#define	PORT_EQ_CRC32(c1,c2)			EQ_CRC32C((c1), (c2))
#define PORT_FAIL_HINT					"PostgreSQL 9.4"
#else
#include "utils/pg_crc.h"
typedef pg_crc32 port_crc32;
#define	PORT_INIT_CRC32(crc)			INIT_CRC32((crc))
#define	PORT_COMP_CRC32(crc,data,len)	COMP_CRC32((crc), (data), (len))
#define	PORT_FIN_CRC32(crc)				FIN_CRC32((crc))
#define	PORT_EQ_CRC32(c1,c2)			EQ_CRC32((c1), (c2))
#define PORT_FAIL_HINT					"PostgreSQL 9.5+"
#endif

/* Provide system catalog OIDs where unavailable */
#if PG_VERSION_NUM < 90500
#define ReplicationOriginRelationId		6000
#define ReplicationOriginIdentIndex		6001
#define ReplicationOriginNameIndex		6002
#define PgShseclabelToastTable			4060
#define PgShseclabelToastIndex			4061
#endif
#if PG_VERSION_NUM < 100000
#define SubscriptionRelationId			6100
#define SubscriptionObjectIndexId		6114
#define SubscriptionNameIndexId			6115
#endif

/*
 * catalog/toasting.h doesn't bother to provide constants for these two pg_proc
 * TOAST relations, presumably because no core code needs to reference the
 * toast tables.  We invent our own constants, to be consistent.
 */
#define OID_PG_TOAST_1255				2836
#define OID_PG_TOAST_1255_INDEX			2837

/* For calculating display padding */
#define CATALOG_NAME_COLS				45
#define ENTRY_NUM_COLS					2

/* These constants appear at the top of PostgreSQL's relmapper.c */
#define RELMAPPER_FILEMAGIC				0x592717	/* version ID value */
#define MAX_MAPPINGS					62			/* 62 * 8 + 16 = 512 */

/* Struct definitions from the top of PostgreSQL's relmapper.c */
typedef struct RelMapping
{
	Oid			mapoid;			/* OID of a catalog */
	Oid			mapfilenode;	/* its filenode number */
} RelMapping;

typedef struct RelMapFile
{
	int32		magic;			/* always RELMAPPER_FILEMAGIC */
	int32		num_mappings;	/* number of valid RelMapping entries */
	RelMapping	mappings[MAX_MAPPINGS];
	port_crc32	crc;			/* CRC of all above */
	int32		pad;			/* to make the struct size be 512 exactly */
} RelMapFile;

/* Possible return codes from option validation routine */
typedef enum optionReturnCodes
{
	OPT_RC_VALID,				/* All options are valid */
	OPT_RC_INVALID,				/* Improper option string */
	OPT_RC_COPYRIGHT			/* Copyright should be displayed */
} optionReturnCodes;

/* Program exit code */
static int exitCode = 0;

static void DisplayOptions(unsigned int validOptions);
static unsigned int ConsumeOptions(int numOptions, char **options);
static const char *GetCatalogNameFromOid(Oid classOid);
static void PrintRelMapContents(RelMapFile *map);
static void VerifyRelMapContents(RelMapFile *map);
static bool InitRelMapFromFile(char *mapFileName, RelMapFile *map);

/*
 * Send properly formed usage information to the user
 */
static void
DisplayOptions(unsigned int validOptions)
{
	if (validOptions == OPT_RC_COPYRIGHT)
		printf
			("pg_relfilenodemapdata %s (for PostgreSQL %s)"
			 "\nCopyright (c) 2018, VMware, Inc.\n",
			 HEXEDIT_VERSION, PG_VERSION);
	printf
		("\nUsage: pg_relfilenodemapdata file\n\n"
		 "Displays details from a PostgreSQL pg_filenode.map file\n"
		 "\nReport bugs to <pg@bowt.ie>\n");
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

	/* For now, only accept a single file argument, without flags */
	if (numOptions != 2)
		rc = OPT_RC_INVALID;

	return rc;
}

/*
 * Get the name of a known sytem catalog from its pg_class OID
 */
static const char *
GetCatalogNameFromOid(Oid classOid)
{
	switch (classOid)
	{
		/* Local/nailed mappings */
		case RelationRelationId:
			return "pg_class";
		case AttributeRelationId:
			return "pg_attribute";
		case ProcedureRelationId:
			return "pg_proc";
		case TypeRelationId:
			return "pg_type";
		case OID_PG_TOAST_1255:
			return "pg_toast_1255";
		case OID_PG_TOAST_1255_INDEX:
			return "pg_toast_1255_index";
		case AttributeRelidNameIndexId:
			return "pg_attribute_relid_attnam_index";
		case AttributeRelidNumIndexId:
			return "pg_attribute_relid_attnum_index";
		case ClassOidIndexId:
			return "pg_class_oid_index";
		case ClassNameNspIndexId:
			return "pg_class_relname_nsp_index";
		case ClassTblspcRelfilenodeIndexId:
			return "pg_class_tblspc_relfilenode_index";
		case ProcedureOidIndexId:
			return "pg_proc_oid_index";
		case ProcedureNameArgsNspIndexId:
			return "pg_proc_proname_args_nsp_index";
		case TypeOidIndexId:
			return "pg_type_oid_index";
		case TypeNameNspIndexId:
			return "pg_type_typname_nsp_index";

		/* Global/shared mappings */
		case DatabaseRelationId:
			return "pg_database";
		case DbRoleSettingRelationId:
			return "pg_db_role_setting";
		case TableSpaceRelationId:
			return "pg_tablespace";
		case PLTemplateRelationId:
			return "pg_pltemplate";
		case AuthIdRelationId:
			return "pg_authid";
		case AuthMemRelationId:
			return "pg_auth_members";
		case SharedDependRelationId:
			return "pg_shdepend";
		case ReplicationOriginRelationId:
			return "pg_replication_origin";
		case SharedDescriptionRelationId:
			return "pg_shdescription";
		case SharedSecLabelRelationId:
			return "pg_shseclabel";
		case SubscriptionRelationId:
			return "pg_subscription";
		case PgShdescriptionToastTable:
			return "pg_toast_2396";
		case PgShdescriptionToastIndex:
			return "pg_toast_2396_index";
		case PgDbRoleSettingToastTable:
			return "pg_toast_2964";
		case PgDbRoleSettingToastIndex:
			return "pg_toast_2964_index";
		case PgShseclabelToastTable:
			return "pg_toast_3592";
		case PgShseclabelToastIndex:
			return "pg_toast_3592_index";
		case AuthIdRolnameIndexId:
			return "pg_authid_rolname_index";
		case AuthIdOidIndexId:
			return "pg_authid_oid_index";
		case AuthMemRoleMemIndexId:
			return "pg_auth_members_role_member_index";
		case AuthMemMemRoleIndexId:
			return "pg_auth_members_member_role_index";
		case DatabaseNameIndexId:
			return "pg_database_datname_index";
		case DatabaseOidIndexId:
			return "pg_database_oid_index";
		case SharedDescriptionObjIndexId:
			return "pg_shdescription_o_c_index";
		case PLTemplateNameIndexId:
			return "pg_pltemplate_name_index";
		case SharedDependDependerIndexId:
			return "pg_shdepend_depender_index";
		case SharedDependReferenceIndexId:
			return "pg_shdepend_reference_index";
		case TablespaceOidIndexId:
			return "pg_tablespace_oid_index";
		case TablespaceNameIndexId:
			return "pg_tablespace_spcname_index";
		case DbRoleSettingDatidRolidIndexId:
			return "pg_db_role_setting_databaseid_rol_index";
		case SharedSecLabelObjectIndexId:
			return "pg_shseclabel_object_index";
		case ReplicationOriginIdentIndex:
			return "pg_replication_origin_roiident_index";
		case ReplicationOriginNameIndex:
			return "pg_replication_origin_roname_index";
		case SubscriptionObjectIndexId:
			return "pg_subscription_oid_index";
		case SubscriptionNameIndexId:
			return "pg_subscription_subname_index";

		/*
		 * We expect to be able to identify every mapped catalog on supported
		 * versions.  If we haven't got a record of this catalog's OID, our
		 * assumption is that that's because it's from a future PostgreSQL
		 * version.
		 */
		default:
			return "unlisted system catalog relation";
	}
}

static void
PrintRelMapContents(RelMapFile *map)
{
	int32		num_mappings = Min(MAX_MAPPINGS, map->num_mappings);
	int			i;

	/* Print pg_filenode.map file's header */
	printf("magic:               0x%.8X\n"
		   "num_mappings:        %d\n\n", map->magic, map->num_mappings);

	/* Print mappings from file */
	for (i = 0; i < num_mappings; i++)
	{
		Oid				reloid = map->mappings[i].mapoid;
		Oid				relfilenode = map->mappings[i].mapfilenode;
		const char	   *catalogname;

		catalogname = GetCatalogNameFromOid(reloid);

		printf("%*d) %d - %s: %*d\n", ENTRY_NUM_COLS, i, reloid, catalogname,
			   CATALOG_NAME_COLS - (int) strlen(catalogname), relfilenode);
	}

	/* Print CRC-32C checksum at the end of the file */
	printf("\nfile checksum:       0x%.8X\n", map->crc);
}

/*
 * Verify the consistency of a pg_relfilenode.map file.
 *
 * Checks that magic number matches, and verifies CRC.
 */
static void
VerifyRelMapContents(RelMapFile *map)
{
	port_crc32	crc;

	if (map->magic != RELMAPPER_FILEMAGIC ||
		map->num_mappings < 0 ||
		map->num_mappings > MAX_MAPPINGS)
	{
		fprintf(stderr, "relation mapping file contains invalid data\n");
		exitCode = 1;
	}

	/* Print our own CRC-32/CRC-32C calculation */
	PORT_INIT_CRC32(crc);
	PORT_COMP_CRC32(crc, (char *) map, offsetof(RelMapFile, crc));
	PORT_FIN_CRC32(crc);

	/* Raise error if they don't match */
	if (!PORT_EQ_CRC32(crc, map->crc))
	{
		fprintf(stderr, "calculated checksum 0x%.8X does not match file checksum\n",
				crc);
		if (exitCode != 0)
			fprintf(stderr, "if the pg_filenode.map file is from " PORT_FAIL_HINT ", this may be harmless.\n");
		exitCode = 1;
	}
}

/*
 * Given a path to a pg_relfilenode.map file, initialized caller's RelMapFile
 * structure.  Returns true on success.
 */
static bool
InitRelMapFromFile(char *mapFileName, RelMapFile *map)
{
	FILE	   *file = fopen(mapFileName, "rb");

	if (!file)
	{
		fprintf(stderr, "could not open file \"%s\": %s\n", mapFileName,
				strerror(errno));
		exitCode = 1;
		return false;
	}

	if (fread(map, 1, sizeof(RelMapFile), file) != sizeof(RelMapFile))
	{
		fprintf(stderr, "could not read relation mapping file \"%s\": %s\n",
				mapFileName, strerror(errno));
		exitCode = 1;
		fclose(file);
		return false;
	}

	fclose(file);

	return true;
}

int
main(int argv, char **argc)
{
	unsigned int	validOptions;
	RelMapFile		map;

	/* If there is a parameter list, validate the options */
	validOptions = (argv < 2) ? OPT_RC_COPYRIGHT : ConsumeOptions(argv, argc);

	/*
	 * Display valid options if no parameters are received or invalid options
	 * where encountered
	 */
	if (validOptions != OPT_RC_VALID)
		DisplayOptions(validOptions);
	else if (InitRelMapFromFile(argc[1], &map))
	{
		/* Print file header, contents, and footer */
		PrintRelMapContents(&map);

		/* Verify consistency of file last, so that errors appear last */
		VerifyRelMapContents(&map);
	}

	exit(exitCode);
}

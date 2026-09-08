/* pg_hexedit_poke--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_hexedit_poke" to load this file. \quit

CREATE FUNCTION pg_hexedit_poke(rel regclass, blkno integer,
                                "offset" integer, data bytea)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_hexedit_poke'
LANGUAGE C;

REVOKE ALL ON FUNCTION pg_hexedit_poke(regclass, integer, integer, bytea)
FROM PUBLIC;

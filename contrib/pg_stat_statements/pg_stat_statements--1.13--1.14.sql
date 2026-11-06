/* contrib/pg_stat_statements/pg_stat_statements--1.13--1.14.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.14'" to load this file. \quit

-- Run in the leader, which can flush its pending statistics first
ALTER FUNCTION pg_stat_statements(boolean) PARALLEL RESTRICTED;

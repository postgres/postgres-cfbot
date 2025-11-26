/*------------------------------------------------------------------------
 *
 * system_version.c
 *	  Functions for reporting version of system components.
 *
 * A system component is defined very broadly here, it might be the PostgreSQL
 * core itself, the compiler, the host system, any dependency that is used at
 * compile time or run time.
 *
 * Version reporting is implemented via a hash table containing the component's
 * name as a key and the callback to fetch the version string. Every component
 * can register such a callback during initialization and is responsible for
 * exposing its own information. The idea is that storing a callback instead of
 * a version string directly allows for more flexibility about how and when the
 * information could be reported.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/system_version.c
 *
 *------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_ICU
#include <unicode/uchar.h>
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/system_version.h"
#include "utils/tuplestore.h"

static HTAB *versions = NULL;

void
add_system_version(const char *name, SystemVersionCB cb, VersionType type)
{
	SystemVersion *hentry;
	const char *key;
	bool		found;

	if (!versions)
	{
		HASHCTL		ctl;

		ctl.keysize = NAMEDATALEN;
		ctl.entrysize = sizeof(SystemVersion);

		versions = hash_create("System versions table",
							   MAX_SYSTEM_VERSIONS,
							   &ctl,
							   HASH_ELEM | HASH_STRINGS);
	}

	key = pstrdup(name);
	Assert(strlen(key) < NAMEDATALEN);

	hentry = (SystemVersion *) hash_search(versions, key,
										   HASH_ENTER, &found);

	/* Duplicated entries are not expected */
	Assert(!found);

	hentry->callback = cb;
	hentry->type = type;
}

static const char *
core_get_version(bool *available)
{
	*available = true;
	return (const char *) psprintf("%s", PG_VERSION);
}

static const char *
core_get_arch(bool *available)
{
	*available = true;
	return (const char *) psprintf("%s", PG_ARCH_STR);
}

static const char *
core_get_compiler(bool *available)
{
	*available = true;
	return (const char *) psprintf("%s", PG_CC_STR);
}

#ifdef USE_ICU
static const char *
icu_get_version(bool *available)
{
	UVersionInfo UCDVersion;
	char	   *version = palloc0(U_MAX_VERSION_STRING_LENGTH);

	*available = true;
	u_getUnicodeVersion(UCDVersion);
	u_versionToString(UCDVersion, version);
	return (const char *) version;
}
#endif

#ifdef __GLIBC__
static const char *
glibc_get_version(bool *available)
{
	*available = true;
	return (const char *) gnu_get_libc_version();
}
#endif

/*
 * Register versions that describe core components and do not correspond to any
 * individual component.
 */
void
register_core_versions()
{
	add_system_version("Core", core_get_version, CompileTime);
	add_system_version("Arch", core_get_arch, CompileTime);
	add_system_version("Compiler", core_get_compiler, CompileTime);

#ifdef USE_ICU
	add_system_version("ICU", icu_get_version, RunTime);
#endif

#ifdef __GLIBC__
	add_system_version("Glibc", glibc_get_version, RunTime);
#endif
}

/*
 * pg_get_system_versions
 *
 * List information about system versions.
 */
Datum
pg_get_system_versions(PG_FUNCTION_ARGS)
{
#define PG_GET_SYS_VERSIONS_COLS 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS status;
	SystemVersion *hentry;

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	if (!versions)
		return (Datum) 0;

	hash_seq_init(&status, versions);
	while ((hentry = (SystemVersion *) hash_seq_search(&status)) != NULL)
	{
		Datum		values[PG_GET_SYS_VERSIONS_COLS] = {0};
		bool		nulls[PG_GET_SYS_VERSIONS_COLS] = {0};
		bool		available = false;
		const char *version = hentry->callback(&available);

		if (!available)
			continue;

		values[0] = CStringGetTextDatum(hentry->name);
		values[1] = CStringGetTextDatum(version);
		values[2] = Int32GetDatum(hentry->type);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

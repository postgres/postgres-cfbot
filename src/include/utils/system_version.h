/*-------------------------------------------------------------------------
 * system_version.h
 *	  Definitions related to system versions reporting
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * src/include/utils/system_version.h
 * ----------
 */

#ifndef SYSTEM_VERSION_H
#define SYSTEM_VERSION_H

#define MAX_SYSTEM_VERSIONS 100

typedef enum VersionType
{
	CompileTime,
	RunTime,
} VersionType;

/*
 * Callback to return version string of a system component.
 * The version might be not available, what is indicated via the argument.
 */
typedef const char *(*SystemVersionCB) (bool *available);

typedef struct SystemVersion
{
	char		name[NAMEDATALEN];	/* Unique component name, used as a key
									 * for versions HTAB */
	VersionType type;
	SystemVersionCB callback;	/* Callback to fetch the version string */
} SystemVersion;

void		add_system_version(const char *name, SystemVersionCB cb, VersionType type);
extern void register_core_versions(void);

#endif							/* SYSTEM_VERSION_H */

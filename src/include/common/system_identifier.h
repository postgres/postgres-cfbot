/*-------------------------------------------------------------------------
 *
 * system_identifier.h
 *    Common utility to generate a system identifier
 *
 *-------------------------------------------------------------------------
 */

#ifndef SYSTEM_IDENTIFIER_H
#define SYSTEM_IDENTIFIER_H

#include <stdint.h>

/*
 * Generate a new system identifier.
 */
extern uint64 GenerateSystemIdentifier(void);

#endif		/* SYSTEM_IDENTIFIER_H */
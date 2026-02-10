/*-------------------------------------------------------------------------
 *
 * vci_symbols.c
 *	   Converts a string from a node tag
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/utils/vci_symbols.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodes.h"

#include "vci.h"
#include "vci_utils.h"

#define Item(X) {T_ ## X, # X},

typedef struct
{
	NodeTag		type;
	const char *name;
} node_info_t;

static const node_info_t node_info_table[] = {
#include "nodes.t"
};

#undef Item

/*
 * Returns a literal from a node
 *
 * XXX This is used for debugging or error reporting purposes. Performance is
 * ignored for now, the linear search is used.
 */
const char *
VciGetNodeName(NodeTag type)
{
	for (int i = 0; i < lengthof(node_info_table); i++)
		if (node_info_table[i].type == type)
			return node_info_table[i].name;

	return "Unknown";
}

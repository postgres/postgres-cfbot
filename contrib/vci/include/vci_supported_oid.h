/*-------------------------------------------------------------------------
 *
 * vci_supported_oid.h
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_supported_oid.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_SUPPORTED_OID_H
#define VCI_SUPPORTED_OID_H

#include "utils/snapshot.h"

#define VCI_MAX_APPLICABLE_UDFS (32)

typedef struct
{
	int			num_applicable_udfs;
	Oid			applicable_udfs[VCI_MAX_APPLICABLE_UDFS];
	Oid			vci_runs_in_plan_funcoid;
	Oid			vci_always_return_true_funcoid;
} vci_special_udf_info_t;

extern vci_special_udf_info_t vci_special_udf_info;

extern bool vci_is_supported_type(Oid oid);
extern bool vci_is_supported_function(Oid oid);
extern void vci_register_applicable_udf(Snapshot snapshot);

#endif							/* VCI_SUPPORTED_OID_H */

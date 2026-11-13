/*-------------------------------------------------------------------------
 *
 * vci_read_guc.c
 *	  GUC parameter settings
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/vci_read_guc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/procnumber.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/palloc.h"

#include "vci.h"

#include "vci_executor.h"
#include "vci_mem.h"

/* GUC parameter holder */
VciGucStruct VciGuc;

static void check_max_worker_processes(void);

static const struct config_enum_entry table_scan_policy_options[] = {

	{"column store only", VCI_TABLE_SCAN_POLICY_COLUMN_ONLY, false},
	{"column only", VCI_TABLE_SCAN_POLICY_COLUMN_ONLY, true},
	{"none", VCI_TABLE_SCAN_POLICY_NONE, false},
	{NULL, 0, false}
};

/*
 * These GUC are defined using same format found in
 * src/backend/utils/guc_tables.inc.c
 */
static struct config_generic VciConfigureNames[] =
{
	/*
	 * Bool GUCs
	 */

	/* for internal use */
	{
		.name = "vci.enable",
		.context = PGC_USERSET,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Enables VCI."),
		.flags = GUC_NOT_IN_SAMPLE,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable,
			.boot_val = true,
		}
	},

	{
		.name = "vci.log_query",
		.context = PGC_USERSET,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Logs information when a query fails to be executed by VCI."),
		.flags = GUC_NOT_IN_SAMPLE,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.log_query,
			.boot_val = false,
		}
	},

	{
		.name = "vci.enable_seqscan",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace sequential-scan plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_seqscan,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_indexscan",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace index-scan plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_indexscan,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_bitmapheapscan",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace bitmap-scan plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_bitmapheapscan,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_sort",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace sort plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_sort,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_hashagg",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace hashed aggregation plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_hashagg,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_sortagg",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace sorted aggregation plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_sortagg,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_plainagg",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace plain aggregation plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_plainagg,
			.boot_val = true,
		}
	},

	{
		.name = "vci.enable_hashjoin",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace hash join plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_hashjoin,
			.boot_val = false,
		}
	},

	{
		.name = "vci.enable_nestloop",
		.context = PGC_USERSET,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Enables VCI planner to replace nested-loop plans."),
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_nestloop,
			.boot_val = false,
		}
	},

	{
		.name = "vci.enable_ros_control_daemon",
		.context = PGC_POSTMASTER,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Enables the VCI ROS Control Daemon."),
		.flags = GUC_NOT_IN_SAMPLE,
		.vartype = PGC_BOOL,
		._bool = {
			.variable = &VciGuc.enable_ros_control_daemon,
			.boot_val = false,
		}
	},

	/*
	 * Int GUCs
	 */

	{
		.name = "vci.cost_threshold",
		.context = PGC_USERSET,
		.flags = GUC_NOT_IN_SAMPLE,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Sets the threshold CPU load beyond which the VCI control worker is stopped."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.cost_threshold,
			.boot_val = 18000,
			.min = 0,
			.max = INT_MAX,
		}
	},

	{
		.name = "vci.maintenance_work_mem",
		.context = PGC_SIGHUP,
		.flags = GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Sets the maximum memory to be used by each control worker for VCI control operations."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.maintenance_work_mem,
			.boot_val = 256 * 1024,
			.min = 1024,
			.max = MAX_KILOBYTES,
		},
	},

	/* **************************************** */
	/* ROS Control Daemon/Worker configurations */
	/* **************************************** */

	/* Daemon setup */

	{
		.name = "vci.control_max_workers",
		.context = PGC_POSTMASTER,
		.flags = GUC_NOT_IN_SAMPLE,
		.group = RESOURCES_IO,
		.short_desc = gettext_noop("Sets the maximum number of simultaneously running VCI control worker processes."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.control_max_workers,
			.boot_val = 8,
			.min = 1,
			.max = MAX_BACKENDS,
		}
	},

	{
		.name = "vci.control_naptime",
		.context = PGC_SIGHUP,
		.flags = GUC_NOT_IN_SAMPLE | GUC_UNIT_S,
		.group = RESOURCES_IO,
		.short_desc = gettext_noop("Time to sleep between VCI control worker runs."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.control_naptime,
			.boot_val = 1,
			.min = 1,
			.max = INT_MAX / 1000,
		}
	},

	/* Worker : ROS control command thresholds  */

	{
		.name = "vci.wosros_conv_threshold",
		.context = PGC_SIGHUP,
		.flags = GUC_NOT_IN_SAMPLE,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Sets the threshold of Data WOS rows to execute WOS->ROS conversion."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.wosros_conv_threshold,
			.boot_val = 256 * 1024,
			.min = 1,
			.max = INT_MAX,
		}
	},

	{
		.name = "vci.cdr_threshold",
		.context = PGC_SIGHUP,
		.flags = GUC_NOT_IN_SAMPLE,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Sets the threshold of deleted rows in ROS to execute collect-deleted-rows command."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.cdr_threshold,
			.boot_val = 128 * 1024,
			.min = 1,
			.max = INT_MAX,
		}
	},

	/******************************************/
	/* Custom Plan Execution                  */
	/******************************************/

	{
		.name = "vci.max_local_ros",
		.context = PGC_USERSET,
		.flags = GUC_NOT_IN_SAMPLE |  GUC_UNIT_KB,
		.group = RESOURCES_MEM,
		.short_desc = gettext_noop("Sets the maximum local ROS memory."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.max_local_ros_size,
			.boot_val = 64 * 1024,
			.min = 64 * 1024,
			.max = INT_MAX,
		}
	},

	{
		.name = "vci.table_rows_threshold",
		.context = PGC_USERSET,
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.group = DEVELOPER_OPTIONS,
		.short_desc = gettext_noop("Sets the threshold of table rows to execute VCI Scan."),
		.vartype = PGC_INT,
		._int = {
			.variable = &VciGuc.table_rows_threshold,
			.boot_val = VCI_MAX_FETCHING_ROWS,
			.min = 0,
			.max = INT_MAX,
		}
	},

	/*
	 * Enum GUCs
	 */

	{
		.name = "vci.table_scan_policy",
		.short_desc = gettext_noop("Sets the policy that a scan node reads from the column store table(VCI index) or the row store table(original)."),
		.group = DEVELOPER_OPTIONS,
		.context = PGC_USERSET,
		.flags = GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
		.vartype = PGC_ENUM,
		._enum = {
			.variable = &VciGuc.table_scan_policy,
			.boot_val = VCI_TABLE_SCAN_POLICY_COLUMN_ONLY,
			.options = table_scan_policy_options,
		}
	},

};

/*
 * Set GUC parameters
 */
void
vci_read_guc_variables(void)
{
	/*
	 * TODO: Raise warnings or set parameters to default, when the specified
	 * value is out-of-range.
	 */

	/* FIXME: Add initial value to pass Assert() */
	VciGuc.table_scan_policy = VCI_TABLE_SCAN_POLICY_COLUMN_ONLY;

	for (int i = 0; i < (int) lengthof(VciConfigureNames); i++)
	{
		struct config_generic *conf = &VciConfigureNames[i];

		if (conf->vartype == PGC_BOOL)
		{
			if (IsPostmasterEnvironment)
				DefineCustomBoolVariable(conf->name,
										 conf->short_desc,
										 conf->long_desc,
										 conf->_bool.variable,
										 conf->_bool.boot_val,
										 conf->context,
										 conf->flags,
										 conf->_bool.check_hook,
										 conf->_bool.assign_hook,
										 conf->_bool.show_hook);
			else
				*(conf->_bool.variable) = conf->_bool.boot_val;
		}

		else if (conf->vartype == PGC_INT)
		{
			if (IsPostmasterEnvironment)
				DefineCustomIntVariable(conf->name,
										conf->short_desc,
										conf->long_desc,
										conf->_int.variable,
										conf->_int.boot_val,
										conf->_int.min,
										conf->_int.max,
										conf->context,
										conf->flags,
										conf->_int.check_hook,
										conf->_int.assign_hook,
										conf->_int.show_hook);
			else
				*(conf->_int.variable) = conf->_int.boot_val;
		}

		else if (conf->vartype == PGC_ENUM)
		{
			if (IsPostmasterEnvironment)
				DefineCustomEnumVariable(conf->name,
										 conf->short_desc,
										 conf->long_desc,
										 conf->_enum.variable,
										 conf->_enum.boot_val,
										 conf->_enum.options,
										 conf->context,
										 conf->flags,
										 conf->_enum.check_hook,
										 conf->_enum.assign_hook,
										 conf->_enum.show_hook);
			else
				*(conf->_enum.variable) = conf->_enum.boot_val;
		}

		else
			elog(ERROR, "Unexpected VCI GUC variable type");
	}

	VciGuc.have_loaded_postgresql_conf = true;

	check_max_worker_processes();
}

/*
 * Check for max_worker_processes
 */
static void
check_max_worker_processes(void)
{
	int			num_needed_workers;

	num_needed_workers = 1 + VciGuc.control_max_workers;	/* ros control daemon &
															 * workers */
	num_needed_workers += 1;	/* parallel control daemon  */

	if (num_needed_workers > MAX_BACKENDS)
		num_needed_workers = MAX_BACKENDS;

	if (max_worker_processes < num_needed_workers)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg(VCI_STRING " needs to set at least %d to \"max_worker_processes\"",
						num_needed_workers)));
}

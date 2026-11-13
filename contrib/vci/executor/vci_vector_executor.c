/*-------------------------------------------------------------------------
 *
 * vci_vector_executor.c
 *	  Routines to build and evaluate vector processing object
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_vector_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "commands/typecmds.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "executor/nodeSubplan.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "optimizer/planner.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/xml.h"

#include "vci.h"
#include "vci_executor.h"
#include "vci_utils.h"

/* Private Structure to Vector processing */
typedef struct FuncExprinfo
{
	FmgrInfo   *finfo;
	FunctionCallInfo fcinfo_data;	/* arguments etc */
	PGFunction	fn_addr;		/* actual call address */
	int			nargs;			/* number of arguments */
	List	   *args;			/* states of argument expressions */
	Oid			funcid;
	Oid			inputcollid;
} FuncExprinfo;

/*
 * VciScalarArrayOpExprHashEntry
 * 		Hash table entry type used during VciVPExecHashedScalarArrayOpExpr
 *      Copied from OSS ScalarArrayOpExprHashEntry
 */
typedef struct VciScalarArrayOpExprHashEntry
{
	Datum		key;
	uint32		status;			/* hash status */
	uint32		hash;			/* hash value (cached) */
} VciScalarArrayOpExprHashEntry;

#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE VciScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_SCOPE static inline
#define SH_DECLARE
#include "lib/simplehash.h"

static bool saop_hash_element_match(struct saophash_hash *tb, Datum key1,
									Datum key2);
static uint32 saop_element_hash(struct saophash_hash *tb, Datum key);

/*
 * VciScalarArrayOpExprHashTable
 *		Hash table for VciVPExecHashedScalarArrayOpExpr
 *      Copied from OSS ScalarArrayOpExprHashTable
 */
typedef struct VciScalarArrayOpExprHashTable
{
	saophash_hash *hashtab;		/* underlying hash table */
	struct VciVPNode *pnode;
} VciScalarArrayOpExprHashTable;

/* Define parameters for ScalarArrayOpExpr hash table code generation. */
#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE VciScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_KEY key
#define SH_HASH_KEY(tb, key) saop_element_hash(tb, key)
#define SH_EQUAL(tb, a, b) saop_hash_element_match(tb, a, b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"

static void VciVPExecFunc(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecFunc_arg0(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecFunc_arg1(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecFunc_arg2(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecDistinctExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecNullIfExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecScalarArrayOpExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecHashedScalarArrayOpExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecNullTest(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecBooleanTest(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecNot(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecAnd_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecAnd_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecAnd_nullasfalse_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecOr_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecOr_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecMinMax_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecMinMax_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCoalesce_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCoalesce_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCase_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCase_arg(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCase_cond(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCase_result(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCaseTest(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecParamExec(Expr *expression, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecCoerceViaIO(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecVar(Expr *expression, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void VciVPExecConst(Expr *expression, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
static void vci_vp_exec_simple_copy(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots);

static VciVPContext *vci_create_vp_context(void);
static vci_vp_item_id vci_add_vp_node(VciVPExecOp_func func, Expr *expr, VciVPContext *vpcontext, int len_args, vci_vp_item_id *arg_items, bool allocValueAndIsNull, uint16 *skip_list);
static vci_vp_item_id vci_add_var_node(Var *variable, PlanState *parent, VciVPContext *vpcontext, uint16 *skip_list);
static vci_vp_item_id vci_add_param_node(Param *param, PlanState *parent, VciVPContext *vpcontext, uint16 *skip_list);
static vci_vp_item_id vci_add_const_node(Const *con, VciVPContext *vpcontext, uint16 *skip_list);
static vci_vp_item_id vci_add_control_nodes(VciVPExecOp_func head_func, VciVPExecOp_func next_func, List *args, Expr *expr, PlanState *parent, ExprContext *econtext, VciVPContext *vpcontext, uint16 *skip_list);
static vci_vp_item_id traverse_expr_state_tree(Expr *node, PlanState *parent, ExprContext *econtext, VciVPContext *vpcontext, uint16 *skip_list);

static void VciVPExecInitFunc(Expr *node, List *args, Oid funcid, Oid inputcollid, PlanState *parent, FuncExprinfo *funcinfo);
static vci_vp_item_id vci_add_func_expr_node(Expr *expr, VciVPContext *vpcontext, FuncExprinfo *funcinfo, PlanState *parent, ExprContext *econtext, uint16 *skip_list);
static Datum VciExecEvalParamExec_vp(VciVPNode *vpnode, ExprContext *econtext, bool *isNull);

/*****************************************************************************
 * Vector processing execution function
 *****************************************************************************/

/**
 * Execute vector processing
 *
 * @param[in,out] vpcontext
 * @param[in]     econtext
 * @param[in]     max_slots
 */
void
VciExecEvalVectorProcessing(VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	vci_vp_item_id max;

	max = vpcontext->num_item;

	for (vci_vp_item_id i = 1; i < max; i++)
	{
		VciVPNode  *vpnode = &vpcontext->itemNode[i];

		vpnode->evalfunc(vpnode->expr, vpnode, vpcontext, econtext, max_slots);
	}
}

static void
VciVPExecFunc(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = vpnode->data.func.fcinfo_data;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		int			i;
		Datum		value = (Datum) 0;
		bool		isnull = true;

		for (i = 0; i < vpnode->len_args; i++)
		{
			vci_vp_item_id item = vpnode->arg_items[i];
			VciVPNode  *arg_node = &vpcontext->itemNode[item];

			fcinfo->args[i].value = arg_node->itemValue[slot_index];
			fcinfo->args[i].isnull = arg_node->itemIsNull[slot_index];
		}

		if (vpnode->data.func.finfo->fn_strict)
		{
			while (--i >= 0)
			{
				if (fcinfo->args[i].isnull)
				{
					goto done;
				}
			}
		}

		pgstat_init_function_usage(fcinfo, &fcusage);

		fcinfo->isnull = false;
		value = FunctionCallInvoke(fcinfo);
		isnull = fcinfo->isnull;

		pgstat_end_function_usage(&fcusage, true);

done:
		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecFunc_arg0(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;

	/* PgStat_FunctionCallUsage fcusage; */

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = vpnode->data.func.fcinfo_data;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value = (Datum) 0;
		bool		isnull = true;

		/* pgstat_init_function_usage(fcinfo, &fcusage); */

		fcinfo->isnull = false;
		value = FunctionCallInvoke(fcinfo);
		isnull = fcinfo->isnull;

		/* pgstat_end_function_usage(&fcusage, true); */

		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecFunc_arg1(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;

	/* PgStat_FunctionCallUsage fcusage; */

	VciVPNode  *arg_node;

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = vpnode->data.func.fcinfo_data;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value = (Datum) 0;
		bool		isnull = true;

		fcinfo->args[0].value = arg_node->itemValue[slot_index];
		fcinfo->args[0].isnull = arg_node->itemIsNull[slot_index];

		if (vpnode->data.func.finfo->fn_strict)
			if (fcinfo->args[0].isnull)
				goto done;

		/* pgstat_init_function_usage(fcinfo, &fcusage); */

		fcinfo->isnull = false;
		value = FunctionCallInvoke(fcinfo);
		isnull = fcinfo->isnull;

		/* pgstat_end_function_usage(&fcusage, true); */

done:
		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecFunc_arg2(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;

	/* PgStat_FunctionCallUsage fcusage; */

	VciVPNode  *arg_node0,
			   *arg_node1;

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = vpnode->data.func.fcinfo_data;

	arg_node0 = &vpcontext->itemNode[vpnode->arg_items[0]];
	arg_node1 = &vpcontext->itemNode[vpnode->arg_items[1]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value = (Datum) 0;
		bool		isnull = true;

		fcinfo->args[0].value = arg_node0->itemValue[slot_index];
		fcinfo->args[0].isnull = arg_node0->itemIsNull[slot_index];

		fcinfo->args[1].value = arg_node1->itemValue[slot_index];
		fcinfo->args[1].isnull = arg_node1->itemIsNull[slot_index];

		if (vpnode->data.func.finfo->fn_strict)
			if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
				goto done;

		/* pgstat_init_function_usage(fcinfo, &fcusage); */

		fcinfo->isnull = false;
		value = FunctionCallInvoke(fcinfo);
		isnull = fcinfo->isnull;

		/* pgstat_end_function_usage(&fcusage, true); */

done:
		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecDistinctExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = vpnode->data.func.fcinfo_data;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value = (Datum) 0;
		bool		isnull = false;

		Assert(vpnode->len_args == 2);

		for (int i = 0; i < 2; i++)
		{
			vci_vp_item_id item = vpnode->arg_items[i];
			VciVPNode  *arg_node = &vpcontext->itemNode[item];

			fcinfo->args[i].value = arg_node->itemValue[slot_index];
			fcinfo->args[i].isnull = arg_node->itemIsNull[slot_index];
		}

		if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
		{
			/* Both NULL? Then is not distinct... */
			value = BoolGetDatum(false);
		}
		else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
		{
			/* Only one is NULL? Then is distinct... */
			value = BoolGetDatum(true);
		}
		else
		{
			fcinfo->isnull = false;
			value = FunctionCallInvoke(fcinfo);
			isnull = fcinfo->isnull;
			/* Must invert result of "=" */
			value = BoolGetDatum(!DatumGetBool(value));
		}

		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecNullIfExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = vpnode->data.func.fcinfo_data;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value = (Datum) 0;
		bool		isnull = false;

		Assert(vpnode->len_args == 2);

		for (int i = 0; i < 2; i++)
		{
			vci_vp_item_id item = vpnode->arg_items[i];
			VciVPNode  *arg_node = &vpcontext->itemNode[item];

			fcinfo->args[i].value = arg_node->itemValue[slot_index];
			fcinfo->args[i].isnull = arg_node->itemIsNull[slot_index];
		}

		/* if either argument is NULL they can't be equal */
		if (!fcinfo->args[0].isnull && !fcinfo->args[1].isnull)
		{
			fcinfo->isnull = false;
			value = FunctionCallInvoke(fcinfo);
			/* if the arguments are equal return null */
			if (!fcinfo->isnull && DatumGetBool(value))
			{
				value = (Datum) 0;
				isnull = true;
				goto equal_two_arguments;
			}
		}

		value = fcinfo->args[0].value;
		isnull = fcinfo->args[0].isnull;

equal_two_arguments:
		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecScalarArrayOpExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	bool		useOr = vpnode->data.scalararrayop.useOr;
	FunctionCallInfo fcinfo;

	fcinfo = vpnode->data.scalararrayop.fcinfo_data;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		ArrayType  *arr;
		int			nitems;
		Datum		result;
		bool		resultnull;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		char	   *s;
		bits8	   *bitmap;
		int			bitmask;

		result = (Datum) 0;
		resultnull = false;		/* Set default values for result flags:
								 * non-null, not a set result */

		for (int i = 0; i < 2; i++)
		{
			vci_vp_item_id item = vpnode->arg_items[i];
			VciVPNode  *arg_node = &vpcontext->itemNode[item];

			fcinfo->args[i].value = arg_node->itemValue[slot_index];
			fcinfo->args[i].isnull = arg_node->itemIsNull[slot_index];
		}

		/*
		 * If the array is NULL then we return NULL --- it's not very
		 * meaningful to do anything else, even if the operator isn't strict.
		 */
		if (fcinfo->args[1].isnull)
		{
			result = (Datum) 0;
			resultnull = true;
			goto done;
		}

		/* Else okay to fetch and detoast the array */
		arr = DatumGetArrayTypeP(fcinfo->args[1].value);

		/*
		 * If the array is empty, we return either FALSE or TRUE per the useOr
		 * flag.  This is correct even if the scalar is NULL; since we would
		 * evaluate the operator zero times, it matters not whether it would
		 * want to return NULL.
		 */
		nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
		if (nitems <= 0)
		{
			result = BoolGetDatum(!useOr);
			goto done;
		}

		/*
		 * If the scalar is NULL, and the function is strict, return NULL; no
		 * point in iterating the loop.
		 */
		if (fcinfo->args[0].isnull && vpnode->data.scalararrayop.finfo->fn_strict)
		{
			result = (Datum) 0;
			resultnull = true;
			goto done;
		}

		/*
		 * We arrange to look up info about the element type only once per
		 * series of calls, assuming the element type doesn't change
		 * underneath us.
		 */
		if (vpnode->data.scalararrayop.element_type != ARR_ELEMTYPE(arr))
		{
			get_typlenbyvalalign(ARR_ELEMTYPE(arr),
								 &vpnode->data.scalararrayop.typlen,
								 &vpnode->data.scalararrayop.typbyval,
								 &vpnode->data.scalararrayop.typalign);
			vpnode->data.scalararrayop.element_type = ARR_ELEMTYPE(arr);
		}
		typlen = vpnode->data.scalararrayop.typlen;
		typbyval = vpnode->data.scalararrayop.typbyval;
		typalign = vpnode->data.scalararrayop.typalign;

		result = BoolGetDatum(!useOr);
		resultnull = false;

		/* Loop over the array elements */
		s = (char *) ARR_DATA_PTR(arr);
		bitmap = ARR_NULLBITMAP(arr);
		bitmask = 1;

		for (int i = 0; i < nitems; i++)
		{
			Datum		elt;
			Datum		thisresult;

			/* Get array element, checking for NULL */
			if (bitmap && (*bitmap & bitmask) == 0)
			{
				fcinfo->args[1].value = (Datum) 0;
				fcinfo->args[1].isnull = true;
			}
			else
			{
				elt = fetch_att(s, typbyval, typlen);
				s = att_addlength_pointer(s, typlen, s);
				s = (char *) att_align_nominal(s, typalign);
				fcinfo->args[1].value = elt;
				fcinfo->args[1].isnull = false;
			}

			/* Call comparison function */
			if (fcinfo->args[1].isnull && vpnode->data.scalararrayop.finfo->fn_strict)
			{
				fcinfo->isnull = true;
				thisresult = (Datum) 0;
			}
			else
			{
				fcinfo->isnull = false;
				thisresult = FunctionCallInvoke(fcinfo);
			}

			/* Combine results per OR or AND semantics */
			if (fcinfo->isnull)
				resultnull = true;
			else if (useOr)
			{
				if (DatumGetBool(thisresult))
				{
					result = BoolGetDatum(true);
					resultnull = false;
					break;		/* needn't look at any more elements */
				}
			}
			else
			{
				if (!DatumGetBool(thisresult))
				{
					result = BoolGetDatum(false);
					resultnull = false;
					break;		/* needn't look at any more elements */
				}
			}

			/* advance bitmap pointer if any */
			if (bitmap)
			{
				bitmask <<= 1;
				if (bitmask == 0x100)
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

done:
		itemValue[slot_index] = result;
		itemIsNull[slot_index] = resultnull;
	}
}

/*
 * Hash function for scalar array hash op elements.
 *
 * We use the element type's default hash opclass, and the column collation
 * if the type is collation-sensitive.
 */
static uint32
saop_element_hash(struct saophash_hash *tb, Datum key)
{
	VciScalarArrayOpExprHashTable *elements_tab = (VciScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = elements_tab->pnode->data.hashedscalararrayop.fcinfo_data;
	Datum		hash;

	fcinfo->args[0].value = key;
	fcinfo->args[0].isnull = false;

	hash = elements_tab->pnode->data.hashedscalararrayop.hash_fn_addr(fcinfo);

	return DatumGetUInt32(hash);
}

/*
 * Matching function for scalar array hash op elements, to be used in hashtable
 * lookups.
 */
static bool
saop_hash_element_match(struct saophash_hash *tb, Datum key1, Datum key2)
{
	Datum		result;

	VciScalarArrayOpExprHashTable *elements_tab = (VciScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = elements_tab->pnode->data.hashedscalararrayop.fcinfo_data;

	fcinfo->args[0].value = key1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = key2;
	fcinfo->args[1].isnull = false;

	result = elements_tab->pnode->data.hashedscalararrayop.fn_addr(fcinfo);

	return DatumGetBool(result);
}

static void
VciVPExecHashedScalarArrayOpExpr(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	FunctionCallInfo fcinfo;
	bool		strictfunc;
	Datum		scalar;
	bool		scalar_isnull;
	VciScalarArrayOpExprHashTable *elements_tab;

	fcinfo = vpnode->data.hashedscalararrayop.fcinfo_data;
	strictfunc = vpnode->data.hashedscalararrayop.finfo->fn_strict;
	elements_tab = vpnode->data.hashedscalararrayop.elements_tab;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		result;
		bool		resultnull;
		bool		hashfound;

		/* We don't setup a hashed scalar array op if the array const is null. */
		Assert(!fcinfo->args[1].isnull);

		for (int i = 0; i < 2; i++)
		{
			vci_vp_item_id item = vpnode->arg_items[i];
			VciVPNode  *arg_node = &vpcontext->itemNode[item];

			fcinfo->args[i].value = arg_node->itemValue[slot_index];
			fcinfo->args[i].isnull = arg_node->itemIsNull[slot_index];
		}
		scalar = fcinfo->args[0].value;
		scalar_isnull = fcinfo->args[0].isnull;

		/*
		 * If the scalar is NULL, and the function is strict, return NULL; no
		 * point in executing the search.
		 */
		if (fcinfo->args[0].isnull && strictfunc)
		{
			result = (Datum) 0;
			resultnull = true;
			goto done;
		}

		/* Build the hash table on first evaluation */
		if (elements_tab == NULL)
		{
			int16		typlen;
			bool		typbyval;
			char		typalign;
			int			nitems;
			bool		has_nulls = false;
			char	   *s;
			bits8	   *bitmap;
			int			bitmask;
			MemoryContext oldcontext;
			ArrayType  *arr;

			arr = DatumGetArrayTypeP(fcinfo->args[1].value);
			nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

			get_typlenbyvalalign(ARR_ELEMTYPE(arr),
								 &typlen,
								 &typbyval,
								 &typalign);

			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

			elements_tab =
				palloc_object(VciScalarArrayOpExprHashTable);
			vpnode->data.hashedscalararrayop.elements_tab = elements_tab;
			elements_tab->pnode = vpnode;

			/*
			 * Create the hash table sizing it according to the number of
			 * elements in the array.  This does assume that the array has no
			 * duplicates. If the array happens to contain many duplicate
			 * values then it'll just mean that we sized the table a bit on
			 * the large side.
			 */
			elements_tab->hashtab = saophash_create(CurrentMemoryContext, nitems,
													elements_tab);

			MemoryContextSwitchTo(oldcontext);

			s = (char *) ARR_DATA_PTR(arr);
			bitmap = ARR_NULLBITMAP(arr);
			bitmask = 1;
			for (int i = 0; i < nitems; i++)
			{
				/* Get array element, checking for NULL. */
				if (bitmap && (*bitmap & bitmask) == 0)
				{
					has_nulls = true;
				}
				else
				{
					Datum		element;

					element = fetch_att(s, typbyval, typlen);
					s = att_addlength_pointer(s, typlen, s);
					s = (char *) att_align_nominal(s, typalign);

					saophash_insert(elements_tab->hashtab, element, &hashfound);
				}

				/* Advance bitmap pointer if any. */
				if (bitmap)
				{
					bitmask <<= 1;
					if (bitmask == 0x100)
					{
						bitmap++;
						bitmask = 1;
					}
				}
			}

			/*
			 * Remember if we had any nulls so that we know if we need to
			 * execute non-strict functions with a null lhs value if no match
			 * is found.
			 */
			vpnode->data.hashedscalararrayop.has_nulls = has_nulls;
		}

		/* Check the hash to see if we have a match. */
		hashfound = NULL != saophash_lookup(elements_tab->hashtab, scalar);

		result = BoolGetDatum(hashfound);
		resultnull = false;

		/*
		 * If we didn't find a match in the array, we still might need to
		 * handle the possibility of null values.  We didn't put any NULLs
		 * into the hashtable, but instead marked if we found any when
		 * building the table in has_nulls.
		 */
		if (!DatumGetBool(result) && vpnode->data.hashedscalararrayop.has_nulls)
		{
			if (strictfunc)
			{

				/*
				 * We have nulls in the array so a non-null lhs and no match
				 * must yield NULL.
				 */
				result = (Datum) 0;
				resultnull = true;
			}
			else
			{
				/*
				 * Execute function will null rhs just once.
				 *
				 * The hash lookup path will have scribbled on the lhs
				 * argument so we need to set it up also (even though we
				 * entered this function with it already set).
				 */
				fcinfo->args[0].value = scalar;
				fcinfo->args[0].isnull = scalar_isnull;
				fcinfo->args[1].value = (Datum) 0;
				fcinfo->args[1].isnull = true;

				result = FunctionCallInvoke(fcinfo);
				resultnull = fcinfo->isnull;
			}
		}

done:
		itemValue[slot_index] = result;
		itemIsNull[slot_index] = resultnull;
	}
}

static void
VciVPExecNullTest(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	NullTest   *ntest = (NullTest *) expr;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		Assert(!ntest->argisrow);

		/* Simple scalar-argument case, or a null rowtype datum */
		switch (ntest->nulltesttype)
		{
			case IS_NULL:
				if (isnull)
				{
					value = BoolGetDatum(true);
					isnull = false;
				}
				else
					value = BoolGetDatum(false);
				break;

			case IS_NOT_NULL:
				if (isnull)
				{
					value = BoolGetDatum(false);
					isnull = false;
				}
				else
					value = BoolGetDatum(true);
				break;

			default:
				/* LCOV_EXCL_START */
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) ntest->nulltesttype);
				break;
				/* LCOV_EXCL_STOP */
		}

		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecBooleanTest(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	BooleanTest *btest = (BooleanTest *) expr;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		switch (btest->booltesttype)
		{
			case IS_TRUE:
				if (isnull)
				{
					value = BoolGetDatum(false);
					isnull = false;
				}
				else if (DatumGetBool(value))
					value = BoolGetDatum(true);
				else
					value = BoolGetDatum(false);
				break;

			case IS_NOT_TRUE:
				if (isnull)
				{
					value = BoolGetDatum(true);
					isnull = false;
				}
				else if (DatumGetBool(value))
					value = BoolGetDatum(false);
				else
					value = BoolGetDatum(true);
				break;

			case IS_FALSE:
				if (isnull)
				{
					value = BoolGetDatum(false);
					isnull = false;
				}
				else if (DatumGetBool(value))
					value = BoolGetDatum(false);
				else
					value = BoolGetDatum(true);
				break;

			case IS_NOT_FALSE:
				if (isnull)
				{
					value = BoolGetDatum(true);
					isnull = false;
				}
				else if (DatumGetBool(value))
					value = BoolGetDatum(true);
				else
					value = BoolGetDatum(false);
				break;

			case IS_UNKNOWN:
				if (isnull)
				{
					value = BoolGetDatum(true);
					isnull = false;
				}
				else
					value = BoolGetDatum(false);
				break;

			case IS_NOT_UNKNOWN:
				if (isnull)
				{
					value = BoolGetDatum(false);
					isnull = false;
				}
				else
					value = BoolGetDatum(true);
				break;

			default:
				/* LCOV_EXCL_START */
				elog(ERROR, "unrecognized booltesttype: %d",
					 (int) btest->booltesttype);
				break;
				/* LCOV_EXCL_STOP */
		}

		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecNot(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull)
			value = (Datum) 0;
		else
			value = BoolGetDatum(!DatumGetBool(value));

		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecAnd_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	for (int i = 0; i < VCI_MAX_FETCHING_ROWS; i++)
		vpnode->itemValue[i] = BoolGetDatum(true);

	memset(vpnode->itemIsNull, 0, sizeof(bool) * VCI_MAX_FETCHING_ROWS);
	memcpy(vpnode->skip_list, vpnode->data.init.orig_skip_list, sizeof(uint16) * VCI_MAX_SKIP_LIST_SLOTS);
}

static void
VciVPExecAnd_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	int			check_slot_index = 0;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull)
		{
			itemValue[slot_index] = (Datum) 0;
			itemIsNull[slot_index] = true;

			check_slot_index = slot_index + 1;
		}
		else if (!DatumGetBool(value))
		{
			itemValue[slot_index] = BoolGetDatum(false);
			itemIsNull[slot_index] = false;

			skip_list[check_slot_index] += skip_list[slot_index + 1] + 1;
		}
		else
		{
			check_slot_index = slot_index + 1;
		}
	}
}

static void
VciVPExecAnd_nullasfalse_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	int			check_slot_index = 0;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull || !DatumGetBool(value))
		{
			itemValue[slot_index] = BoolGetDatum(false);
			itemIsNull[slot_index] = false;

			skip_list[check_slot_index] += skip_list[slot_index + 1] + 1;
		}
		else
		{
			check_slot_index = slot_index + 1;
		}
	}
}

static void
VciVPExecOr_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	for (int i = 0; i < VCI_MAX_FETCHING_ROWS; i++)
		vpnode->itemValue[i] = BoolGetDatum(false);

	memset(vpnode->itemIsNull, 0, sizeof(bool) * VCI_MAX_FETCHING_ROWS);
	memcpy(vpnode->skip_list, vpnode->data.init.orig_skip_list, sizeof(uint16) * VCI_MAX_SKIP_LIST_SLOTS);
}

static void
VciVPExecOr_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	int			check_slot_index = 0;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull)
		{
			itemValue[slot_index] = (Datum) 0;
			itemIsNull[slot_index] = true;

			check_slot_index = slot_index + 1;
		}
		else if (DatumGetBool(value))
		{
			itemValue[slot_index] = BoolGetDatum(true);
			itemIsNull[slot_index] = false;

			skip_list[check_slot_index] += skip_list[slot_index + 1] + 1;
		}
		else
		{
			check_slot_index = slot_index + 1;
		}
	}
}

static void
VciVPExecMinMax_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	for (int i = 0; i < VCI_MAX_FETCHING_ROWS; i++)
		vpnode->itemIsNull[i] = true;

	memcpy(vpnode->skip_list, vpnode->data.init.orig_skip_list, sizeof(uint16) * VCI_MAX_SKIP_LIST_SLOTS);
}

static void
VciVPExecMinMax_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	VciVPNode  *arg_node;
	FmgrInfo   *finfo;
	TypeCacheEntry *typentry;

	MinMaxExpr *minmax = (MinMaxExpr *) expr;
	Oid			collation = minmax->inputcollid;
	MinMaxOp	op = minmax->op;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	finfo = palloc0_object(FmgrInfo);	/* will be freed as part of query
										 * context free */

	/* Look up the btree comparison function for the datatype */
	typentry = lookup_type_cache(minmax->minmaxtype,
								 TYPECACHE_CMP_PROC);

	if (!OidIsValid(typentry->cmp_proc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify a comparison function for type %s",
						format_type_be(minmax->minmaxtype))));

	fmgr_info(typentry->cmp_proc, finfo);
	fmgr_info_set_expr((Node *) expr, finfo);

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		LOCAL_FCINFO(locfcinfo, 2);
		int32		cmpresult;

		InitFunctionCallInfoData(*locfcinfo, finfo, 2,
								 collation, NULL, NULL);
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].isnull = false;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull)
		{
		}
		else if (itemIsNull[slot_index] == true)
		{
			/* first nonnull input, adopt value */
			itemValue[slot_index] = value;
			itemIsNull[slot_index] = false;
		}
		else
		{
			/* apply comparison function */
			locfcinfo->args[0].value = itemValue[slot_index];
			locfcinfo->args[1].value = value;
			locfcinfo->isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));
			if (cmpresult > 0 && op == IS_LEAST)
				itemValue[slot_index] = value;
			else if (cmpresult < 0 && op == IS_GREATEST)
				itemValue[slot_index] = value;
		}
	}
}

static void
VciVPExecCoalesce_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	for (int i = 0; i < VCI_MAX_FETCHING_ROWS; i++)
	{
		vpnode->itemValue[i] = (Datum) 0;
		vpnode->itemIsNull[i] = true;
	}

	memcpy(vpnode->skip_list, vpnode->data.init.orig_skip_list, sizeof(uint16) * VCI_MAX_SKIP_LIST_SLOTS);
}

static void
VciVPExecCoalesce_next(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	int			check_slot_index = 0;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull)
		{
			itemValue[slot_index] = (Datum) 0;
			itemIsNull[slot_index] = true;

			check_slot_index = slot_index + 1;
		}
		else
		{
			itemValue[slot_index] = value;
			itemIsNull[slot_index] = false;

			skip_list[check_slot_index] += skip_list[slot_index + 1] + 1;
		}
	}
}

static void
VciVPExecCase_head(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	for (int i = 0; i < VCI_MAX_FETCHING_ROWS; i++)
	{
		vpnode->itemValue[i] = (Datum) 0;
		vpnode->itemIsNull[i] = true;
	}

	memcpy(vpnode->skip_list, vpnode->data.init.orig_skip_list, sizeof(uint16) * VCI_MAX_SKIP_LIST_SLOTS);
}

static void
VciVPExecCase_arg(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	vci_vp_exec_simple_copy(expr, vpnode, vpcontext, econtext, max_slots);
}

static void
VciVPExecCase_cond(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list0 = vpnode->data.init.orig_skip_list;
	uint16	   *skip_list1 = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	int			check_slot_index0 = 0;
	int			check_slot_index1 = 0;
	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	memcpy(skip_list1, skip_list0, sizeof(uint16) * VCI_MAX_SKIP_LIST_SLOTS);

	for (int slot_index0 = skip_list0[0],
		 slot_index1 = skip_list1[0];
		 slot_index0 < max_slots;
		 slot_index0 += skip_list0[slot_index0 + 1] + 1,
		 slot_index1 += skip_list1[slot_index1 + 1] + 1)
	{
		Datum		clause_value;
		bool		isnull;

		clause_value = arg_node->itemValue[slot_index0];
		isnull = arg_node->itemIsNull[slot_index0];

		if (DatumGetBool(clause_value) && !isnull)
		{
			itemValue[slot_index0] = arg_node->itemValue[slot_index0];
			itemIsNull[slot_index0] = arg_node->itemIsNull[slot_index0];

			skip_list0[check_slot_index0] += skip_list0[slot_index0 + 1] + 1;
			check_slot_index1 = slot_index1 + 1;
		}
		else
		{
			check_slot_index0 = slot_index0 + 1;
			skip_list1[check_slot_index1] += skip_list1[slot_index1 + 1] + 1;
		}
	}
}

static void
VciVPExecCase_result(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	vci_vp_exec_simple_copy(expr, vpnode, vpcontext, econtext, max_slots);
}

static void
VciVPExecCaseTest(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	/* Do nothging */
}

static void
VciVPExecParamExec(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	bool		first_eval_exec = false;
	Datum		paramValue;
	bool		paramIsNull;

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{

		if (!first_eval_exec)
		{
			paramValue = VciExecEvalParamExec_vp(vpnode, econtext, &paramIsNull);
			first_eval_exec = true;
		}

		itemValue[slot_index] = paramValue;
		itemIsNull[slot_index] = paramIsNull;
	}
}

static void
VciVPExecCoerceViaIO(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;
	VciVPNode  *arg_node;

	FmgrInfo   *outfunc = vpnode->data.iocoerce.finfo_out;	/* lookup info for
															 * source output
															 * function */
	FmgrInfo   *infunc = vpnode->data.iocoerce.finfo_in;	/* lookup info for
															 * result input function */
	Oid			typioparam = vpnode->data.iocoerce.typioparam;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		char	   *string;
		Datum		value;
		bool		isnull;

		value = arg_node->itemValue[slot_index];
		isnull = arg_node->itemIsNull[slot_index];

		if (isnull)
			string = NULL;
		else
			string = OutputFunctionCall(outfunc, value);

		value = InputFunctionCall(infunc,
								  string,
								  typioparam,
								  -1);

		itemValue[slot_index] = value;
		itemIsNull[slot_index] = isnull;
	}
}

static void
VciVPExecVar(Expr *expression, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	/* Do nothging */
}

static void
VciVPExecConst(Expr *expression, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	/* Do nothging */
}

static void
vci_vp_exec_simple_copy(Expr *expr, VciVPNode *vpnode, VciVPContext *vpcontext, ExprContext *econtext, int max_slots)
{
	uint16	   *skip_list = vpnode->skip_list;
	Datum	   *itemValue = vpnode->itemValue;
	bool	   *itemIsNull = vpnode->itemIsNull;

	VciVPNode  *arg_node;

	arg_node = &vpcontext->itemNode[vpnode->arg_items[0]];

	for (int slot_index = skip_list[0];
		 slot_index < max_slots;
		 slot_index += skip_list[slot_index + 1] + 1)
	{
		itemValue[slot_index] = arg_node->itemValue[slot_index];
		itemIsNull[slot_index] = arg_node->itemIsNull[slot_index];
	}
}

/*****************************************************************************
 * Vector processing setting function
 *****************************************************************************/

VciVPContext *
VciBuildVectorProcessing(Expr *node, PlanState *parent, ExprContext *econtext, uint16 *skip_list)
{
	VciVPContext *vpcontext;
	VciVPNode  *lastNode;

	if (node == NULL)
		return NULL;

	vpcontext = vci_create_vp_context();

	traverse_expr_state_tree(node, parent, econtext, vpcontext, skip_list);

	lastNode = &vpcontext->itemNode[vpcontext->num_item - 1];

	vpcontext->resultValue = lastNode->itemValue;
	vpcontext->resultIsNull = lastNode->itemIsNull;

	return vpcontext;
}

static VciVPContext *
vci_create_vp_context(void)
{
	const vci_vp_item_id max = 16;
	VciVPContext *vpcontext;

	vpcontext = palloc0_object(VciVPContext);

	vpcontext->num_item = 1;
	vpcontext->max_item = max;
	vpcontext->itemNode = palloc0_array(VciVPNode, max);

	return vpcontext;
}

static vci_vp_item_id
vci_add_vp_node(VciVPExecOp_func evalfunc, Expr *expr, VciVPContext *vpcontext, int len_args, vci_vp_item_id *arg_items, bool allocValueAndIsNull, uint16 *skip_list)
{
	vci_vp_item_id item;
	VciVPNode  *vpnode;

	item = vpcontext->num_item;

	if (vpcontext->num_item + 1 >= vpcontext->max_item)
	{
		VciVPNode  *oldnodes = vpcontext->itemNode;
		VciVPNode  *newnodes = palloc0_array(VciVPNode, vpcontext->max_item * 2);

		for (vci_vp_item_id j = 1; j < vpcontext->max_item; j++)
			newnodes[j] = oldnodes[j];

		vpcontext->max_item *= 2;
		vpcontext->itemNode = newnodes;

		pfree(oldnodes);
	}

	vpnode = &vpcontext->itemNode[item];

	vpnode->evalfunc = evalfunc;
	vpnode->expr = expr;
	vpnode->len_args = len_args;

	if (len_args > 0)
	{
		int			i;

		vpnode->arg_items = palloc_array(vci_vp_item_id, len_args);

		for (i = 0; i < len_args; i++)
			vpnode->arg_items[i] = arg_items[i];
	}

	if (allocValueAndIsNull)
	{
		vpnode->itemValue = palloc_array(Datum, VCI_MAX_FETCHING_ROWS);
		vpnode->itemIsNull = palloc_array(bool, VCI_MAX_FETCHING_ROWS);
	}

	vpnode->skip_list = skip_list;

	vpcontext->num_item++;

	return item;
}

static vci_vp_item_id
vci_add_var_node(Var *variable, PlanState *parent, VciVPContext *vpcontext, uint16 *skip_list)
{
	vci_vp_item_id ret;
	VciVPNode  *vpnode;

	VciScanState *scanstate = vci_search_scan_state((VciPlanState *) parent);

	ret = vci_add_vp_node(VciVPExecVar, (Expr *) variable, vpcontext, 0, NULL, false, skip_list);

	vpnode = &vpcontext->itemNode[ret];

	if (variable->varno == OUTER_VAR)
	{
		vpnode->itemValue = scanstate->result_values[variable->varattno - 1];
		vpnode->itemIsNull = scanstate->result_isnull[variable->varattno - 1];
	}
	else
	{
		int			index;

		index = scanstate->attr_map[variable->varattno] - 1;

		Assert(index >= 0);
		Assert(index < scanstate->vector_set->num_columns);

		vpnode->itemValue = vci_CSGetValueAddrFromVirtualTuplesColumnwise(scanstate->vector_set, index);
		vpnode->itemIsNull = vci_CSGetIsNullAddrFromVirtualTuplesColumnwise(scanstate->vector_set, index);
	}

	return ret;
}
static vci_vp_item_id
vci_add_param_node(Param *param, PlanState *parent, VciVPContext *vpcontext, uint16 *skip_list)
{
	vci_vp_item_id ret;
	VciVPNode  *vpnode;

	ret = vci_add_vp_node((VciVPExecOp_func) VciVPExecParamExec, (Expr *) param, vpcontext, 0, NULL, true, skip_list);

	vpnode = &vpcontext->itemNode[ret];

	vpnode->data.param.paramid = param->paramid;
	vpnode->data.param.paramtype = param->paramtype;
	vpnode->data.param.vci_parent_plan = parent->plan;

	return ret;
}
static vci_vp_item_id
vci_add_const_node(Const *con, VciVPContext *vpcontext, uint16 *skip_list)
{
	vci_vp_item_id ret;
	VciVPNode  *vpnode;
	Datum	   *itemValue;
	bool	   *itemIsNull;

	ret = vci_add_vp_node(VciVPExecConst, (Expr *) con, vpcontext, 0, NULL, true, skip_list);

	vpnode = &vpcontext->itemNode[ret];

	itemValue = vpnode->itemValue;
	itemIsNull = vpnode->itemIsNull;

	for (int i = 0; i < VCI_MAX_FETCHING_ROWS; i++)
	{
		itemValue[i] = con->constvalue;
		itemIsNull[i] = con->constisnull;
	}

	return ret;
}
static vci_vp_item_id
vci_add_func_expr_node(Expr *expr, VciVPContext *vpcontext, FuncExprinfo *funcinfo, PlanState *parent, ExprContext *econtext, uint16 *skip_list)
{

	vci_vp_item_id result;
	int			i;
	int			len_args = 0;
	vci_vp_item_id *arg_items;
	ListCell   *l;

	len_args = list_length(funcinfo->args);
	if (len_args > 0)
		arg_items = palloc_array(vci_vp_item_id, len_args);
	else
		arg_items = NULL;

	i = 0;
	foreach(l, funcinfo->args)
	{
		Expr	   *arg = (Expr *) lfirst(l);

		arg_items[i] = traverse_expr_state_tree(arg, parent, econtext, vpcontext, skip_list);
		i++;
	}

	/*
	 * pgstat_init_function_usage()
	 */

	if (pgstat_track_functions <= funcinfo->fcinfo_data->flinfo->fn_stats)
	{
		switch (list_length(funcinfo->args))
		{
			case 0:
				result = vci_add_vp_node((VciVPExecOp_func) VciVPExecFunc_arg0,
										 expr, vpcontext, len_args, arg_items, true, skip_list);
				goto func_expr_state_done;

			case 1:
				result = vci_add_vp_node((VciVPExecOp_func) VciVPExecFunc_arg1,
										 expr, vpcontext, len_args, arg_items, true, skip_list);
				goto func_expr_state_done;

			case 2:
				result = vci_add_vp_node((VciVPExecOp_func) VciVPExecFunc_arg2,
										 expr, vpcontext, len_args, arg_items, true, skip_list);
				goto func_expr_state_done;

			default:
				break;
		}
	}

	result = vci_add_vp_node((VciVPExecOp_func) VciVPExecFunc,
							 expr, vpcontext, len_args, arg_items, true, skip_list);

func_expr_state_done:
	{
		VciVPNode  *vpnode;

		vpnode = &vpcontext->itemNode[result];
		vpnode->data.func.finfo = funcinfo->finfo;
		vpnode->data.func.fcinfo_data = funcinfo->fcinfo_data;
		vpnode->data.func.fn_addr = funcinfo->fn_addr;
		vpnode->data.func.nargs = funcinfo->nargs;

		if (arg_items)
			pfree(arg_items);
	}
	return result;
}
static vci_vp_item_id
vci_add_control_nodes(VciVPExecOp_func head_func, VciVPExecOp_func next_func, List *args,
					  Expr *expr, PlanState *parent, ExprContext *econtext, VciVPContext *vpcontext, uint16 *skip_list)
{
	vci_vp_item_id ret;
	ListCell   *l;
	Datum	   *itemValue = palloc_array(Datum, VCI_MAX_FETCHING_ROWS);
	bool	   *itemIsNull = palloc_array(bool, VCI_MAX_FETCHING_ROWS);
	uint16	   *inner_skip_list = palloc_array(uint16, VCI_MAX_SKIP_LIST_SLOTS);
	VciVPNode  *head_node;

	ret = vci_add_vp_node(head_func, expr, vpcontext, 0, NULL, false, inner_skip_list);

	head_node = &vpcontext->itemNode[ret];
	head_node->itemValue = itemValue;
	head_node->itemIsNull = itemIsNull;
	head_node->data.init.orig_skip_list = skip_list;

	foreach(l, args)
	{
		Expr	   *arg = (Expr *) lfirst(l);
		vci_vp_item_id next_item;
		VciVPNode  *next_node;

		next_item = traverse_expr_state_tree(arg, parent, econtext, vpcontext, inner_skip_list);
		ret = vci_add_vp_node(next_func, expr, vpcontext, 1, &next_item, false, inner_skip_list);

		next_node = &vpcontext->itemNode[ret];
		next_node->itemValue = itemValue;
		next_node->itemIsNull = itemIsNull;
	}

	return ret;
}

static vci_vp_item_id
traverse_expr_state_tree(Expr *node, PlanState *parent, ExprContext *econtext, VciVPContext *vpcontext, uint16 *skip_list)
{

	if (node == NULL)
		return 0;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	if (IsA(node, List))
	{
		int			num_args = list_length((List *) node);

		if (num_args > 1)
			return vci_add_control_nodes(VciVPExecAnd_head, VciVPExecAnd_nullasfalse_next, (List *) node,
										 node, parent, econtext, vpcontext, skip_list);
		else if (num_args == 1)
			return traverse_expr_state_tree(linitial((List *) node), parent, econtext, vpcontext, skip_list);
	}

	switch (nodeTag(node))
	{
		case T_Var:

			/*
			 * Assert(state->evalfunc == (ExprStateEvalFunc)
			 * VciExecEvalScalarVarFromColumnStore);
			 *
			 * If execinitexpr for qual is decided to be not needed, then this
			 * assertion also becomes invalid
			 */

			return vci_add_var_node((Var *) node, parent, vpcontext, skip_list);

		case T_Const:
			return vci_add_const_node((Const *) node, vpcontext, skip_list);

		case T_Param:
			return vci_add_param_node((Param *) node, parent, vpcontext, skip_list);

			/*
			 * return vci_add_vp_node((VciVPExecOp_func) VciVPExecParamExec,
			 * node, vpcontext, 0, NULL, true, skip_list);
			 */

		case T_Aggref:
			/* LCOV_EXCL_START */
			elog(ERROR, "Aggref should not be targeted by vector processing");
			node = NULL;
			break;
			/* LCOV_EXCL_STOP */

		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) node;
				FuncExprinfo *funcinfo = palloc0_object(struct FuncExprinfo);	/* will be freed as part
																				 * of query context free */

				VciVPExecInitFunc(node, op->args, op->opfuncid, op->inputcollid, parent, funcinfo);
				return vci_add_func_expr_node(node, vpcontext, funcinfo, parent, econtext, skip_list);
			}

		case T_FuncExpr:
			{
				FuncExpr   *func = (FuncExpr *) node;
				FuncExprinfo *funcinfo = palloc0_object(struct FuncExprinfo);	/* will be freed as part
																				 * of query context free */

				VciVPExecInitFunc(node, func->args, func->funcid, func->inputcollid, parent, funcinfo);
				return vci_add_func_expr_node(node, vpcontext, funcinfo, parent, econtext, skip_list);
			}

		case T_DistinctExpr:
			{
				DistinctExpr *op = (DistinctExpr *) node;
				FuncExprinfo *funcinfo = palloc0_object(struct FuncExprinfo);	/* will be freed as part
																				 * of query context free */
				vci_vp_item_id result;
				VciVPNode  *vpnode;

				vci_vp_item_id arg_items[2];

				/*
				 * Not required as this was the value always set earlier in
				 * execinitexpr stage
				 */

				VciVPExecInitFunc(node, op->args, op->opfuncid, op->inputcollid, parent, funcinfo);

				Assert(list_length(funcinfo->args) == 2);

				arg_items[0] = traverse_expr_state_tree(list_nth(funcinfo->args, 0), parent, econtext, vpcontext, skip_list);
				arg_items[1] = traverse_expr_state_tree(list_nth(funcinfo->args, 1), parent, econtext, vpcontext, skip_list);

				result = vci_add_vp_node((VciVPExecOp_func) VciVPExecDistinctExpr,
										 node, vpcontext, 2, arg_items, true, skip_list);

				vpnode = &vpcontext->itemNode[result];
				vpnode->data.func.finfo = funcinfo->finfo;
				vpnode->data.func.fcinfo_data = funcinfo->fcinfo_data;
				vpnode->data.func.fn_addr = funcinfo->fn_addr;
				vpnode->data.func.nargs = funcinfo->nargs;

				return result;
			}

		case T_NullIfExpr:
			{
				NullIfExpr *op = (NullIfExpr *) node;
				vci_vp_item_id arg_items[2];
				FuncExprinfo *funcinfo = palloc0_object(struct FuncExprinfo);	/* will be freed as part
																				 * of query context free */
				vci_vp_item_id result;
				VciVPNode  *vpnode;

				/*
				 * Not required as this was the value always set earlier in
				 * execinitexpr stage
				 */
				VciVPExecInitFunc(node, op->args, op->opfuncid, op->inputcollid, parent, funcinfo);

				Assert(list_length(funcinfo->args) == 2);

				arg_items[0] = traverse_expr_state_tree(list_nth(funcinfo->args, 0), parent, econtext, vpcontext, skip_list);
				arg_items[1] = traverse_expr_state_tree(list_nth(funcinfo->args, 1), parent, econtext, vpcontext, skip_list);

				result = vci_add_vp_node((VciVPExecOp_func) VciVPExecNullIfExpr,
										 node, vpcontext, 2, arg_items, true, skip_list);

				vpnode = &vpcontext->itemNode[result];
				vpnode->data.func.finfo = funcinfo->finfo;
				vpnode->data.func.fcinfo_data = funcinfo->fcinfo_data;
				vpnode->data.func.fn_addr = funcinfo->fn_addr;
				vpnode->data.func.nargs = funcinfo->nargs;

				return result;
			}

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) node;
				vci_vp_item_id arg_items[2];
				FuncExprinfo *funcinfo = palloc0_object(struct FuncExprinfo);	/* will be freed as part
																				 * of query context free */
				vci_vp_item_id result;
				VciVPNode  *vpnode;

				/*
				 * Not required as this was the value always set earlier in
				 * execinitexpr stage
				 */
				VciVPExecInitFunc(node, op->args, op->opfuncid, op->inputcollid, parent, funcinfo);

				Assert(list_length(funcinfo->args) == 2);

				arg_items[0] = traverse_expr_state_tree(list_nth(funcinfo->args, 0), parent, econtext, vpcontext, skip_list);
				arg_items[1] = traverse_expr_state_tree(list_nth(funcinfo->args, 1), parent, econtext, vpcontext, skip_list);

				if (OidIsValid(op->hashfuncid))
				{
					FmgrInfo   *hash_finfo = palloc0_object(FmgrInfo);
					FunctionCallInfo hash_fcinfo = palloc0(SizeForFunctionCallInfo(1));

					fmgr_info(op->hashfuncid, hash_finfo);
					fmgr_info_set_expr((Node *) node, hash_finfo);
					InitFunctionCallInfoData(*hash_fcinfo, hash_finfo,
											 1, op->inputcollid, NULL,
											 NULL);

					result = vci_add_vp_node((VciVPExecOp_func) VciVPExecHashedScalarArrayOpExpr,
											 node, vpcontext, 2, arg_items, true, skip_list);

					vpnode = &vpcontext->itemNode[result];
					vpnode->data.hashedscalararrayop.finfo = funcinfo->finfo;
					vpnode->data.hashedscalararrayop.fcinfo_data = funcinfo->fcinfo_data;
					vpnode->data.hashedscalararrayop.fn_addr = funcinfo->fn_addr;

					vpnode->data.hashedscalararrayop.hash_finfo = funcinfo->finfo;
					vpnode->data.hashedscalararrayop.hash_fcinfo_data = funcinfo->fcinfo_data;
					vpnode->data.hashedscalararrayop.hash_fn_addr = funcinfo->fn_addr;
				}
				else
				{
					result = vci_add_vp_node((VciVPExecOp_func) VciVPExecScalarArrayOpExpr,
											 node, vpcontext, 2, arg_items, true, skip_list);

					vpnode = &vpcontext->itemNode[result];
					vpnode->data.scalararrayop.element_type = InvalidOid;
					vpnode->data.scalararrayop.useOr = op->useOr;
					vpnode->data.scalararrayop.finfo = funcinfo->finfo;
					vpnode->data.scalararrayop.fcinfo_data = funcinfo->fcinfo_data;
					vpnode->data.scalararrayop.fn_addr = funcinfo->fn_addr;
				}

				return result;
			}

		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;

				return traverse_expr_state_tree(relabel->arg, parent, econtext, vpcontext, skip_list);
			}
		case T_NullTest:
			{
				vci_vp_item_id ret;
				NullTest   *ntest = (NullTest *) node;

				ret = traverse_expr_state_tree(ntest->arg, parent, econtext, vpcontext, skip_list);
				return vci_add_vp_node((VciVPExecOp_func) VciVPExecNullTest,
									   node, vpcontext, 1, &ret, true, skip_list);
			}

		case T_BooleanTest:
			{
				BooleanTest *booltest = (BooleanTest *) node;
				vci_vp_item_id ret;

				ret = traverse_expr_state_tree(booltest->arg, parent, econtext, vpcontext, skip_list);
				return vci_add_vp_node((VciVPExecOp_func) VciVPExecBooleanTest,
									   node, vpcontext, 1, &ret, true, skip_list);
			}

		case T_BoolExpr:
			{
				BoolExpr   *boolexpr = (BoolExpr *) node;
				vci_vp_item_id arg_item;

				switch (boolexpr->boolop)
				{
					case AND_EXPR:
						return vci_add_control_nodes(VciVPExecAnd_head, VciVPExecAnd_next, boolexpr->args,
													 node, parent, econtext, vpcontext, skip_list);
						break;

					case OR_EXPR:
						return vci_add_control_nodes(VciVPExecOr_head, VciVPExecOr_next, boolexpr->args,
													 node, parent, econtext, vpcontext, skip_list);
						break;

					case NOT_EXPR:
						arg_item = traverse_expr_state_tree((Expr *) linitial(boolexpr->args), parent, econtext, vpcontext, skip_list);
						return vci_add_vp_node((VciVPExecOp_func) VciVPExecNot,
											   node, vpcontext, 1, &arg_item, true, skip_list);

					default:
						/* LCOV_EXCL_START */
						elog(ERROR, "unrecognized boolop: %d",
							 (int) boolexpr->boolop);
						break;
						/* LCOV_EXCL_STOP */
				}
			}
			break;

		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;

				return vci_add_control_nodes(VciVPExecMinMax_head, VciVPExecMinMax_next, minmaxexpr->args,
											 node, parent, econtext, vpcontext, skip_list);
			}

		case T_CoalesceExpr:
			{
				CoalesceExpr *cexpr = (CoalesceExpr *) node;

				return vci_add_control_nodes(VciVPExecCoalesce_head, VciVPExecCoalesce_next, cexpr->args,
											 node, parent, econtext, vpcontext, skip_list);
			}

		case T_CoerceViaIO:
			{
				CoerceViaIO *coerceViaIOexpr = (CoerceViaIO *) node;
				vci_vp_item_id ret;
				VciVPNode  *vpnode;
				Oid			iofunc;
				Oid			typioparam;
				bool		typisvarlena;
				FmgrInfo   *finfo_out = palloc0_object(struct FmgrInfo);	/* will be freed as part
																			 * of query context free */
				FmgrInfo   *finfo_in = palloc0_object(struct FmgrInfo);		/* will be freed as part
																			 * of query context free */

				ret = traverse_expr_state_tree(coerceViaIOexpr->arg, parent, econtext, vpcontext, skip_list);

				ret = vci_add_vp_node((VciVPExecOp_func) VciVPExecCoerceViaIO,
									  node, vpcontext, 1, &ret, true, skip_list);
				getTypeOutputInfo(exprType((Node *) coerceViaIOexpr->arg),
								  &iofunc, &typisvarlena);
				fmgr_info(iofunc, finfo_out);
				fmgr_info_set_expr((Node *) node, finfo_out);

				getTypeInputInfo(coerceViaIOexpr->resulttype,
								 &iofunc, &typioparam);
				fmgr_info(iofunc, finfo_in);
				fmgr_info_set_expr((Node *) node, finfo_in);

				vpnode = &vpcontext->itemNode[ret];
				vpnode->data.iocoerce.finfo_out = finfo_out;
				vpnode->data.iocoerce.finfo_in = finfo_in;
				vpnode->data.iocoerce.typioparam = typioparam;

				return ret;
			}

		case T_CaseExpr:
			{
				CaseExpr   *caseExpr = (CaseExpr *) node;
				vci_vp_item_id head,
							ret = 0,
							save_caseValue;
				ListCell   *lc;
				Datum	   *itemValue = palloc_array(Datum, VCI_MAX_FETCHING_ROWS);
				bool	   *itemIsNull = palloc_array(bool, VCI_MAX_FETCHING_ROWS);
				Datum	   *caseValue = palloc_array(Datum, VCI_MAX_FETCHING_ROWS);
				bool	   *caseIsNull = palloc_array(bool, VCI_MAX_FETCHING_ROWS);
				uint16	   *case_whole_skip_list = palloc_array(uint16, VCI_MAX_SKIP_LIST_SLOTS);
				VciVPNode  *arg_node;

				head = vci_add_vp_node(VciVPExecCase_head, node, vpcontext, 0, NULL, false, case_whole_skip_list);

				arg_node = &vpcontext->itemNode[head];
				arg_node->itemValue = caseValue;
				arg_node->itemIsNull = caseIsNull;
				arg_node->data.init.orig_skip_list = skip_list;

				save_caseValue = vpcontext->caseValue;
				vpcontext->caseValue = head;

				if (caseExpr->arg)
				{
					vci_vp_item_id arg_item;

					arg_item = traverse_expr_state_tree(caseExpr->arg, parent, econtext, vpcontext, case_whole_skip_list);
					ret = vci_add_vp_node(VciVPExecCase_arg, node, vpcontext, 1, &arg_item, false, case_whole_skip_list);
					arg_node = &vpcontext->itemNode[ret];
					arg_node->itemValue = caseValue;
					arg_node->itemIsNull = caseIsNull;
				}

				foreach(lc, caseExpr->args)
				{
					CaseWhen   *when = lfirst(lc);
					vci_vp_item_id arg_item;
					uint16	   *each_case_skip_list = palloc_array(uint16, VCI_MAX_SKIP_LIST_SLOTS);

					/* WHEN evaluation */

					arg_item = traverse_expr_state_tree(when->expr, parent, econtext, vpcontext, case_whole_skip_list);
					ret = vci_add_vp_node(VciVPExecCase_cond, node, vpcontext, 1, &arg_item, false, each_case_skip_list);

					arg_node = &vpcontext->itemNode[ret];
					arg_node->itemValue = caseValue;
					arg_node->itemIsNull = caseIsNull;
					arg_node->data.init.orig_skip_list = case_whole_skip_list;

					vpcontext->caseValue = save_caseValue;

					/* THEN evaluation */

					arg_item = traverse_expr_state_tree(when->result, parent, econtext, vpcontext, each_case_skip_list);
					ret = vci_add_vp_node(VciVPExecCase_result, node, vpcontext, 1, &arg_item, false, each_case_skip_list);

					arg_node = &vpcontext->itemNode[ret];
					arg_node->itemValue = itemValue;
					arg_node->itemIsNull = itemIsNull;

					save_caseValue = vpcontext->caseValue;
					vpcontext->caseValue = head;
				}

				vpcontext->caseValue = save_caseValue;

				if (caseExpr->defresult)
				{
					/* ELSE evaluation */
					vci_vp_item_id arg_item;
					vci_vp_item_id save_caseValue_defresult;

					save_caseValue_defresult = vpcontext->caseValue;

					arg_item = traverse_expr_state_tree(caseExpr->defresult, parent, econtext, vpcontext, case_whole_skip_list);
					ret = vci_add_vp_node(VciVPExecCase_result, node, vpcontext, 1, &arg_item, false, case_whole_skip_list);

					arg_node = &vpcontext->itemNode[ret];
					arg_node->itemValue = itemValue;
					arg_node->itemIsNull = itemIsNull;

					vpcontext->caseValue = save_caseValue_defresult;
				}

				Assert(ret > 0);

				return ret;
			}

		case T_CaseTestExpr:
			{
				vci_vp_item_id ret;
				VciVPNode  *arg_node,
						   *caseValue_node;

				ret = vci_add_vp_node(VciVPExecCaseTest, node, vpcontext, 0, NULL, false, skip_list);

				caseValue_node = &vpcontext->itemNode[vpcontext->caseValue];

				arg_node = &vpcontext->itemNode[ret];
				arg_node->itemValue = caseValue_node->itemValue;
				arg_node->itemIsNull = caseValue_node->itemIsNull;

				return ret;
			}

		case T_List:
		case T_TargetEntry:
			/* LCOV_EXCL_START */
			Assert(0);
			break;
			/* LCOV_EXCL_STOP */

		default:
			/* LCOV_EXCL_START */
			elog(ERROR, "unrecognized node type: %s(%d)",
				 VciGetNodeName(nodeTag(node)), (int) nodeTag(node));
			node = NULL;		/* keep compiler quiet */
			break;
			/* LCOV_EXCL_STOP */
	}

	Assert(0);
	return 0;
}
static
void
VciVPExecInitFunc(Expr *node, List *args, Oid funcid, Oid inputcollid, PlanState *parent, FuncExprinfo *funcinfo)
{
	int			nargs = list_length(args);
	AclResult	aclresult;
	FmgrInfo   *flinfo;
	FunctionCallInfo fcinfo;

	/* Check permission to call function */
	aclresult = object_aclcheck(ProcedureRelationId, funcid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);

	/*
	 * Safety check on nargs.  Under normal circumstances this should never
	 * fail, as parser should check sooner.  But possibly it might fail if
	 * server has been compiled with FUNC_MAX_ARGS smaller than some functions
	 * declared in pg_proc?
	 */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* Allocate function lookup data and parameter workspace for this call */
	funcinfo->finfo = palloc0_object(FmgrInfo);
	funcinfo->fcinfo_data = palloc0(SizeForFunctionCallInfo(nargs));
	flinfo = funcinfo->finfo;
	fcinfo = funcinfo->fcinfo_data;

	/* Set up the primary fmgr lookup information */
	fmgr_info(funcid, flinfo);
	fmgr_info_set_expr((Node *) node, flinfo);

	/* Initialize function call parameter structure too */
	InitFunctionCallInfoData(*fcinfo, flinfo,
							 nargs, inputcollid, NULL, NULL);

	/* Keep extra copies of this info to save an indirection at runtime */
	funcinfo->fn_addr = flinfo->fn_addr;
	funcinfo->nargs = nargs;

	funcinfo->args = args;
	funcinfo->funcid = funcid;
	funcinfo->inputcollid = inputcollid;

	Assert(!flinfo->fn_retset);

}

Datum
VciExecEvalParamExec_vp(VciVPNode *vpnode, ExprContext *econtext,
						bool *isNull)
{
	int			thisParamId = vpnode->data.param.paramid;
	ParamExecData *prm;

	/*
	 * PARAM_EXEC params (internal executor parameters) are stored in the
	 * ecxt_param_exec_vals array, and can be accessed by array index.
	 */
	prm = &(econtext->ecxt_param_exec_vals[thisParamId]);

	if (prm->execPlan != NULL)
	{
		/* Parameter not evaluated yet, so go do it */
		ExecSetParamPlan(prm->execPlan, econtext);
		/* ExecSetParamPlan should have processed this param... */
		Assert(prm->execPlan == NULL);
	}

	*isNull = prm->isnull;
	return prm->value;
}

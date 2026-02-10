/*-------------------------------------------------------------------------
 *
 * vci_executor.c
 *	  Miscellaneous executor utility routines
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "access/xact.h"		/* for XactEvent */
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/typecmds.h"
#include "executor/execdebug.h"
#include "executor/execExpr.h"
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
#include "utils/jsonfuncs.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/xml.h"

#include "vci.h"

#include "vci_executor.h"
#include "vci_utils.h"

/**
 * Record QueryDesc executing VCI on Executor
 *
 * - NULL on no execution, and records QueryDesc when executing VCI on ExecutorStart hook
 * - Only 1 VCI running query runs at a time (VCI does not run multiple queries in parallel)
 * - Return to NULL when VCI ends at ExecutorEnd
 * - In case of transaction error, force return to NULL using vci_xact_callback callback
 * - In case of error in subtransaction, determine if it is applicable using SubTransactionId
 *
 * @note There are patterns in which Executor is recursively called, such as when stored procedure is called
 * @note When FETCH-ing a DECLARE CURSOR, multiple Executor of queries are called in parallel.
 */
static QueryDesc *vci_execution_query_desc = NULL;
static SubTransactionId vci_execution_subid = InvalidSubTransactionId;

/**
 * Record the first call of vci_executor_run_routine() of VCI execution
 *
 * - When using cursor, ExecutorRun() may be called multiple times for a query,
 *   but this is used to limit the setup process to the first time.
 * - Even if stored procedure calls executor at multiple stages, it does not change
 *   at stages unrelated to VCI execution.
 */
static bool vci_executor_run_routine_once = false;

/* static function decls */
static bool should_fetch_column_store(Var *var, PlanState *parent);

static void vci_executor_start_routine(QueryDesc *queryDesc, int eflags);
static void vci_executor_run_routine(QueryDesc *queryDesc, ScanDirection direction, uint64 count);
static void vci_executor_end_routine(QueryDesc *queryDesc);
static void vci_explain_one_query_routine(Query *queryDesc, int cursorOptions, IntoClause *into,
										  ExplainState *es, const char *queryString, ParamListInfo params,
										  QueryEnvironment *queryEnv);

/* Static variables */
static ExecutorStart_hook_type executor_start_prev;
static ExecutorRun_hook_type executor_run_prev;
static ExecutorEnd_hook_type executor_end_prev;
static ExplainOneQuery_hook_type explain_one_query_prev;

static void VciExecInitExprRec(Expr *node, PlanState *parent, ExprState *state, Datum *resv, bool *resnull, vci_initexpr_t inittype);
static void VciExecInitFunc(ExprEvalStep *scratch, Expr *node, List *args, Oid funcid, Oid inputcollid, PlanState *parent, ExprState *state, vci_initexpr_t inittype);
static void VciExecInitJsonExpr(JsonExpr *jsexpr, PlanState *parent, ExprState *state,
								Datum *resv, bool *resnull,
								ExprEvalStep *scratch, vci_initexpr_t inittype);
static void VciExecInitJsonCoercion(ExprState *state, JsonReturning *returning,
									ErrorSaveContext *escontext, bool omit_quotes,
									bool exists_coerce,
									Datum *resv, bool *resnull);

/**
 * Registration of VCI's executor routine
 */
void
vci_setup_executor_hook(void)
{
	executor_start_prev = ExecutorStart_hook;
	ExecutorStart_hook = vci_executor_start_routine;

	executor_run_prev = ExecutorRun_hook;
	ExecutorRun_hook = vci_executor_run_routine;

	executor_end_prev = ExecutorEnd_hook;
	ExecutorEnd_hook = vci_executor_end_routine;

	explain_one_query_prev = ExplainOneQuery_hook;
	ExplainOneQuery_hook = vci_explain_one_query_routine;

	ExprEvalVar_hook = VciExecEvalScalarVarFromColumnStore;
	ExprEvalParam_hook = VciExecEvalParamExec;

}

/**
 * ExecutorStart hook callback
 */
static void
vci_executor_start_routine(QueryDesc *queryDesc, int eflags)
{
	SubTransactionId mySubid;

	if (IsParallelWorker())
		goto end;

	mySubid = GetCurrentSubTransactionId();

	if (vci_execution_query_desc == NULL)
	{
		/* Start plan rewrite only if no other Executor is running */
		vci_initialize_query_context(queryDesc, eflags);

		if (vci_is_processing_custom_plan())
		{
			vci_execution_query_desc = queryDesc;
			vci_execution_subid = mySubid;
			vci_executor_run_routine_once = false;
		}
	}

end:
	if (executor_start_prev)
		executor_start_prev(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/**
 * ExecutorRun hook callback
 */
static void
vci_executor_run_routine(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	if (IsParallelWorker())
		goto end;

end:
	if (executor_run_prev)
		executor_run_prev(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
}

/**
 * ExecutorEnd hook callback
 */
static void
vci_executor_end_routine(QueryDesc *queryDesc)
{
	if (executor_end_prev)
		executor_end_prev(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	if (IsParallelWorker())
		return;

	if (vci_execution_query_desc == queryDesc)
	{
		vci_finalize_query_context();

		/*
		 * vci_free_query_context call is moved inside
		 * vci_finalize_query_context , otherwise this call will not delete
		 * SMC created for parallelism
		 */
		/* vci_free_query_context(); */

		vci_execution_query_desc = NULL;
		vci_execution_subid = InvalidSubTransactionId;
		vci_executor_run_routine_once = false;
	}
}

static void
vci_explain_one_query_routine(Query *queryDesc, int cursorOptions, IntoClause *into,
							  ExplainState *es, const char *queryString, ParamListInfo params,
							  QueryEnvironment *queryEnv)
{
	if (explain_one_query_prev)
		explain_one_query_prev(queryDesc, cursorOptions, into, es, queryString, params, queryEnv);
	else
	{
		/*
		 * copy from ExplainOneQuery() in src/backend/commands/explain.c
		 */
		standard_ExplainOneQuery(queryDesc, cursorOptions, into, es,
								 queryString, params, queryEnv);
	}
}

/**
 * Stop VCI execute at transaction switch time
 */
void
vci_xact_change_handler(XactEvent event)
{
	switch (event)
	{
		case XACT_EVENT_ABORT:
			if (vci_execution_query_desc != NULL)
			{
				elog(DEBUG1, "vci:executor caught any exception");
				vci_free_query_context();
			}
			vci_execution_query_desc = NULL;
			vci_execution_subid = InvalidSubTransactionId;
			vci_executor_run_routine_once = false;
			break;

		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_COMMIT:
			Assert(vci_execution_query_desc == NULL);
			break;

		default:
			/**
			 * XACT_EVENT_PREPARE
			 * XACT_EVENT_PRE_PREPARE
			 */
			break;
	}
}

/**
 * Event Handler on subxact change.
 */
void
vci_subxact_change_handler(SubXactEvent event, SubTransactionId mySubid)
{
	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
			break;

		case SUBXACT_EVENT_ABORT_SUB:
			if (mySubid == vci_execution_subid)
			{
				elog(DEBUG1, "vci:executor caught any exception in sub transaction");
				vci_free_query_context();

				vci_execution_query_desc = NULL;
				vci_execution_subid = InvalidSubTransactionId;
				vci_executor_run_routine_once = false;
			}
			break;

		case SUBXACT_EVENT_PRE_COMMIT_SUB:
		case SUBXACT_EVENT_COMMIT_SUB:
			break;
	}
}

/**
 * Determine whether Var fetches from column store
 */
static bool
should_fetch_column_store(Var *var, PlanState *planstate)
{
	Assert(var != NULL);
	Assert(planstate != NULL);
	Assert(nodeTag(planstate) != T_Invalid);

	if (IsA(planstate, CustomScanState))
	{
		CustomScanState *cps = (CustomScanState *) planstate;
		uint32		plan_type = cps->flags & VCI_CUSTOMPLAN_MASK;

		if ((plan_type == VCI_CUSTOMPLAN_SCAN) ||
			(plan_type == VCI_CUSTOMPLAN_SORT) ||
			(plan_type == VCI_CUSTOMPLAN_AGG))
		{
			return true;
		}
	}

	return false;
}

/* ----------------------------------------------------------------
 *		ExecEvalOper / ExecEvalFunc support routines
 * ----------------------------------------------------------------
 */

/*
 * VciExecInitExprRec
 * Append the steps necessary for the evaluation of node to ExprState->steps,
 * possibly recursing into sub-expressions of node.
 *
 * node - expression to evaluate
 * parent - parent executor node (or NULL if a standalone expression)
 * state - ExprState to whose ->steps to append the necessary operations
 * resv / resnull - where to store the result of the node into
 * copied from src/backend/executor/execExpr.c
 */
static void
VciExecInitExprRec(Expr *node, PlanState *parent, ExprState *state,
				   Datum *resv, bool *resnull, vci_initexpr_t inittype)
{
	ExprEvalStep scratch = {0};

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	/* Step's output location is always what the caller gave us */
	Assert(resv != NULL && resnull != NULL);
	scratch.resvalue = resv;
	scratch.resnull = resnull;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *variable = (Var *) node;

				Assert(((Var *) node)->varattno != InvalidAttrNumber);

				if ((inittype == VCI_INIT_EXPR_FETCHING_COLUMN_STORE) &&
					should_fetch_column_store((Var *) node, parent))
				{
					/*
					 * CustomScanState *cstate; VciScanState   *vci_scanstate;
					 * cstate = (CustomScanState *) parent;
					 *
					 * Assert(IsA(cstate, CustomScanState)); vci_scanstate	=
					 * vci_search_scan_state((VciPlanState *) parent);
					 * scratch.opcode = EEOP_VCI_VAR; scratch.d.vci_scanstate
					 * = vci_scanstate;
					 *
					 * This is to make use of OSS structure ExprEvalStep
					 * rathen then copying it in VCI again for additional
					 * information on var and param nodes. Searching for
					 * underlying scan state is postponed to
					 * vciExecEvalScalarVarFromColumnStore()
					 */
					scratch.opcode = EEOP_VCI_VAR;
					scratch.d.var.vci_parent_planstate = parent;

				}
				else if (variable->varattno <= 0)
				{
					/* system column */
					scratch.d.var.attnum = variable->varattno;
					scratch.d.var.vartype = variable->vartype;
					scratch.d.var.varreturningtype = variable->varreturningtype;
					switch (variable->varno)
					{
						case INNER_VAR:
							scratch.opcode = EEOP_INNER_SYSVAR;
							break;
						case OUTER_VAR:
							scratch.opcode = EEOP_OUTER_SYSVAR;
							break;

							/* INDEX_VAR is handled by default case */

						default:
							switch (variable->varreturningtype)
							{
								case VAR_RETURNING_DEFAULT:
									scratch.opcode = EEOP_SCAN_SYSVAR;
									break;
								case VAR_RETURNING_OLD:
									scratch.opcode = EEOP_OLD_SYSVAR;
									state->flags |= EEO_FLAG_HAS_OLD;
									break;
								case VAR_RETURNING_NEW:
									scratch.opcode = EEOP_NEW_SYSVAR;
									state->flags |= EEO_FLAG_HAS_NEW;
									break;
							}
							break;
					}
				}
				else
				{
					/* regular user column */
					scratch.d.var.attnum = variable->varattno - 1;
					scratch.d.var.vartype = variable->vartype;
					scratch.d.var.varreturningtype = variable->varreturningtype;
					/* select EEOP_*_FIRST opcode to force one-time checks */
					switch (variable->varno)
					{
						case INNER_VAR:
							scratch.opcode = EEOP_INNER_VAR;
							break;
						case OUTER_VAR:
							scratch.opcode = EEOP_OUTER_VAR;
							break;

							/* INDEX_VAR is handled by default case */

						default:
							switch (variable->varreturningtype)
							{
								case VAR_RETURNING_DEFAULT:
									scratch.opcode = EEOP_SCAN_VAR;
									break;
								case VAR_RETURNING_OLD:
									scratch.opcode = EEOP_OLD_VAR;
									state->flags |= EEO_FLAG_HAS_OLD;
									break;
								case VAR_RETURNING_NEW:
									scratch.opcode = EEOP_NEW_VAR;
									state->flags |= EEO_FLAG_HAS_NEW;
									break;
							}
							break;
					}
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}
		case T_Const:
			{
				Const	   *con = (Const *) node;

				scratch.opcode = EEOP_CONST;
				scratch.d.constval.value = con->constvalue;
				scratch.d.constval.isnull = con->constisnull;

				ExprEvalPushStep(state, &scratch);
				break;
			}
		case T_Param:
			{
				Param	   *param = (Param *) node;

				Assert(param->paramkind == PARAM_EXEC);
				scratch.d.param.vci_parent_plan = parent->plan;
				scratch.opcode = EEOP_VCI_PARAM_EXEC;
				scratch.d.param.paramid = param->paramid;
				scratch.d.param.paramtype = param->paramtype;

				ExprEvalPushStep(state, &scratch);
				break;
			}
		case T_CaseTestExpr:

			/*
			 * Read from location identified by innermost_caseval.  Note that
			 * innermost_caseval could be NULL, if this node isn't actually
			 * within a CASE structure; some parts of the system abuse
			 * CaseTestExpr to cause a read of a value externally supplied in
			 * econtext->caseValue_datum.  We'll take care of that by
			 * generating a specialized operation.
			 */
			if (state->innermost_caseval == NULL)
				scratch.opcode = EEOP_CASE_TESTVAL_EXT;
			else
			{
				scratch.opcode = EEOP_CASE_TESTVAL;
				scratch.d.casetest.value = state->innermost_caseval;
				scratch.d.casetest.isnull = state->innermost_casenull;
			}
			ExprEvalPushStep(state, &scratch);
			break;

		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				scratch.opcode = EEOP_AGGREF;
				scratch.d.aggref.aggno = aggref->aggno;

				if (parent && IsA(parent, CustomScanState))
				{
					VciAggState *aggstate = (VciAggState *) parent;

					aggstate->aggs = lappend(aggstate->aggs, aggref);
				}
				else
				{
					/* planner messed up */
					elog(ERROR, "Aggref found in non-Agg plan node");
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;

		case T_MergeSupportFunc:
			{
				/* must be in a MERGE, else something messed up */
				if (!state->parent ||
					!IsA(state->parent, ModifyTableState) ||
					((ModifyTableState *) state->parent)->operation != CMD_MERGE)
					elog(ERROR, "MergeSupportFunc found in non-merge plan node");
				scratch.opcode = EEOP_MERGE_SUPPORT_FUNC;
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_FuncExpr:
			{
				FuncExpr   *func = (FuncExpr *) node;

				VciExecInitFunc(&scratch, node,
								func->args, func->funcid, func->inputcollid,
								parent, state, inittype);
				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) node;

				VciExecInitFunc(&scratch, node,
								op->args, op->opfuncid, op->inputcollid,
								parent, state, inittype);
				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		case T_DistinctExpr:
			{
				DistinctExpr *op = (DistinctExpr *) node;

				VciExecInitFunc(&scratch, node,
								op->args, op->opfuncid, op->inputcollid,
								parent, state, inittype);

				/*
				 * Change opcode of call instruction to EEOP_DISTINCT.
				 *
				 * XXX: historically we've not called the function usage
				 * pgstat infrastructure - that seems inconsistent given that
				 * we do so for normal function *and* operator evaluation.  If
				 * we decided to do that here, we'd probably want separate
				 * opcodes for FUSAGE or not.
				 */
				scratch.opcode = EEOP_DISTINCT;
				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *op = (NullIfExpr *) node;

				VciExecInitFunc(&scratch, node,
								op->args, op->opfuncid, op->inputcollid,
								parent, state, inittype);

				/*
				 * If first argument is of varlena type, we'll need to ensure
				 * that the value passed to the comparison function is a
				 * read-only pointer.
				 */
				scratch.d.func.make_ro =
					(get_typlen(exprType((Node *) linitial(op->args))) == -1);

				/*
				 * Change opcode of call instruction to EEOP_NULLIF.
				 *
				 * XXX: historically we've not called the function usage
				 * pgstat infrastructure - that seems inconsistent given that
				 * we do so for normal function *and* operator evaluation.  If
				 * we decided to do that here, we'd probably want separate
				 * opcodes for FUSAGE or not.
				 */
				scratch.opcode = EEOP_NULLIF;
				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;
				Expr	   *scalararg;
				Expr	   *arrayarg;
				FmgrInfo   *finfo;
				FunctionCallInfo fcinfo;
				AclResult	aclresult;

				Assert(list_length(opexpr->args) == 2);
				scalararg = (Expr *) linitial(opexpr->args);
				arrayarg = (Expr *) lsecond(opexpr->args);

				/* Check permission to call function */
				aclresult = object_aclcheck(ProcedureRelationId, opexpr->opfuncid,
											GetUserId(),
											ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(opexpr->opfuncid));
				InvokeFunctionExecuteHook(opexpr->opfuncid);

				if (OidIsValid(opexpr->hashfuncid))
				{
					aclresult = object_aclcheck(ProcedureRelationId, opexpr->hashfuncid,
												GetUserId(),
												ACL_EXECUTE);
					if (aclresult != ACLCHECK_OK)
						aclcheck_error(aclresult, OBJECT_FUNCTION,
									   get_func_name(opexpr->hashfuncid));
					InvokeFunctionExecuteHook(opexpr->hashfuncid);
				}

				/* Set up the primary fmgr lookup information */
				finfo = palloc0_object(FmgrInfo);
				fcinfo = palloc0(SizeForFunctionCallInfo(2));
				fmgr_info(opexpr->opfuncid, finfo);
				fmgr_info_set_expr((Node *) node, finfo);
				InitFunctionCallInfoData(*fcinfo, finfo, 2,
										 opexpr->inputcollid, NULL, NULL);

				/*
				 * If hashfuncid is set, we create a EEOP_HASHED_SCALARARRAYOP
				 * step instead of a EEOP_SCALARARRAYOP.  This provides much
				 * faster lookup performance than the normal linear search
				 * when the number of items in the array is anything but very
				 * small.
				 */
				if (OidIsValid(opexpr->hashfuncid))
				{

					/* Evaluate scalar directly into left function argument */
					VciExecInitExprRec(scalararg, parent, state,
									   &fcinfo->args[0].value, &fcinfo->args[0].isnull, inittype);

					/*
					 * Evaluate array argument into our return value.  There's
					 * no danger in that, because the return value is
					 * guaranteed to be overwritten by
					 * EEOP_HASHED_SCALARARRAYOP, and will not be passed to
					 * any other expression.
					 */
					VciExecInitExprRec(arrayarg, parent, state, resv, resnull, inittype);

					/* And perform the operation */
					scratch.opcode = EEOP_HASHED_SCALARARRAYOP;
					scratch.d.hashedscalararrayop.finfo = finfo;
					scratch.d.hashedscalararrayop.fcinfo_data = fcinfo;
					scratch.d.hashedscalararrayop.saop = opexpr;

					ExprEvalPushStep(state, &scratch);
				}
				else
				{
					/* Evaluate scalar directly into left function argument */
					VciExecInitExprRec(scalararg, parent, state,
									   &fcinfo->args[0].value, &fcinfo->args[0].isnull, inittype);

					/*
					 * Evaluate array argument into our return value.  There's
					 * no danger in that, because the return value is
					 * guaranteed to be overwritten by EEOP_SCALARARRAYOP, and
					 * will not be passed to any other expression.
					 */
					VciExecInitExprRec(arrayarg, parent, state, resv, resnull, inittype);

					/* And perform the operation */
					scratch.opcode = EEOP_SCALARARRAYOP;
					scratch.d.scalararrayop.element_type = InvalidOid;
					scratch.d.scalararrayop.useOr = opexpr->useOr;
					scratch.d.scalararrayop.finfo = finfo;
					scratch.d.scalararrayop.fcinfo_data = fcinfo;
					scratch.d.scalararrayop.fn_addr = finfo->fn_addr;
					ExprEvalPushStep(state, &scratch);
				}
				break;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *boolexpr = (BoolExpr *) node;
				int			nargs = list_length(boolexpr->args);
				List	   *adjust_jumps = NIL;
				int			off;
				ListCell   *lc;

				/* allocate scratch memory used by all steps of AND/OR */
				if (boolexpr->boolop != NOT_EXPR)
					scratch.d.boolexpr.anynull = palloc_object(bool);

				/*
				 * For each argument evaluate the argument itself, then
				 * perform the bool operation's appropriate handling.
				 *
				 * We can evaluate each argument into our result area, since
				 * the short-circuiting logic means we only need to remember
				 * previous NULL values.
				 *
				 * AND/OR is split into separate STEP_FIRST (one) / STEP (zero
				 * or more) / STEP_LAST (one) steps, as each of those has to
				 * perform different work.  The FIRST/LAST split is valid
				 * because AND/OR have at least two arguments.
				 */
				off = 0;
				foreach(lc, boolexpr->args)
				{
					Expr	   *arg = (Expr *) lfirst(lc);

					/* Evaluate argument into our output variable */
					VciExecInitExprRec(arg, parent, state, resv, resnull, inittype);

					/* Perform the appropriate step type */
					switch (boolexpr->boolop)
					{
						case AND_EXPR:
							Assert(nargs >= 2);

							if (off == 0)
								scratch.opcode = EEOP_BOOL_AND_STEP_FIRST;
							else if (off + 1 == nargs)
								scratch.opcode = EEOP_BOOL_AND_STEP_LAST;
							else
								scratch.opcode = EEOP_BOOL_AND_STEP;
							break;
						case OR_EXPR:
							Assert(nargs >= 2);

							if (off == 0)
								scratch.opcode = EEOP_BOOL_OR_STEP_FIRST;
							else if (off + 1 == nargs)
								scratch.opcode = EEOP_BOOL_OR_STEP_LAST;
							else
								scratch.opcode = EEOP_BOOL_OR_STEP;
							break;
						case NOT_EXPR:
							Assert(nargs == 1);

							scratch.opcode = EEOP_BOOL_NOT_STEP;
							break;
						default:
							elog(ERROR, "unrecognized boolop: %d",
								 (int) boolexpr->boolop);
							break;
					}

					scratch.d.boolexpr.jumpdone = -1;
					ExprEvalPushStep(state, &scratch);
					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);
					off++;
				}

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->d.boolexpr.jumpdone == -1);
					as->d.boolexpr.jumpdone = state->steps_len;
				}
			}
			break;
		case T_RelabelType:
			{
				/* relabel doesn't need to do anything at runtime */
				RelabelType *relabel = (RelabelType *) node;

				VciExecInitExprRec(relabel->arg, parent, state, resv, resnull, inittype);
				break;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseExpr = (CaseExpr *) node;
				List	   *adjust_jumps = NIL;
				Datum	   *caseval = NULL;
				bool	   *casenull = NULL;
				ListCell   *lc;

				/*
				 * If there's a test expression, we have to evaluate it and
				 * save the value where the CaseTestExpr placeholders can find
				 * it.
				 */
				if (caseExpr->arg != NULL)
				{
					/* Evaluate testexpr into caseval/casenull workspace */
					caseval = palloc_object(Datum);
					casenull = palloc_object(bool);

					VciExecInitExprRec(caseExpr->arg, parent, state,
									   caseval, casenull, inittype);

					/*
					 * Since value might be read multiple times, force to R/O
					 * - but only if it could be an expanded datum.
					 */
					if (get_typlen(exprType((Node *) caseExpr->arg)) == -1)
					{
						/* change caseval in-place */
						scratch.opcode = EEOP_MAKE_READONLY;
						scratch.resvalue = caseval;
						scratch.resnull = casenull;
						scratch.d.make_readonly.value = caseval;
						scratch.d.make_readonly.isnull = casenull;
						ExprEvalPushStep(state, &scratch);
						/* restore normal settings of scratch fields */
						scratch.resvalue = resv;
						scratch.resnull = resnull;
					}
				}

				/*
				 * Prepare to evaluate each of the WHEN clauses in turn; as
				 * soon as one is true we return the value of the
				 * corresponding THEN clause.  If none are true then we return
				 * the value of the ELSE clause, or NULL if there is none.
				 */
				foreach(lc, caseExpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(lc);
					Datum	   *save_innermost_caseval;
					bool	   *save_innermost_casenull;
					int			whenstep;

					/*
					 * Make testexpr result available to CaseTestExpr nodes
					 * within the condition.  We must save and restore prior
					 * setting of innermost_caseval fields, in case this node
					 * is itself within a larger CASE.
					 *
					 * If there's no test expression, we don't actually need
					 * to save and restore these fields; but it's less code to
					 * just do so unconditionally.
					 */
					save_innermost_caseval = state->innermost_caseval;
					save_innermost_casenull = state->innermost_casenull;
					state->innermost_caseval = caseval;
					state->innermost_casenull = casenull;

					/* evaluate condition into CASE's result variables */
					VciExecInitExprRec(when->expr, parent, state, resv, resnull, inittype);

					state->innermost_caseval = save_innermost_caseval;
					state->innermost_casenull = save_innermost_casenull;

					/* If WHEN result isn't true, jump to next CASE arm */
					scratch.opcode = EEOP_JUMP_IF_NOT_TRUE;
					scratch.d.jump.jumpdone = -1;	/* computed later */
					ExprEvalPushStep(state, &scratch);
					whenstep = state->steps_len - 1;

					/*
					 * If WHEN result is true, evaluate THEN result, storing
					 * it into the CASE's result variables.
					 */
					VciExecInitExprRec(when->result, parent, state, resv, resnull, inittype);

					/* Emit JUMP step to jump to end of CASE's code */
					scratch.opcode = EEOP_JUMP;
					scratch.d.jump.jumpdone = -1;	/* computed later */
					ExprEvalPushStep(state, &scratch);

					/*
					 * Don't know address for that jump yet, compute once the
					 * whole CASE expression is built.
					 */
					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);

					/*
					 * But we can set WHEN test's jump target now, to make it
					 * jump to the next WHEN subexpression or the ELSE.
					 */
					state->steps[whenstep].d.jump.jumpdone = state->steps_len;
				}

				/* transformCaseExpr always adds a default */
				Assert(caseExpr->defresult);

				/* evaluate ELSE expr into CASE's result variables */
				VciExecInitExprRec(caseExpr->defresult, parent, state,
								   resv, resnull, inittype);

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->opcode == EEOP_JUMP);
					Assert(as->d.jump.jumpdone == -1);
					as->d.jump.jumpdone = state->steps_len;
				}
			}
			break;
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesce = (CoalesceExpr *) node;
				List	   *adjust_jumps = NIL;
				ListCell   *lc;

				/* We assume there's at least one arg */
				Assert(coalesce->args != NIL);

				/*
				 * Prepare evaluation of all coalesced arguments, after each
				 * one push a step that short-circuits if not null.
				 */
				foreach(lc, coalesce->args)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					/* evaluate argument, directly into result datum */
					VciExecInitExprRec(e, parent, state, resv, resnull, inittype);

					/* if it's not null, skip to end of COALESCE expr */
					scratch.opcode = EEOP_JUMP_IF_NOT_NULL;
					scratch.d.jump.jumpdone = -1;	/* adjust later */
					ExprEvalPushStep(state, &scratch);

					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);
				}

				/*
				 * No need to add a constant NULL return - we only can get to
				 * the end of the expression if a NULL already is being
				 * returned.
				 */

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->opcode == EEOP_JUMP_IF_NOT_NULL);
					Assert(as->d.jump.jumpdone == -1);
					as->d.jump.jumpdone = state->steps_len;
				}
			}
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				int			nelems = list_length(minmaxexpr->args);
				TypeCacheEntry *typentry;
				FmgrInfo   *finfo;
				FunctionCallInfo fcinfo;
				ListCell   *lc;
				int			off;

				/* Look up the btree comparison function for the datatype */
				typentry = lookup_type_cache(minmaxexpr->minmaxtype,
											 TYPECACHE_CMP_PROC);
				if (!OidIsValid(typentry->cmp_proc))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("could not identify a comparison function for type %s",
									format_type_be(minmaxexpr->minmaxtype))));

				/*
				 * If we enforced permissions checks on index support
				 * functions, we'd need to make a check here.  But the index
				 * support machinery doesn't do that, and thus neither does
				 * this code.
				 */

				/* Perform function lookup */
				finfo = palloc0_object(FmgrInfo);
				fcinfo = palloc0(SizeForFunctionCallInfo(2));
				fmgr_info(typentry->cmp_proc, finfo);
				fmgr_info_set_expr((Node *) node, finfo);
				InitFunctionCallInfoData(*fcinfo, finfo, 2,
										 minmaxexpr->inputcollid, NULL, NULL);

				scratch.opcode = EEOP_MINMAX;
				/* allocate space to store arguments */
				scratch.d.minmax.values =
					palloc_array(Datum, nelems);
				scratch.d.minmax.nulls =
					palloc_array(bool, nelems);
				scratch.d.minmax.nelems = nelems;

				scratch.d.minmax.op = minmaxexpr->op;
				scratch.d.minmax.finfo = finfo;
				scratch.d.minmax.fcinfo_data = fcinfo;

				/* evaluate expressions into minmax->values/nulls */
				off = 0;
				foreach(lc, minmaxexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					VciExecInitExprRec(e, parent, state,
									   &scratch.d.minmax.values[off],
									   &scratch.d.minmax.nulls[off], inittype);
					off++;
				}

				/* and push the final comparison */
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_SQLValueFunction:
			{
				SQLValueFunction *svf = (SQLValueFunction *) node;

				scratch.opcode = EEOP_SQLVALUEFUNCTION;
				scratch.d.sqlvaluefunction.svf = svf;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				Assert(jve->raw_expr != NULL);
				VciExecInitExprRec(jve->raw_expr, parent, state, resv, resnull, inittype);
				Assert(jve->formatted_expr != NULL);
				VciExecInitExprRec(jve->formatted_expr, parent, state, resv, resnull, inittype);

				break;
			}

		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;
				List	   *args = ctor->args;
				ListCell   *lc;
				int			nargs = list_length(args);
				int			argno = 0;

				if (ctor->func)
				{
					VciExecInitExprRec(ctor->func, parent, state, resv, resnull, inittype);
				}
				else if ((ctor->type == JSCTOR_JSON_PARSE && !ctor->unique) ||
						 ctor->type == JSCTOR_JSON_SERIALIZE)
				{
					/* Use the value of the first argument as result */
					VciExecInitExprRec(linitial(args), parent, state, resv, resnull, inittype);
				}
				else
				{
					JsonConstructorExprState *jcstate;

					jcstate = palloc0_object(JsonConstructorExprState);

					scratch.opcode = EEOP_JSON_CONSTRUCTOR;
					scratch.d.json_constructor.jcstate = jcstate;

					jcstate->constructor = ctor;
					jcstate->arg_values = palloc_array(Datum, nargs);
					jcstate->arg_nulls = palloc_array(bool, nargs);
					jcstate->arg_types = palloc_array(Oid, nargs);
					jcstate->nargs = nargs;

					foreach(lc, args)
					{
						Expr	   *arg = (Expr *) lfirst(lc);

						jcstate->arg_types[argno] = exprType((Node *) arg);

						if (IsA(arg, Const))
						{
							/* Don't evaluate const arguments every round */
							Const	   *con = (Const *) arg;

							jcstate->arg_values[argno] = con->constvalue;
							jcstate->arg_nulls[argno] = con->constisnull;
						}
						else
						{
							VciExecInitExprRec(arg, parent, state, &jcstate->arg_values[argno], &jcstate->arg_nulls[argno], inittype);
						}
						argno++;
					}

					/* prepare type cache for datum_to_json[b]() */
					if (ctor->type == JSCTOR_JSON_SCALAR)
					{
						bool		is_jsonb =
							ctor->returning->format->format_type == JS_FORMAT_JSONB;

						jcstate->arg_type_cache =
							palloc(sizeof(*jcstate->arg_type_cache) * nargs);

						for (int i = 0; i < nargs; i++)
						{
							JsonTypeCategory category;
							Oid			outfuncid;
							Oid			typid = jcstate->arg_types[i];

							json_categorize_type(typid, is_jsonb,
												 &category, &outfuncid);

							jcstate->arg_type_cache[i].outfuncid = outfuncid;
							jcstate->arg_type_cache[i].category = (int) category;
						}
					}

					ExprEvalPushStep(state, &scratch);
				}

				if (ctor->coercion)
				{
					Datum	   *innermost_caseval = state->innermost_caseval;
					bool	   *innermost_isnull = state->innermost_casenull;

					state->innermost_caseval = resv;
					state->innermost_casenull = resnull;

					VciExecInitExprRec(ctor->coercion, parent, state, resv, resnull, inittype);

					state->innermost_caseval = innermost_caseval;
					state->innermost_casenull = innermost_isnull;
				}
			}
			break;

		case T_JsonIsPredicate:
			{
				JsonIsPredicate *pred = (JsonIsPredicate *) node;

				VciExecInitExprRec((Expr *) pred->expr, parent, state, resv, resnull, inittype);

				scratch.opcode = EEOP_IS_JSON;
				scratch.d.is_json.pred = pred;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_JsonExpr:
			{
				JsonExpr   *jsexpr = castNode(JsonExpr, node);

				/*
				 * No need to initialize a full JsonExprState For
				 * JSON_TABLE(), because the upstream caller tfuncFetchRows()
				 * is only interested in the value of formatted_expr.
				 */
				if (jsexpr->op == JSON_TABLE_OP)
					VciExecInitExprRec((Expr *) jsexpr->formatted_expr, parent, state,
									   resv, resnull, inittype);
				else
					VciExecInitJsonExpr(jsexpr, parent, state, resv, resnull, &scratch, inittype);
				break;
			}

		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (ntest->nulltesttype == IS_NULL)
				{
					if (ntest->argisrow)
						scratch.opcode = EEOP_NULLTEST_ROWISNULL;
					else
						scratch.opcode = EEOP_NULLTEST_ISNULL;
				}
				else if (ntest->nulltesttype == IS_NOT_NULL)
				{
					if (ntest->argisrow)
						scratch.opcode = EEOP_NULLTEST_ROWISNOTNULL;
					else
						scratch.opcode = EEOP_NULLTEST_ISNOTNULL;
				}
				else
				{
					elog(ERROR, "unrecognized nulltesttype: %d",
						 (int) ntest->nulltesttype);
				}
				/* initialize cache in case it's a row test */
				scratch.d.nulltest_row.rowcache.cacheptr = NULL;

				/* first evaluate argument into result variable */
				VciExecInitExprRec(ntest->arg, parent, state,
								   resv, resnull, inittype);

				/* then push the test of that argument */
				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;

				/*
				 * Evaluate argument, directly into result datum.  That's ok,
				 * because resv/resnull is definitely not used anywhere else,
				 * and will get overwritten by the below EEOP_BOOLTEST_IS_*
				 * step.
				 */
				VciExecInitExprRec(btest->arg, parent, state, resv, resnull, inittype);

				switch (btest->booltesttype)
				{
					case IS_TRUE:
						scratch.opcode = EEOP_BOOLTEST_IS_TRUE;
						break;
					case IS_NOT_TRUE:
						scratch.opcode = EEOP_BOOLTEST_IS_NOT_TRUE;
						break;
					case IS_FALSE:
						scratch.opcode = EEOP_BOOLTEST_IS_FALSE;
						break;
					case IS_NOT_FALSE:
						scratch.opcode = EEOP_BOOLTEST_IS_NOT_FALSE;
						break;
					case IS_UNKNOWN:
						/* Same as scalar IS NULL test */
						scratch.opcode = EEOP_NULLTEST_ISNULL;
						break;
					case IS_NOT_UNKNOWN:
						/* Same as scalar IS NOT NULL test */
						scratch.opcode = EEOP_NULLTEST_ISNOTNULL;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) btest->booltesttype);
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				Oid			iofunc;
				bool		typisvarlena;
				Oid			typioparam;
				FunctionCallInfo fcinfo_in;

				/* evaluate argument into step's result area */
				VciExecInitExprRec(iocoerce->arg, parent, state, resv, resnull, inittype);

				/*
				 * Prepare both output and input function calls, to be
				 * evaluated inside a single evaluation step for speed - this
				 * can be a very common operation.
				 *
				 * We don't check permissions here as a type's input/output
				 * function are assumed to be executable by everyone.
				 */
				if (state->escontext == NULL)
					scratch.opcode = EEOP_IOCOERCE;
				else
					scratch.opcode = EEOP_IOCOERCE_SAFE;

				/* lookup the source type's output function */
				scratch.d.iocoerce.finfo_out = palloc0_object(FmgrInfo);
				scratch.d.iocoerce.fcinfo_data_out = palloc0(SizeForFunctionCallInfo(1));

				getTypeOutputInfo(exprType((Node *) iocoerce->arg),
								  &iofunc, &typisvarlena);
				fmgr_info(iofunc, scratch.d.iocoerce.finfo_out);
				fmgr_info_set_expr((Node *) node, scratch.d.iocoerce.finfo_out);
				InitFunctionCallInfoData(*scratch.d.iocoerce.fcinfo_data_out,
										 scratch.d.iocoerce.finfo_out,
										 1, InvalidOid, NULL, NULL);

				/* lookup the result type's input function */
				scratch.d.iocoerce.finfo_in = palloc0_object(FmgrInfo);
				scratch.d.iocoerce.fcinfo_data_in = palloc0(SizeForFunctionCallInfo(3));

				getTypeInputInfo(iocoerce->resulttype,
								 &iofunc, &typioparam);
				fmgr_info(iofunc, scratch.d.iocoerce.finfo_in);
				fmgr_info_set_expr((Node *) node, scratch.d.iocoerce.finfo_in);
				InitFunctionCallInfoData(*scratch.d.iocoerce.fcinfo_data_in,
										 scratch.d.iocoerce.finfo_in,
										 3, InvalidOid, NULL, NULL);

				/*
				 * We can preload the second and third arguments for the input
				 * function, since they're constants.
				 */
				fcinfo_in = scratch.d.iocoerce.fcinfo_data_in;
				fcinfo_in->args[1].value = ObjectIdGetDatum(typioparam);
				fcinfo_in->args[1].isnull = false;
				fcinfo_in->args[2].value = Int32GetDatum(-1);
				fcinfo_in->args[2].isnull = false;

				fcinfo_in->context = (Node *) state->escontext;

				ExprEvalPushStep(state, &scratch);
				break;
			}
			break;
		default:
			/* LCOV_EXCL_START */
			elog(ERROR, "unrecognized node type: %s(%d)",
				 VciGetNodeName(nodeTag(node)), (int) nodeTag(node));
			break;
			/* LCOV_EXCL_STOP */
	}

}

/*
 * VciExecInitQual: prepare a qual for execution by ExecQual
 *
 * Prepares for the evaluation of a conjunctive boolean expression (qual list
 * with implicit AND semantics) that returns true if none of the
 * subexpressions are false.
 *
 * We must return true if the list is empty.  Since that's a very common case,
 * we optimize it a bit further by translating to a NULL ExprState pointer
 * rather than setting up an ExprState that computes constant TRUE.  (Some
 * especially hot-spot callers of ExecQual detect this and avoid calling
 * ExecQual at all.)
 *
 * If any of the subexpressions yield NULL, then the result of the conjunction
 * is false.  This makes ExecQual primarily useful for evaluating WHERE
 * clauses, since SQL specifies that tuples with null WHERE results do not
 * get selected.
 * copied from src/backend/executor/execExpr.c
 */
ExprState *
VciExecInitQual(List *qual, PlanState *parent, vci_initexpr_t inittype)
{
	ExprState  *state;
	ExprEvalStep scratch;
	List	   *adjust_jumps = NIL;

	/* short-circuit (here and in ExecQual) for empty restriction list */
	if (qual == NIL)
		return NULL;

	Assert(IsA(qual, List));

	state = makeNode(ExprState);
	state->expr = (Expr *) qual;
	state->parent = parent;
	state->ext_params = NULL;

	/* mark expression as to be used with ExecQual() */
	state->flags = EEO_FLAG_IS_QUAL;

	/* Insert setup steps as needed */
	ExecCreateExprSetupSteps(state, (Node *) qual);

	/*
	 * ExecQual() needs to return false for an expression returning NULL. That
	 * allows us to short-circuit the evaluation the first time a NULL is
	 * encountered.  As qual evaluation is a hot-path this warrants using a
	 * special opcode for qual evaluation that's simpler than BOOL_AND (which
	 * has more complex NULL handling).
	 */
	scratch.opcode = EEOP_QUAL;

	/*
	 * We can use ExprState's resvalue/resnull as target for each qual expr.
	 */
	scratch.resvalue = &state->resvalue;
	scratch.resnull = &state->resnull;

	foreach_ptr(Expr, node, qual)
	{

		/* first evaluate expression */
		VciExecInitExprRec(node, parent, state, &state->resvalue, &state->resnull, inittype);

		/* then emit EEOP_QUAL to detect if it's false (or null) */
		scratch.d.qualexpr.jumpdone = -1;
		ExprEvalPushStep(state, &scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   state->steps_len - 1);
	}

	/* adjust jump targets */
	foreach_int(jump, adjust_jumps)
	{
		ExprEvalStep *as = &state->steps[jump];

		Assert(as->opcode == EEOP_QUAL);
		Assert(as->d.qualexpr.jumpdone == -1);
		as->d.qualexpr.jumpdone = state->steps_len;
	}

	/*
	 * At the end, we don't need to do anything more.  The last qual expr must
	 * have yielded TRUE, and since its result is stored in the desired output
	 * location, we're done.
	 */
	scratch.opcode = EEOP_DONE_RETURN;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}

/*
 * Perform setup necessary for the evaluation of a function-like expression,
 * appending argument evaluation steps to the steps list in *state, and
 * setting up *scratch so it is ready to be pushed.
 *
 * scratch is not pushed here, so that callers may override the opcode,
 * which is useful for function-like cases like DISTINCT.
 */
static void
VciExecInitFunc(ExprEvalStep *scratch, Expr *node, List *args, Oid funcid,
				Oid inputcollid, PlanState *parent, ExprState *state, vci_initexpr_t inittype)
{
	int			nargs = list_length(args);
	AclResult	aclresult;
	FmgrInfo   *flinfo;
	FunctionCallInfo fcinfo;
	int			argno;
	ListCell   *lc;

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
	scratch->d.func.finfo = palloc0_object(FmgrInfo);
	scratch->d.func.fcinfo_data = palloc0(SizeForFunctionCallInfo(nargs));
	flinfo = scratch->d.func.finfo;
	fcinfo = scratch->d.func.fcinfo_data;

	/* Set up the primary fmgr lookup information */
	fmgr_info(funcid, flinfo);
	fmgr_info_set_expr((Node *) node, flinfo);

	/* Initialize function call parameter structure too */
	InitFunctionCallInfoData(*fcinfo, flinfo,
							 nargs, inputcollid, NULL, NULL);

	/* Keep extra copies of this info to save an indirection at runtime */
	scratch->d.func.fn_addr = flinfo->fn_addr;
	scratch->d.func.nargs = nargs;

	/* We only support non-set functions here */
	if (flinfo->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 parent ? executor_errposition(parent->state,
											   exprLocation((Node *) node)) : 0));

	/* Build code to evaluate arguments directly into the fcinfo struct */
	argno = 0;
	foreach(lc, args)
	{
		Expr	   *arg = (Expr *) lfirst(lc);

		if (IsA(arg, Const))
		{
			/*
			 * Don't evaluate const arguments every round; especially
			 * interesting for constants in comparisons.
			 */
			Const	   *con = (Const *) arg;

			fcinfo->args[argno].value = con->constvalue;
			fcinfo->args[argno].isnull = con->constisnull;
		}
		else
		{
			VciExecInitExprRec(arg, parent, state,
							   &fcinfo->args[argno].value, &fcinfo->args[argno].isnull, inittype);
		}
		argno++;
	}

	/* Insert appropriate opcode depending on strictness and stats level */
	if (pgstat_track_functions <= flinfo->fn_stats)
	{
		if (flinfo->fn_strict && nargs > 0)
		{
			/* Choose nargs optimized implementation if available. */
			if (nargs == 1)
				scratch->opcode = EEOP_FUNCEXPR_STRICT_1;
			else if (nargs == 2)
				scratch->opcode = EEOP_FUNCEXPR_STRICT_2;
			else
				scratch->opcode = EEOP_FUNCEXPR_STRICT;
		}
		else
			scratch->opcode = EEOP_FUNCEXPR;
	}
	else
	{
		if (flinfo->fn_strict && nargs > 0)
			scratch->opcode = EEOP_FUNCEXPR_STRICT_FUSAGE;
		else
			scratch->opcode = EEOP_FUNCEXPR_FUSAGE;
	}
}

/* ----------------------------------------------------------------
 *					 ExecQual / ExecTargetList / ExecProject
 * ----------------------------------------------------------------
 */

/**
 * ExecProject
 *
 *		projects a tuple based on projection info and stores
 *		it in the previously specified tuple table slot.
 *
 *		Note: the result is always a virtual tuple; therefore it
 *		may reference the contents of the exprContext's scan tuples
 *		and/or temporary results constructed in the exprContext.
 *		If the caller wishes the result to be valid longer than that
 *		data will be valid, he must call ExecMaterializeSlot on the
 *		result slot.
 *
 * copied from src/include/executor/executor.h
 */
TupleTableSlot *
VciExecProject(VciProjectionInfo *projInfo)
{
	ExprContext *econtext = projInfo->pi_exprContext;
	ExprState  *state = &projInfo->pi_state;
	TupleTableSlot *slot = state->resultslot;
	bool		isnull;

	/*
	 * Clear any former contents of the result slot.  This makes it safe for
	 * us to use the slot's Datum/isnull arrays as workspace.
	 */
	ExecClearTuple(slot);

	/* Run the expression, discarding scalar result from the last column. */
	(void) ExecEvalExprSwitchContext(state, econtext, &isnull);

	/*
	 * Successfully formed a result row.  Mark the result slot as containing a
	 * valid virtual tuple (inlined version of ExecStoreVirtualTuple()).
	 */
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;

	return slot;
}

/**
 * Generate projection based on target list
 *
 * @param[in] targetlist Target list
 * @param[in] econtext   Execution context
 * @param[in] slot
 * @param[in] inputDesc
 * @return VciProjectionInfo type projection
 */
VciProjectionInfo *
VciExecBuildProjectionInfo(List *targetList,
						   ExprContext *econtext,
						   TupleTableSlot *slot,
						   PlanState *parent,
						   TupleDesc inputDesc)
{
	VciProjectionInfo *projInfo;
	ExprState  *state;
	ExprEvalStep scratch;
	ListCell   *lc;
	int			len = ExecTargetListLength(targetList);
	int			numSimpleVars;
	bool		directMap;
	int			exprlist_len;
	int			tle_id;
	int		   *workspace;
	int		   *varNumbers;
	int		   *varOutputCols;

	projInfo = palloc0_object(VciProjectionInfo);
	projInfo->pi_slotMap = palloc0_array(VciProjectionInfoSlot, len);
	projInfo->pi_tle_array = palloc0_array(TargetEntry *, len);

	/* since these are all int arrays, we need do just one palloc */
	workspace = palloc_array(int, len * 2);
	projInfo->pi_varNumbers = varNumbers = workspace;
	projInfo->pi_varOutputCols = varOutputCols = workspace + len;

	projInfo->pi_exprContext = econtext;
	/* We embed ExprState into ProjectionInfo instead of doing extra palloc */
	projInfo->pi_state.type = T_ExprState;
	state = &projInfo->pi_state;
	state->expr = (Expr *) targetList;
	state->resultslot = slot;

	numSimpleVars = 0;
	tle_id = 0;
	exprlist_len = 0;
	directMap = true;

	/* Insert setup steps as needed */
	ExecCreateExprSetupSteps(state, (Node *) targetList);

	/* Now compile each tlist column */
	foreach(lc, targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *variable = NULL;
		AttrNumber	attnum = 0;
		bool		isSafeVar = false;

		/*
		 * If tlist expression is a safe non-system Var, use the fast-path
		 * ASSIGN_*_VAR opcodes.  "Safe" means that we don't need to apply
		 * CheckVarSlotCompatibility() during plan startup.  If a source slot
		 * was provided, we make the equivalent tests here; if a slot was not
		 * provided, we assume that no check is needed because we're dealing
		 * with a non-relation-scan-level expression.
		 */
		if (tle->expr != NULL &&
			IsA(tle->expr, Var) &&
			((Var *) tle->expr)->varattno > 0)
		{
			/* Non-system Var, but how safe is it? */
			variable = (Var *) tle->expr;
			attnum = variable->varattno;

			if (inputDesc == NULL)
				isSafeVar = true;	/* can't check, just assume OK */
			else if (attnum <= inputDesc->natts)
			{
				Form_pg_attribute attr = TupleDescAttr(inputDesc, attnum - 1);

				/*
				 * If user attribute is dropped or has a type mismatch, don't
				 * use ASSIGN_*_VAR.  Instead let the normal expression
				 * machinery handle it (which'll possibly error out).
				 */
				if (!attr->attisdropped && variable->vartype == attr->atttypid)
				{
					isSafeVar = true;
				}
			}
		}

		if (isSafeVar)
		{
			varNumbers[numSimpleVars] = attnum;
			varOutputCols[numSimpleVars] = tle->resno;

			if (tle->resno != numSimpleVars + 1)
				directMap = false;

			/* Fast-path: just generate an EEOP_ASSIGN_*_VAR step */
			switch (variable->varno)
			{
				case INNER_VAR:
					/* get the tuple from the inner node */
					scratch.opcode = EEOP_ASSIGN_INNER_VAR;
					break;

				case OUTER_VAR:
					/* get the tuple from the outer node */
					scratch.opcode = EEOP_ASSIGN_OUTER_VAR;
					break;

					/* INDEX_VAR is handled by default case */

				default:

					/*
					 * Get the tuple from the relation being scanned, or the
					 * old/new tuple slot, if old/new values were requested.
					 */
					switch (variable->varreturningtype)
					{
						case VAR_RETURNING_DEFAULT:
							scratch.opcode = EEOP_ASSIGN_SCAN_VAR;
							break;
						case VAR_RETURNING_OLD:
							scratch.opcode = EEOP_ASSIGN_OLD_VAR;
							state->flags |= EEO_FLAG_HAS_OLD;
							break;
						case VAR_RETURNING_NEW:
							scratch.opcode = EEOP_ASSIGN_NEW_VAR;
							state->flags |= EEO_FLAG_HAS_NEW;
							break;
					}
					break;
			}

			scratch.d.assign_var.attnum = attnum - 1;
			scratch.d.assign_var.resultnum = tle->resno - 1;
			ExprEvalPushStep(state, &scratch);

			projInfo->pi_slotMap[tle_id].is_simple_var = true;
			projInfo->pi_slotMap[tle_id].data.simple_var.relid = variable->varno;
			projInfo->pi_slotMap[tle_id].data.simple_var.attno = variable->varattno;

			numSimpleVars++;
		}
		else
		{
			/*
			 * Otherwise, compile the column expression normally.
			 *
			 * We can't tell the expression to evaluate directly into the
			 * result slot, as the result slot (and the exprstate for that
			 * matter) can change between executions.  We instead evaluate
			 * into the ExprState's resvalue/resnull and then move.
			 */
			VciExecInitExprRec(tle->expr, parent, state,
							   &state->resvalue, &state->resnull, VCI_INIT_EXPR_NORMAL);

			/*
			 * Column might be referenced multiple times in upper nodes, so
			 * force value to R/O - but only if it could be an expanded datum.
			 */
			if (get_typlen(exprType((Node *) tle->expr)) == -1)
				scratch.opcode = EEOP_ASSIGN_TMP_MAKE_RO;
			else
				scratch.opcode = EEOP_ASSIGN_TMP;
			scratch.d.assign_tmp.resultnum = tle->resno - 1;
			ExprEvalPushStep(state, &scratch);

			/* Not a simple variable, add it to generic targetlist */
			projInfo->pi_tle_array[exprlist_len] = tle;

			projInfo->pi_slotMap[tle_id].is_simple_var = false;
			projInfo->pi_slotMap[tle_id].data.expr.expr_id = exprlist_len;

			exprlist_len++;
		}

		tle_id++;
	}

	projInfo->pi_tle_array_len = exprlist_len;

	projInfo->pi_numSimpleVars = numSimpleVars;
	projInfo->pi_directMap = directMap;

	if (projInfo->pi_tle_array == 0)
		projInfo->pi_tle_array = NULL;

	scratch.opcode = EEOP_DONE_RETURN;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return projInfo;
}

/*
 * Push steps to evaluate a JsonExpr and its various subsidiary expressions.
 */
static void
VciExecInitJsonExpr(JsonExpr *jsexpr, PlanState *parent, ExprState *state,
					Datum *resv, bool *resnull,
					ExprEvalStep *scratch, vci_initexpr_t inittype)
{
	JsonExprState *jsestate = palloc0_object(JsonExprState);
	ListCell   *argexprlc;
	ListCell   *argnamelc;
	List	   *jumps_return_null = NIL;
	List	   *jumps_to_end = NIL;
	ListCell   *lc;
	ErrorSaveContext *escontext;
	bool		returning_domain =
		get_typtype(jsexpr->returning->typid) == TYPTYPE_DOMAIN;

	Assert(jsexpr->on_error != NULL);

	jsestate->jsexpr = jsexpr;

	/*
	 * Evaluate formatted_expr storing the result into
	 * jsestate->formatted_expr.
	 */
	VciExecInitExprRec((Expr *) jsexpr->formatted_expr, parent, state,
					   &jsestate->formatted_expr.value,
					   &jsestate->formatted_expr.isnull, inittype);

	/* JUMP to return NULL if formatted_expr evaluates to NULL */
	jumps_return_null = lappend_int(jumps_return_null, state->steps_len);
	scratch->opcode = EEOP_JUMP_IF_NULL;
	scratch->resnull = &jsestate->formatted_expr.isnull;
	scratch->d.jump.jumpdone = -1;	/* set below */
	ExprEvalPushStep(state, scratch);

	/*
	 * Evaluate pathspec expression storing the result into
	 * jsestate->pathspec.
	 */
	VciExecInitExprRec((Expr *) jsexpr->path_spec, parent, state,
					   &jsestate->pathspec.value,
					   &jsestate->pathspec.isnull, inittype);

	/* JUMP to return NULL if path_spec evaluates to NULL */
	jumps_return_null = lappend_int(jumps_return_null, state->steps_len);
	scratch->opcode = EEOP_JUMP_IF_NULL;
	scratch->resnull = &jsestate->pathspec.isnull;
	scratch->d.jump.jumpdone = -1;	/* set below */
	ExprEvalPushStep(state, scratch);

	/* Steps to compute PASSING args. */
	jsestate->args = NIL;
	forboth(argexprlc, jsexpr->passing_values,
			argnamelc, jsexpr->passing_names)
	{
		Expr	   *argexpr = (Expr *) lfirst(argexprlc);
		String	   *argname = lfirst_node(String, argnamelc);
		JsonPathVariable *var = palloc_object(JsonPathVariable);

		var->name = argname->sval;
		var->typid = exprType((Node *) argexpr);
		var->typmod = exprTypmod((Node *) argexpr);

		VciExecInitExprRec((Expr *) argexpr, parent, state, &var->value, &var->isnull, inittype);

		jsestate->args = lappend(jsestate->args, var);
	}

	/* Step for jsonpath evaluation; see ExecEvalJsonExprPath(). */
	scratch->opcode = EEOP_JSONEXPR_PATH;
	scratch->resvalue = resv;
	scratch->resnull = resnull;
	scratch->d.jsonexpr.jsestate = jsestate;
	ExprEvalPushStep(state, scratch);

	/*
	 * Step to return NULL after jumping to skip the EEOP_JSONEXPR_PATH step
	 * when either formatted_expr or pathspec is NULL.  Adjust jump target
	 * addresses of JUMPs that we added above.
	 */
	foreach(lc, jumps_return_null)
	{
		ExprEvalStep *as = &state->steps[lfirst_int(lc)];

		as->d.jump.jumpdone = state->steps_len;
	}
	scratch->opcode = EEOP_CONST;
	scratch->resvalue = resv;
	scratch->resnull = resnull;
	scratch->d.constval.value = (Datum) 0;
	scratch->d.constval.isnull = true;
	ExprEvalPushStep(state, scratch);

	escontext = jsexpr->on_error->btype != JSON_BEHAVIOR_ERROR ?
		&jsestate->escontext : NULL;

	/*
	 * To handle coercion errors softly, use the following ErrorSaveContext to
	 * pass to VciExecInitExprRec() when initializing the coercion expressions
	 * and in the EEOP_JSONEXPR_COERCION step.
	 */
	jsestate->escontext.type = T_ErrorSaveContext;

	/*
	 * Steps to coerce the result value computed by EEOP_JSONEXPR_PATH or the
	 * NULL returned on NULL input as described above.
	 */
	jsestate->jump_eval_coercion = -1;
	if (jsexpr->use_json_coercion)
	{

		jsestate->jump_eval_coercion = state->steps_len;

		VciExecInitJsonCoercion(state, jsexpr->returning, escontext,
								jsexpr->omit_quotes,
								jsexpr->op == JSON_EXISTS_OP,
								resv, resnull);
	}
	else if (jsexpr->use_io_coercion)
	{
		/*
		 * Here we only need to initialize the FunctionCallInfo for the target
		 * type's input function, which is called by ExecEvalJsonExprPath()
		 * itself, so no additional step is necessary.
		 */
		Oid			typinput;
		Oid			typioparam;
		FmgrInfo   *finfo;
		FunctionCallInfo fcinfo;

		getTypeInputInfo(jsexpr->returning->typid, &typinput, &typioparam);
		finfo = palloc0_object(FmgrInfo);
		fcinfo = palloc0(SizeForFunctionCallInfo(3));
		fmgr_info(typinput, finfo);
		fmgr_info_set_expr((Node *) jsexpr->returning, finfo);
		InitFunctionCallInfoData(*fcinfo, finfo, 3, InvalidOid, NULL, NULL);

		/*
		 * We can preload the second and third arguments for the input
		 * function, since they're constants.
		 */
		fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
		fcinfo->args[1].isnull = false;
		fcinfo->args[2].value = Int32GetDatum(jsexpr->returning->typmod);
		fcinfo->args[2].isnull = false;
		fcinfo->context = (Node *) escontext;

		jsestate->input_fcinfo = fcinfo;
	}

	/*
	 * Add a special step, if needed, to check if the coercion evaluation ran
	 * into an error but was not thrown because the ON ERROR behavior is not
	 * ERROR.  It will set jsestate->error if an error did occur.
	 */
	if (jsestate->jump_eval_coercion >= 0 && escontext != NULL)
	{
		scratch->opcode = EEOP_JSONEXPR_COERCION_FINISH;
		scratch->d.jsonexpr.jsestate = jsestate;
		ExprEvalPushStep(state, scratch);
	}

	jsestate->jump_empty = jsestate->jump_error = -1;

	/*
	 * Step to check jsestate->error and return the ON ERROR expression if
	 * there is one.  This handles both the errors that occur during jsonpath
	 * evaluation in EEOP_JSONEXPR_PATH and subsequent coercion evaluation.
	 *
	 * Speed up common cases by avoiding extra steps for a NULL-valued ON
	 * ERROR expression unless RETURNING a domain type, where constraints must
	 * be checked. ExecEvalJsonExprPath() already returns NULL on error,
	 * making additional steps unnecessary in typical scenarios. Note that the
	 * default ON ERROR behavior for JSON_VALUE() and JSON_QUERY() is to
	 * return NULL.
	 */
	if (jsexpr->on_error->btype != JSON_BEHAVIOR_ERROR &&
		(!(IsA(jsexpr->on_error->expr, Const) &&
		   ((Const *) jsexpr->on_error->expr)->constisnull) ||
		 returning_domain))
	{
		ErrorSaveContext *saved_escontext;

		jsestate->jump_error = state->steps_len;

		/* JUMP to end if false, that is, skip the ON ERROR expression. */
		jumps_to_end = lappend_int(jumps_to_end, state->steps_len);
		scratch->opcode = EEOP_JUMP_IF_NOT_TRUE;
		scratch->resvalue = &jsestate->error.value;
		scratch->resnull = &jsestate->error.isnull;
		scratch->d.jump.jumpdone = -1;	/* set below */
		ExprEvalPushStep(state, scratch);

		/*
		 * Steps to evaluate the ON ERROR expression; handle errors softly to
		 * rethrow them in COERCION_FINISH step that will be added later.
		 */
		saved_escontext = state->escontext;
		state->escontext = escontext;
		VciExecInitExprRec((Expr *) jsexpr->on_error->expr, parent,
						   state, resv, resnull, inittype);
		state->escontext = saved_escontext;

		/* Step to coerce the ON ERROR expression if needed */
		if (jsexpr->on_error->coerce)
			VciExecInitJsonCoercion(state, jsexpr->returning, escontext, jsexpr->omit_quotes, false, resv,
									resnull);

		/*
		 * Add a COERCION_FINISH step to check for errors that may occur when
		 * coercing and rethrow them.
		 */
		if (jsexpr->on_error->coerce ||
			IsA(jsexpr->on_error->expr, CoerceViaIO) ||
			IsA(jsexpr->on_error->expr, CoerceToDomain))
		{
			scratch->opcode = EEOP_JSONEXPR_COERCION_FINISH;
			scratch->resvalue = resv;
			scratch->resnull = resnull;
			scratch->d.jsonexpr.jsestate = jsestate;
			ExprEvalPushStep(state, scratch);
		}

		/* JUMP to end to skip the ON EMPTY steps added below. */
		jumps_to_end = lappend_int(jumps_to_end, state->steps_len);
		scratch->opcode = EEOP_JUMP;
		scratch->d.jump.jumpdone = -1;
		ExprEvalPushStep(state, scratch);
	}

	/*
	 * Step to check jsestate->empty and return the ON EMPTY expression if
	 * there is one.
	 *
	 * See the comment above for details on the optimization for NULL-valued
	 * expressions.
	 */
	if (jsexpr->on_empty != NULL &&
		jsexpr->on_empty->btype != JSON_BEHAVIOR_ERROR &&
		(!(IsA(jsexpr->on_empty->expr, Const) &&
		   ((Const *) jsexpr->on_empty->expr)->constisnull) ||
		 returning_domain))
	{
		ErrorSaveContext *saved_escontext;

		jsestate->jump_empty = state->steps_len;

		/* JUMP to end if false, that is, skip the ON EMPTY expression. */
		jumps_to_end = lappend_int(jumps_to_end, state->steps_len);
		scratch->opcode = EEOP_JUMP_IF_NOT_TRUE;
		scratch->resvalue = &jsestate->empty.value;
		scratch->resnull = &jsestate->empty.isnull;
		scratch->d.jump.jumpdone = -1;	/* set below */
		ExprEvalPushStep(state, scratch);

		/*
		 * Steps to evaluate the ON EMPTY expression; handle errors softly to
		 * rethrow them in COERCION_FINISH step that will be added later.
		 */
		saved_escontext = state->escontext;
		state->escontext = escontext;
		VciExecInitExprRec((Expr *) jsexpr->on_empty->expr, parent,
						   state, resv, resnull, inittype);
		state->escontext = saved_escontext;

		/* Step to coerce the ON EMPTY expression if needed */
		if (jsexpr->on_empty->coerce)
			VciExecInitJsonCoercion(state, jsexpr->returning, escontext, jsexpr->omit_quotes, false, resv,
									resnull);

		/*
		 * Add a COERCION_FINISH step to check for errors that may occur when
		 * coercing and rethrow them.
		 */
		if (jsexpr->on_empty->coerce ||
			IsA(jsexpr->on_empty->expr, CoerceViaIO) ||
			IsA(jsexpr->on_empty->expr, CoerceToDomain))
		{
			scratch->opcode = EEOP_JSONEXPR_COERCION_FINISH;
			scratch->resvalue = resv;
			scratch->resnull = resnull;
			scratch->d.jsonexpr.jsestate = jsestate;
			ExprEvalPushStep(state, scratch);
		}
	}

	foreach(lc, jumps_to_end)
	{
		ExprEvalStep *as = &state->steps[lfirst_int(lc)];

		as->d.jump.jumpdone = state->steps_len;
	}

	jsestate->jump_end = state->steps_len;
}

/*
 * Initialize a EEOP_JSONEXPR_COERCION step to coerce the value given in resv
 * to the given RETURNING type.
 */
static void
VciExecInitJsonCoercion(ExprState *state, JsonReturning *returning,
						ErrorSaveContext *escontext, bool omit_quotes,
						bool exists_coerce,
						Datum *resv, bool *resnull)
{
	ExprEvalStep scratch = {0};

	/* For json_populate_type() */
	scratch.opcode = EEOP_JSONEXPR_COERCION;
	scratch.resvalue = resv;
	scratch.resnull = resnull;
	scratch.d.jsonexpr_coercion.targettype = returning->typid;
	scratch.d.jsonexpr_coercion.targettypmod = returning->typmod;
	scratch.d.jsonexpr_coercion.json_coercion_cache = NULL;
	scratch.d.jsonexpr_coercion.escontext = escontext;
	scratch.d.jsonexpr_coercion.omit_quotes = omit_quotes;
	scratch.d.jsonexpr_coercion.exists_coerce = exists_coerce;
	scratch.d.jsonexpr_coercion.exists_cast_to_int = exists_coerce &&
		getBaseType(returning->typid) == INT4OID;
	scratch.d.jsonexpr_coercion.exists_check_domain = exists_coerce &&
		DomainHasConstraints(returning->typid);
	ExprEvalPushStep(state, &scratch);
}

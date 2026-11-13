/*-------------------------------------------------------------------------
 *
 * vci_param.c
 *	  Routines to handle VCI Param Expr node
 *
 * Param evaluation may execute ExecSetParamPlan() only the first time to execute the
 * subquery and receive and return the result, but parallel workers in parallel execution
 * may not be able to execute the subquery. To avoid this, the parallel worker asks
 * the main backend process to execute ExecSetParamPlan() on its behalf.
 *
 * Therefore, Param is converted to dedicated VciParamState.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_param.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"

#include "vci.h"
#include "vci_executor.h"

/**
 * VciParamState evaluation function
 *
 * @param[in]  exprstate Pointer to VciParamState (Casted to ExprState)
 * @param[in]  econtext  execution context
 * @param[out] isNull    true if result of evaluation function is NULL
 * @return result of evaluation function
 */
void
VciExecEvalParamExec(ExprState *exprstate, ExprEvalStep *op, ExprContext *econtext)
{
	ParamExecData *prm;

	int			thisParamId = op->d.param.paramid;

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

	*op->resnull = prm->isnull;
	*op->resvalue = prm->value;
}

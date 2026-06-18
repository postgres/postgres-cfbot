/*-------------------------------------------------------------------------
 *
 * subselect.h
 *	  Planning routines for subselects.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/subselect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSELECT_H
#define SUBSELECT_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

/*
 * The quals that are guaranteed to filter the rows on which the SubLinks at
 * one jointree node are evaluated, and the Vars they force non-null.  The
 * latter is computed on first use and then shared by every SubLink at that
 * node.
 */
typedef struct SafeQualsInfo
{
	List	   *quals;			/* qual clauses, with implicit-AND semantics */
	List	   *nonnullable_vars;	/* multibitmapset; valid if computed */
	bool		computed;		/* have we computed nonnullable_vars yet? */
} SafeQualsInfo;

extern void SS_process_ctes(PlannerInfo *root);
extern ScalarArrayOpExpr *convert_VALUES_to_ANY(PlannerInfo *root,
												Node *testexpr,
												Query *values);
extern JoinExpr *convert_ANY_sublink_to_join(PlannerInfo *root,
											 SubLink *sublink,
											 bool under_not,
											 Relids available_rels,
											 SafeQualsInfo *sqinfo);
extern JoinExpr *convert_EXISTS_sublink_to_join(PlannerInfo *root,
												SubLink *sublink,
												bool under_not,
												Relids available_rels);
extern Node *SS_replace_correlation_vars(PlannerInfo *root, Node *expr);
extern Node *SS_process_sublinks(PlannerInfo *root, Node *expr, bool isQual);
extern void SS_identify_outer_params(PlannerInfo *root);
extern void SS_charge_for_initplans(PlannerInfo *root, RelOptInfo *final_rel);
extern void SS_compute_initplan_cost(List *init_plans,
									 Cost *initplan_cost_p,
									 bool *unsafe_initplans_p);
extern void SS_attach_initplans(PlannerInfo *root, Plan *plan);
extern void SS_finalize_plan(PlannerInfo *root, Plan *plan);
extern Param *SS_make_initplan_output_param(PlannerInfo *root,
											Oid resulttype, int32 resulttypmod,
											Oid resultcollation);
extern void SS_make_initplan_from_plan(PlannerInfo *root,
									   PlannerInfo *subroot, Plan *plan,
									   Param *prm);

#endif							/* SUBSELECT_H */

/*-------------------------------------------------------------------------
 *
 * like_bmh.c
 *	  Boyer-Moore-Horspool search for LIKE patterns containing one literal.
 *
 * varlena.c already uses Boyer-Moore-Horspool for the position/replace
 * built-ins, but that code searches a single (haystack, needle) pair with an
 * adaptively sized skip table.  This file keeps its own small implementation
 * because LIKE must interpret its internal backslash escapes while extracting
 * the literal and caches the compiled search state in FmgrInfo across rows,
 * so sharing the varlena.c machinery did not look worthwhile.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/like_bmh.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "nodes/primnodes.h"
#include "utils/like_bmh.h"
#include "utils/pg_locale.h"

#define LIKE_BMH_SKIP_TABLE_SIZE	256
/*
 * Minimum literal length for the BMH path.  Measurements for these
 * thresholds showed that the single-byte matcher's cheap first-byte scan
 * favors a longer literal, while UTF-8 can benefit at shorter lengths.
 */
#define LIKE_BMH_MIN_LITERAL_LEN_SB	6
#define LIKE_BMH_MIN_LITERAL_LEN_UTF8	4
#define LIKE_TRUE					1
#define LIKE_FALSE					0

static inline int
like_bmh_min_literal_len(void)
{
	return pg_database_encoding_max_length() == 1 ?
		LIKE_BMH_MIN_LITERAL_LEN_SB : LIKE_BMH_MIN_LITERAL_LEN_UTF8;
}

typedef struct LikeBMHSearchState
{
	LikeBMHState base;
	Oid			collation;
	int			literal_len;
	int			skip_table[LIKE_BMH_SKIP_TABLE_SIZE];
	char		literal[FLEXIBLE_ARRAY_MEMBER];
} LikeBMHSearchState;

/*
 * Check whether a pattern in LIKE's internal backslash-escape form is a
 * single literal surrounded by '%' wildcards.  Explicit ESCAPE characters
 * have already been converted to backslashes by like_escape().
 *
 * Report the literal length with escapes skipped.  The escapes are removed
 * later, when the search state is built.
 */
static bool
like_bmh_pattern_is_eligible(const char *p, int plen, int *literal_len)
{
	int			min_len = like_bmh_min_literal_len();
	int			i;

	*literal_len = 0;
	if (plen < min_len + 2 ||
		p[0] != '%' || p[plen - 1] != '%')
		return false;

	for (i = 1; i < plen - 1; i++)
	{
		if (p[i] == '%' || p[i] == '_')
			return false;
		if (p[i] == '\\')
		{
			/*
			 * A backslash must escape a real literal byte, not the trailing
			 * '%'.  Reject patterns such as '%foo\%', where the escape would
			 * consume the closing wildcard.
			 */
			if (i + 1 >= plen - 1)
				return false;
			i++;
		}
		(*literal_len)++;
	}

	return *literal_len >= min_len;
}

/*
 * Cache only Const patterns.  Params and PL/pgSQL variables can change
 * during execution, and ScalarArrayOpExpr can pass different elements.
 */
static bool
like_bmh_pattern_arg_is_const(FmgrInfo *flinfo)
{
	List	   *args;
	Node	   *expr;
	Node	   *arg;

	if (!flinfo->fn_expr)
		return false;

	expr = flinfo->fn_expr;
	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else
		return false;

	if (list_length(args) < 2)
		return false;

	arg = (Node *) list_nth(args, 1);
	return IsA(arg, Const);
}

/* Keep one-time setup out of the stable-pattern matching hot path. */
static pg_noinline LikeBMHState *
like_bmh_init(const char *p, int plen, FmgrInfo *flinfo, Oid collation)
{
	LikeBMHState *state;
	LikeBMHSearchState *search_state;
	int		   *skip_table;
	char	   *literal;
	int			literal_len;
	int			i;
	int			j;

	if (!like_bmh_pattern_arg_is_const(flinfo))
	{
		state = MemoryContextAlloc(flinfo->fn_mcxt, sizeof(LikeBMHState));
		state->mode = LIKE_BMH_GENERIC;
		flinfo->fn_extra = state;
		return state;
	}

	/*
	 * A byte search is safe in single-byte encodings and UTF-8.  In UTF-8,
	 * neither a leading byte nor an ASCII byte can occur as a continuation
	 * byte, so a valid literal cannot match starting inside a character.
	 *
	 * Cache all rejected cases so subsequent rows go directly to the generic
	 * matcher.  In particular, reject other multibyte encodings before scanning
	 * the pattern.  An unresolved collation is left for GenericMatchText to
	 * report, so that the fallback path raises the usual error.
	 */
	if ((pg_database_encoding_max_length() > 1 &&
		 GetDatabaseEncoding() != PG_UTF8) ||
		!OidIsValid(collation) ||
		!like_bmh_pattern_is_eligible(p, plen, &literal_len) ||
		!pg_newlocale_from_collation(collation)->deterministic)
	{
		state = MemoryContextAlloc(flinfo->fn_mcxt, sizeof(LikeBMHState));
		state->mode = LIKE_BMH_GENERIC;
		flinfo->fn_extra = state;
		return state;
	}

	search_state = MemoryContextAlloc(flinfo->fn_mcxt,
										  sizeof(LikeBMHSearchState) + literal_len);
	search_state->base.mode = LIKE_BMH_SEARCH;
	search_state->collation = collation;
	search_state->literal_len = literal_len;
	state = (LikeBMHState *) search_state;
	skip_table = search_state->skip_table;
	literal = search_state->literal;

	for (i = 0; i < LIKE_BMH_SKIP_TABLE_SIZE; i++)
		skip_table[i] = literal_len;

	for (i = 1, j = 0; i < plen - 1; i++, j++)
	{
		if (p[i] == '\\')
			i++;
		literal[j] = p[i];
	}
	Assert(j == literal_len);
	for (i = 0; i < literal_len - 1; i++)
		skip_table[(unsigned char) literal[i]] =
			literal_len - i - 1;

	flinfo->fn_extra = state;
	return state;
}

/*
 * Keep Horspool's last-byte guard and shifts, but check the remaining bytes
 * from left to right.  This detects an early mismatch without first scanning
 * a long matching suffix.  The shift depends only on the byte at the end of
 * the candidate, so changing the comparison order does not affect its safety.
 */
static int
like_bmh_search(const char *s, int slen, const char *literal, int literal_len,
				const int *skip_table)
{
	int			pos = literal_len - 1;
	unsigned char lastlit = (unsigned char) literal[literal_len - 1];
	int			match_shift = skip_table[lastlit];

	while (pos < slen)
	{
		unsigned char last = (unsigned char) s[pos];
		const char *t;
		int			i;

		if (last != lastlit)
		{
			pos += skip_table[last];
			continue;
		}

		t = s + pos - literal_len + 1;
		i = 0;
		while (i < literal_len - 1 && literal[i] == t[i])
			i++;
		if (i == literal_len - 1)
			return LIKE_TRUE;

		/* The last byte matched, so its shift is already known. */
		pos += match_shift;
	}

	return LIKE_FALSE;
}


/*
 * Use or initialize the BMH state cached in the caller's FmgrInfo.
 * LIKE_BMH_FALLBACK tells the caller to use the generic matcher.
 */
int
like_bmh_match(const char *s, int slen, const char *p, int plen,
			   FmgrInfo *flinfo, Oid collation)
{
	LikeBMHState *state = flinfo->fn_extra;
	LikeBMHSearchState *search_state;

	if (state == NULL)
		state = like_bmh_init(p, plen, flinfo, collation);

	if (unlikely(state->mode == LIKE_BMH_GENERIC))
		return LIKE_BMH_FALLBACK;

	search_state = (LikeBMHSearchState *) state;
	if (unlikely(search_state->collation != collation))
		return LIKE_BMH_FALLBACK;

	return like_bmh_search(s, slen, search_state->literal,
						 search_state->literal_len, search_state->skip_table);
}

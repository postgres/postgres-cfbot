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
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/like_bmh.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "nodes/primnodes.h"
#include "utils/like_bmh.h"
#include "utils/pg_locale.h"

#define LIKE_BMH_SKIP_TABLE_SIZE	256
#define LIKE_BMH_MIN_LITERAL_LEN	4
#define LIKE_TRUE					1
#define LIKE_FALSE					0

typedef struct LikeBMHSearchState
{
	LikeBMHState base;
	Oid			collation;
	int			literal_len;
	int			skip_table[LIKE_BMH_SKIP_TABLE_SIZE];
	char		literal[FLEXIBLE_ARRAY_MEMBER];
} LikeBMHSearchState;

typedef struct LikeBMHRevalidateState
{
	LikeBMHState base;
	Oid			collation;
	int			pattern_len;
	int			literal_len;
	int			skip_table[LIKE_BMH_SKIP_TABLE_SIZE];
	char		buf[FLEXIBLE_ARRAY_MEMBER];
} LikeBMHRevalidateState;

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
	int			i;

	*literal_len = 0;
	if (plen < LIKE_BMH_MIN_LITERAL_LEN + 2 ||
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

	return *literal_len >= LIKE_BMH_MIN_LITERAL_LEN;
}

/* Keep one-time setup out of the stable-pattern matching hot path. */
static pg_noinline LikeBMHState *
like_bmh_init(const char *p, int plen, FmgrInfo *flinfo, Oid collation)
{
	LikeBMHState *state;
	LikeBMHSearchState *search_state;
	LikeBMHRevalidateState *revalidate_state;
	int		   *skip_table;
	char	   *literal;
	pg_locale_t locale;
	bool		pattern_stable;
	int			literal_len;
	int			i;
	int			j;

	/*
	 * ScalarArrayOpExpr invokes the operator once per array element.  The
	 * array expression can be stable while the pattern passed to this function
	 * changes between calls, so its cached search state must be revalidated.
	 */
	if (flinfo->fn_expr != NULL && IsA(flinfo->fn_expr, ScalarArrayOpExpr))
		pattern_stable = false;
	else
		pattern_stable = get_fn_expr_arg_stable(flinfo, 1);

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
		!like_bmh_pattern_is_eligible(p, plen, &literal_len))
	{
		state = MemoryContextAlloc(flinfo->fn_mcxt, sizeof(LikeBMHState));
		state->mode = LIKE_BMH_GENERIC;
		flinfo->fn_extra = state;
		return state;
	}

	locale = pg_newlocale_from_collation(collation);
	if (!locale->deterministic)
	{
		state = MemoryContextAlloc(flinfo->fn_mcxt, sizeof(LikeBMHState));
		state->mode = LIKE_BMH_GENERIC;
		flinfo->fn_extra = state;
		return state;
	}

	if (pattern_stable)
	{
		search_state = MemoryContextAlloc(flinfo->fn_mcxt,
										  sizeof(LikeBMHSearchState) + literal_len);
		search_state->base.mode = LIKE_BMH_SEARCH;
		search_state->collation = collation;
		search_state->literal_len = literal_len;
		state = (LikeBMHState *) search_state;
		skip_table = search_state->skip_table;
		literal = search_state->literal;
	}
	else
	{
		revalidate_state = MemoryContextAlloc(flinfo->fn_mcxt,
											  sizeof(LikeBMHRevalidateState) +
											  literal_len + plen);
		revalidate_state->base.mode = LIKE_BMH_SEARCH_REVALIDATE;
		revalidate_state->collation = collation;
		revalidate_state->pattern_len = plen;
		revalidate_state->literal_len = literal_len;
		state = (LikeBMHState *) revalidate_state;
		skip_table = revalidate_state->skip_table;
		literal = revalidate_state->buf;
	}

	for (i = 0; i < LIKE_BMH_SKIP_TABLE_SIZE; i++)
		skip_table[i] = literal_len;

	for (i = 1, j = 0; i < plen - 1; i++, j++)
	{
		if (p[i] == '\\')
			i++;
		literal[j] = p[i];
	}
	Assert(j == literal_len);
	if (!pattern_stable)
		memcpy(literal + literal_len, p, plen);

	for (i = 0; i < literal_len - 1; i++)
		skip_table[(unsigned char) literal[i]] =
			literal_len - i - 1;

	flinfo->fn_extra = state;
	return state;
}

static int
like_bmh_search(const char *s, int slen, const char *literal, int literal_len,
				const int *skip_table)
{
	int			pos = literal_len - 1;

	while (pos < slen)
	{
		int			i = literal_len - 1;
		int			text_pos = pos;
		unsigned char last = (unsigned char) s[pos];

		while (i >= 0 && literal[i] == s[text_pos])
		{
			i--;
			text_pos--;
		}
		if (i < 0)
			return LIKE_TRUE;

		pos += skip_table[last];
	}

	return LIKE_FALSE;
}

/* Keep variable-pattern revalidation out of the stable-pattern hot path. */
static pg_noinline int
like_bmh_match_revalidate(const char *s, int slen, const char *p, int plen,
						  LikeBMHState *state, Oid collation)
{
	LikeBMHRevalidateState *revalidate_state;

	if (state->mode == LIKE_BMH_GENERIC)
		return LIKE_BMH_FALLBACK;

	Assert(state->mode == LIKE_BMH_SEARCH_REVALIDATE);
	revalidate_state = (LikeBMHRevalidateState *) state;
	if (unlikely(revalidate_state->collation != collation))
		return LIKE_BMH_FALLBACK;

	if (revalidate_state->pattern_len != plen ||
		memcmp(revalidate_state->buf + revalidate_state->literal_len,
			   p, plen) != 0)
	{
		/*
		 * A changing pattern would make rebuilding the skip table every row
		 * expensive.  Once variation is observed, use the generic matcher for
		 * the rest of this expression's execution.
		 */
		state->mode = LIKE_BMH_GENERIC;
		return LIKE_BMH_FALLBACK;
	}

	return like_bmh_search(s, slen, revalidate_state->buf,
						 revalidate_state->literal_len,
						 revalidate_state->skip_table);
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

	if (unlikely(state->mode != LIKE_BMH_SEARCH))
		return like_bmh_match_revalidate(s, slen, p, plen, state, collation);

	search_state = (LikeBMHSearchState *) state;
	if (unlikely(search_state->collation != collation))
		return LIKE_BMH_FALLBACK;

	return like_bmh_search(s, slen, search_state->literal,
						 search_state->literal_len, search_state->skip_table);
}

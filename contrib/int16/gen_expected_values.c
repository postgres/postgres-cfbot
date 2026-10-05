/*
 * gen_expected_values.c
 *
 * Standalone program to compute expected values for int16_overflow regression
 * tests using native __int128 arithmetic.  This is NOT compiled as part of the
 * extension — it exists solely as documentation for how the expected output
 * was generated.
 *
 * Build (for reference, not needed for the extension):
 *   gcc -o gen_expected_values gen_expected_values.c
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;

/* INT128_MIN in two's complement = (uint128_t)1 << 127, reinterpreted as signed */
#define INT16_MAX_VAL  ((int128_t)(((uint128_t)1 << 127) - 1))
#define INT16_MIN_VAL  ((int128_t)((uint128_t)1 << 127))
#define TWO_POW_63     ((int128_t)1 << 63)
#define TWO_POW_64     ((int128_t)1 << 64)

static void
print_int128(int128_t v)
{
	if (v == 0)
	{
		printf("0");
		return;
	}
	if (v == INT16_MIN_VAL)
	{
		/* Can't negate INT128_MIN; print the constant directly */
		printf("-170141183460469231731687303715884105728");
		return;
	}
	if (v < 0)
	{
		printf("-");
		v = -v;
	}
	char		buf[40];
	int			i = 0;

	while (v > 0)
	{
		buf[i++] = '0' + (int) (v % 10);
		v /= 10;
	}
	while (i > 0)
		printf("%c", buf[--i]);
}

static int128_t
parse_int128(const char *str)
{
	int128_t	v = 0;
	int			neg = 0;
	const char *p = str;

	if (*p == '-')
	{
		neg = 1;
		p++;
	}
	while (*p)
		v = v * 10 + (*p++ - '0');
	if (neg)
		v = -v;
	return v;
}

static void
print_add(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);
	int128_t	r;

	printf("ADD %s + %s = ", a_str, b_str);
	if (__builtin_add_overflow(a, b, &r))
		printf("OVERFLOW (integer out of range)");
	else
		print_int128(r);
	printf("\n");
}

static void
print_sub(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);
	int128_t	r;

	printf("SUB %s - %s = ", a_str, b_str);
	if (__builtin_sub_overflow(a, b, &r))
		printf("OVERFLOW (integer out of range)");
	else
		print_int128(r);
	printf("\n");
}

static void
print_mul(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);

	printf("MUL %s * %s = ", a_str, b_str);
	if (a == 0 || b == 0)
	{
		printf("0");
	}
	else
	{
		int128_t	r;

		if (__builtin_mul_overflow(a, b, &r))
			printf("OVERFLOW (integer out of range)");
		else
			print_int128(r);
	}
	printf("\n");
}

static void
print_div(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);

	printf("DIV %s / %s = ", a_str, b_str);
	if (b == 0)
		printf("OVERFLOW (division by zero)");
	else if (a == INT16_MIN_VAL && b == -1)
		printf("OVERFLOW (integer out of range)");
	else
		print_int128(a / b);
	printf("\n");
}

static void
print_mod(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);

	printf("MOD %s %% %s = ", a_str, b_str);
	if (b == 0)
		printf("OVERFLOW (division by zero)");
	else if (a == INT16_MIN_VAL && b == -1)
		printf("OVERFLOW (integer out of range)");
	else
		print_int128(a % b);	/* C % follows dividend sign */
	printf("\n");
}

static void
print_neg(const char *a_str)
{
	int128_t	a = parse_int128(a_str);

	printf("NEG -(%s) = ", a_str);
	if (a == INT16_MIN_VAL)
		printf("OVERFLOW (integer out of range)");
	else
		print_int128(-a);
	printf("\n");
}

static void
print_bitand(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);

	printf("AND %s & %s = ", a_str, b_str);
	print_int128(a & b);
	printf("\n");
}

static void
print_bitor(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);

	printf("OR %s | %s = ", a_str, b_str);
	print_int128(a | b);
	printf("\n");
}

static void
print_bitxor(const char *a_str, const char *b_str)
{
	int128_t	a = parse_int128(a_str);
	int128_t	b = parse_int128(b_str);

	printf("XOR %s # %s = ", a_str, b_str);
	print_int128(a ^ b);
	printf("\n");
}

static void
print_bitnot(const char *a_str)
{
	int128_t	a = parse_int128(a_str);

	printf("NOT ~(%s) = ", a_str);
	print_int128(~a);
	printf("\n");
}

static void
print_shl(const char *a_str, int shift)
{
	int128_t	a = parse_int128(a_str);

	printf("SHL %s << %d = ", a_str, shift);
	print_int128(a << shift);
	printf("\n");
}

static void
print_shr(const char *a_str, int shift)
{
	int128_t	a = parse_int128(a_str);

	printf("SHR %s >> %d = ", a_str, shift);
	print_int128(a >> shift);	/* arithmetic shift */
	printf("\n");
}

int
main(void)
{
	printf("=== Addition ===\n");
	print_add("9223372036854775808", "9223372036854775808");	/* 2^63 + 2^63 = 2^64 */
	print_add("-9223372036854775809", "-9223372036854775809");	/* -2^63-1 + -2^63-1 */
	print_add("170141183460469231731687303715884105727", "-170141183460469231731687303715884105728");	/* MAX + MIN */
	print_add("170141183460469231731687303715884105727", "1");	/* overflow */
	print_add("-170141183460469231731687303715884105728", "-1");	/* overflow */
	print_add("18446744073709551616", "0"); /* 2^64 + 0 */
	print_add("-18446744073709551616", "18446744073709551616"); /* -2^64 + 2^64 */
	print_add("9223372036854775807", "9223372036854775808");	/* INT64_MAX + 2^63 */
	print_add("-9223372036854775808", "-9223372036854775808");	/* INT64_MIN + INT64_MIN */
	print_add("85070591730234615865843651857942052864", "85070591730234615865843651857942052864");	/* 2^126 + 2^126 */

	printf("\n=== Subtraction ===\n");
	print_sub("-9223372036854775809", "9223372036854775808");	/* -2^63-1 - 2^63 */
	print_sub("9223372036854775808", "-9223372036854775809");	/* 2^63 - (-2^63-1) */
	print_sub("170141183460469231731687303715884105727", "-1"); /* MAX - (-1) = overflow */
	print_sub("-170141183460469231731687303715884105728", "1"); /* MIN - 1 = overflow */
	print_sub("18446744073709551616", "1"); /* 2^64 - 1 */
	print_sub("0", "-18446744073709551616");	/* 0 - (-2^64) */
	print_sub("170141183460469231731687303715884105727", "170141183460469231731687303715884105726");	/* MAX - (MAX-1) */
	print_sub("-170141183460469231731687303715884105727", "-170141183460469231731687303715884105728");	/* -(MIN+1) - MIN */
	print_sub("9223372036854775808", "9223372036854775807");	/* 2^63 - INT64_MAX */
	print_sub("-9223372036854775809", "-9223372036854775808");	/* -(2^63+1) - INT64_MIN */

	printf("\n=== Multiplication ===\n");
	print_mul("3000000000", "3000000000");	/* 9e18 > INT64_MAX */
	print_mul("-3000000000", "3000000000"); /* -9e18 */
	print_mul("3000000000", "-3000000000"); /* -9e18 */
	print_mul("-3000000000", "-3000000000");	/* 9e18 */
	print_mul("170141183460469231731687303715884105727", "2");	/* overflow */
	print_mul("18446744073709551616", "18446744073709551616");	/* 2^64 * 2^64 = 2^128
																 * overflow */
	print_mul("4294967296", "4294967296");	/* 2^32 * 2^32 = 2^64 */
	print_mul("-4294967296", "4294967296"); /* -2^64 */
	print_mul("9223372036854775807", "9223372036854775807");	/* INT64_MAX^2 */
	print_mul("0", "18446744073709551616"); /* 0 * 2^64 */
	print_mul("18446744073709551616", "1"); /* 2^64 * 1 */
	print_mul("18446744073709551615", "2"); /* (2^64-1) * 2 */

	printf("\n=== Division ===\n");
	print_div("170141183460469231731687303715884105727", "170141183460469231731687303715884105727");	/* MAX / MAX = 1 */
	print_div("170141183460469231731687303715884105727", "-1"); /* MAX / -1 = -MAX */
	print_div("-170141183460469231731687303715884105728", "-1");	/* MIN / -1 = overflow */
	print_div("170141183460469231731687303715884105727", "2");	/* MAX / 2 */
	print_div("-170141183460469231731687303715884105728", "2"); /* MIN / 2 */
	print_div("18446744073709551616", "9223372036854775808");	/* 2^64 / 2^63 = 2 */
	print_div("-18446744073709551616", "9223372036854775808");	/* -2^64 / 2^63 = -2 */
	print_div("18446744073709551616", "-9223372036854775808");	/* 2^64 / -2^63 = -2 */
	print_div("-18446744073709551616", "-9223372036854775808"); /* -2^64 / -2^63 = 2 */
	print_div("170141183460469231731687303715884105727", "9223372036854775807");	/* MAX / INT64_MAX */
	print_div("1", "0");		/* division by zero */

	printf("\n=== Remainder ===\n");
	print_mod("170141183460469231731687303715884105727", "10"); /* MAX % 10 */
	print_mod("-170141183460469231731687303715884105728", "10");	/* MIN % 10 */
	print_mod("170141183460469231731687303715884105727", "-10");	/* MAX % -10 */
	print_mod("-170141183460469231731687303715884105728", "-10");	/* MIN % -10 */
	print_mod("18446744073709551616", "9223372036854775808");	/* 2^64 % 2^63 = 0 */
	print_mod("18446744073709551617", "9223372036854775808");	/* 2^64+1 % 2^63 = 1 */
	print_mod("-18446744073709551617", "9223372036854775808");	/* -(2^64+1) % 2^63 = -1 */
	print_mod("18446744073709551617", "-9223372036854775808");	/* 2^64+1 % -2^63 = 1 */
	print_mod("-18446744073709551617", "-9223372036854775808"); /* -(2^64+1) % -2^63 =
																 * -1 */
	print_mod("1", "0");		/* division by zero */

	printf("\n=== Unary Minus ===\n");
	print_neg("170141183460469231731687303715884105727");	/* -MAX */
	print_neg("-170141183460469231731687303715884105728");	/* -MIN = overflow */
	print_neg("9223372036854775808");	/* -2^63 */
	print_neg("-9223372036854775808");	/* 2^63 */
	print_neg("18446744073709551616");	/* -2^64 */

	printf("\n=== Bitwise AND ===\n");
	print_bitand("18446744073709551616", "18446744073709551616");	/* 2^64 & 2^64 */
	print_bitand("18446744073709551616", "1");	/* 2^64 & 1 = 0 */
	print_bitand("18446744073709551617", "1");	/* 2^64+1 & 1 = 1 */
	print_bitand("-1", "18446744073709551616"); /* -1 & 2^64 = 2^64 */
	print_bitand("-1", "0");	/* -1 & 0 = 0 */
	print_bitand("170141183460469231731687303715884105727", "0");	/* MAX & 0 */
	print_bitand("-170141183460469231731687303715884105728", "0");	/* MIN & 0 */

	printf("\n=== Bitwise OR ===\n");
	print_bitor("18446744073709551616", "1");	/* 2^64 | 1 = 2^64+1 */
	print_bitor("18446744073709551616", "0");	/* 2^64 | 0 = 2^64 */
	print_bitor("0", "18446744073709551616");	/* 0 | 2^64 = 2^64 */
	print_bitor("-1", "0");		/* -1 | 0 = -1 */
	print_bitor("18446744073709551615", "1");	/* (2^64-1) | 1 = 2^64-1 */

	printf("\n=== Bitwise XOR ===\n");
	print_bitxor("18446744073709551616", "1");	/* 2^64 # 1 = 2^64+1 */
	print_bitxor("18446744073709551616", "18446744073709551616");	/* 2^64 # 2^64 = 0 */
	print_bitxor("0", "18446744073709551616");	/* 0 # 2^64 = 2^64 */
	print_bitxor("-1", "0");	/* -1 # 0 = -1 */
	print_bitxor("-1", "18446744073709551616"); /* -1 # 2^64 = -(2^64+1) */
	print_bitxor("-1", "-1");	/* -1 # -1 = 0 */

	printf("\n=== Bitwise NOT ===\n");
	print_bitnot("0");			/* ~0 = -1 */
	print_bitnot("-1");			/* ~(-1) = 0 */
	print_bitnot("18446744073709551616");	/* ~(2^64) */
	print_bitnot("170141183460469231731687303715884105727");	/* ~MAX = MIN */
	print_bitnot("-170141183460469231731687303715884105728");	/* ~MIN = MAX */

	printf("\n=== Left Shift ===\n");
	print_shl("1", 0);			/* 1 */
	print_shl("1", 1);			/* 2 */
	print_shl("1", 64);			/* 2^64 */
	print_shl("1", 127);		/* 2^127 = INT16_MIN */
	print_shl("18446744073709551616", 1);	/* 2^64 << 1 = 2^65 */
	print_shl("18446744073709551616", 63);	/* 2^64 << 63 = 2^127 = INT16_MIN */
	print_shl("-1", 1);			/* -1 << 1 = -2 */
	print_shl("-1", 0);			/* -1 << 0 = -1 */

	printf("\n=== Right Shift ===\n");
	print_shr("-1", 1);			/* -1 >> 1 = -1 (arithmetic) */
	print_shr("-1", 64);		/* -1 >> 64 = -1 (arithmetic) */
	print_shr("18446744073709551616", 1);	/* 2^64 >> 1 = 2^63 */
	print_shr("18446744073709551616", 64);	/* 2^64 >> 64 = 1 */
	print_shr("18446744073709551616", 0);	/* 2^64 >> 0 = 2^64 */
	print_shr("170141183460469231731687303715884105727", 1);	/* MAX >> 1 */
	print_shr("-170141183460469231731687303715884105728", 1);	/* MIN >> 1 */
	print_shr("-170141183460469231731687303715884105728", 64);	/* MIN >> 64 */
	print_shr("0", 10);			/* 0 >> 10 = 0 */

	printf("\n=== Aggregates ===\n");
	/* sum of two 2^63 values = 2^64 */
	int128_t	sum = 0;

	sum += TWO_POW_63;
	sum += TWO_POW_63;
	printf("SUM(2^63, 2^63) = ");
	print_int128(sum);
	printf("\n");

	/* sum with overflow: MAX + 2^63 */
	sum = INT16_MAX_VAL;
	printf("SUM(MAX, 2^63) = ");
	if (__builtin_add_overflow(sum, TWO_POW_63, &sum))
		printf("OVERFLOW (integer out of range)");
	else
		print_int128(sum);
	printf("\n");

	/* avg of (2^63, 2^63) = 2^63 */
	sum = TWO_POW_63 + TWO_POW_63;
	printf("AVG(2^63, 2^63) = ");
	print_int128(sum / 2);
	printf("\n");

	/* avg of (2^64, 2^64, 2^64) = 2^64 */
	sum = TWO_POW_64 + TWO_POW_64 + TWO_POW_64;
	printf("AVG(2^64, 2^64, 2^64) = ");
	print_int128(sum / 3);
	printf("\n");

	return 0;
}

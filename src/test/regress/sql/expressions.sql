--
-- expression evaluation tests that don't fit into a more specific file
--

--
-- Tests for SQLValueFunction
--


-- current_date  (always matches because of transactional behaviour)
SELECT date(now())::text = current_date::text;


-- current_time / localtime
SELECT now()::timetz::text = current_time::text;
SELECT now()::timetz(4)::text = current_time(4)::text;
SELECT now()::time::text = localtime::text;
SELECT now()::time(3)::text = localtime(3)::text;

-- current_time[stamp]/ localtime[stamp] (always matches because of transactional behaviour)
SELECT current_timestamp = NOW();
-- precision
SELECT length(current_timestamp::text) >= length(current_timestamp(0)::text);
-- localtimestamp
SELECT now()::timestamp::text = localtimestamp::text;
-- precision overflow
SELECT current_time = current_time(7);
SELECT current_timestamp = current_timestamp(7);
SELECT localtime = localtime(7);
SELECT localtimestamp = localtimestamp(7);

-- current_role/user/user is tested in rolenames.sql

-- current database / catalog
SELECT current_catalog = current_database();

-- current_schema
SELECT current_schema;
SET search_path = 'notme';
SELECT current_schema;
SET search_path = 'pg_catalog';
SELECT current_schema;
RESET search_path;


--
-- Test parsing of a no-op cast to a type with unspecified typmod
--
begin;

create table numeric_tbl (f1 numeric(18,3), f2 numeric);

create view numeric_view as
  select
    f1, f1::numeric(16,4) as f1164, f1::numeric as f1n,
    f2, f2::numeric(16,4) as f2164, f2::numeric as f2n
  from numeric_tbl;

\d+ numeric_view

explain (verbose, costs off) select * from numeric_view;

-- bpchar, lacking planner support for its length coercion function,
-- could behave differently

create table bpchar_tbl (f1 character(16) unique, f2 bpchar);

create view bpchar_view as
  select
    f1, f1::character(14) as f114, f1::bpchar as f1n,
    f2, f2::character(14) as f214, f2::bpchar as f2n
  from bpchar_tbl;

\d+ bpchar_view

explain (verbose, costs off) select * from bpchar_view
  where f1::bpchar = 'foo';

rollback;


--
-- Ordinarily, IN/NOT IN can be converted to a ScalarArrayOpExpr
-- with a suitably-chosen array type.
--
explain (verbose, costs off)
select random() IN (1, 4, 8.0);
explain (verbose, costs off)
select random()::int IN (1, 4, 8.0);
-- However, if there's not a common supertype for the IN elements,
-- we should instead try to produce "x = v1 OR x = v2 OR ...".
-- In most cases that'll fail for lack of all the requisite = operators,
-- but it can succeed sometimes.  So this should complain about lack of
-- an = operator, not about cast failure.
select '(0,0)'::point in ('(0,0,0,0)'::box, point(0,0));


--
-- Tests for ScalarArrayOpExpr with a hashfn
--

-- create a stable function so that the tests below are not
-- evaluated using the planner's constant folding.
begin;

create function return_int_input(int) returns int as $$
begin
	return $1;
end;
$$ language plpgsql stable;

create function return_text_input(text) returns text as $$
begin
	return $1;
end;
$$ language plpgsql stable;

select return_int_input(1) in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
select return_int_input(1) in (10, 9, 2, 8, 3, 7, 4, 6, 5, null);
select return_int_input(1) in (null, null, null, null, null, null, null, null, null, null, null);
select return_int_input(1) in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1, null);
select return_int_input(null::int) in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
select return_int_input(null::int) in (10, 9, 2, 8, 3, 7, 4, 6, 5, null);
select return_text_input('a') in ('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j');
-- NOT IN
select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 0);
select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 2, null);
select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1, null);
select return_int_input(1) not in (null, null, null, null, null, null, null, null, null, null, null);
select return_int_input(null::int) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
select return_int_input(null::int) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, null);
select return_text_input('a') not in ('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j');

rollback;

--
-- Hashed ScalarArrayOpExpr when the array argument is not a Const but is fixed
-- for the whole execution: external params, IN ($1,...,$N), stable functions.
-- Check the hashed path returns what the linear path does, and that the planner
-- does not hash an array that can vary per row or per group.
--
begin;

create table saop_stab (i int, r int4range);
insert into saop_stab
  select g, int4range(g, g + 1) from generate_series(1, 20) g;

-- a stable plpgsql function is never inlined, so the array stays non-Const
create function saop_intarr(int[]) returns int[] as
  $$ begin return $1; end $$ language plpgsql stable;

-- just below / at / above the hashing threshold of 9
select array_agg(i order by i) from saop_stab where i = any (saop_intarr('{1,2,3,4,5,6,7,8}'));
select array_agg(i order by i) from saop_stab where i = any (saop_intarr('{1,2,3,4,5,6,7,8,9}'));
select array_agg(i order by i) from saop_stab where i = any (saop_intarr('{1,2,3,4,5,6,7,8,9,10,11,12}'));

-- NULL array yields NULL; empty array and no-match array yield no rows
select count(*) from saop_stab where i = any (saop_intarr(null));
select count(*) from saop_stab where i = any (saop_intarr('{}'));
select array_agg(i order by i) from saop_stab where i = any (saop_intarr('{5,5,5,5,5,5,5,5,5,5}'));

-- NOT IN / <> ALL, with and without a NULL element (three-valued logic)
select count(*) from saop_stab where i <> all (saop_intarr('{1,2,3,4,5,6,7,8,9,10}'));
select count(*) from saop_stab where i <> all (saop_intarr('{1,2,3,4,5,6,7,8,9,null}'));

-- bare external Param array, generic plan (stays a Param); re-EXECUTE with a
-- different array, then NULL and empty
set plan_cache_mode = force_generic_plan;
prepare saop_p(int[]) as
  select array_agg(i order by i) from saop_stab where i = any ($1);
execute saop_p('{1,2,3,4,5,6,7,8,9,10}');
execute saop_p('{11,12,13}');
execute saop_p(null);
execute saop_p('{}');
deallocate saop_p;

-- IN ($1, ..., $N) is an ArrayExpr of Params
prepare saop_in(int,int,int,int,int,int,int,int,int,int) as
  select array_agg(i order by i) from saop_stab
  where i in ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10);
execute saop_in(1,2,3,4,5,6,7,8,9,10);
deallocate saop_in;

-- $1::int[] cast, and string_to_array($1, ',')
prepare saop_cast(text) as
  select array_agg(i order by i) from saop_stab where i = any ($1::int[]);
execute saop_cast('{2,4,6,8,10,12,14,16,18,20}');
deallocate saop_cast;
prepare saop_sta(text) as
  select array_agg(i order by i) from saop_stab
  where i::text = any (string_to_array($1, ','));
execute saop_sta('1,2,3,4,5,6,7,8,9,10,11,12');
deallocate saop_sta;

-- same query under a custom plan: $1 folds to a Const and the pre-existing
-- Const path handles it -- must match the generic-plan result above
set plan_cache_mode = force_custom_plan;
prepare saop_c(int[]) as
  select array_agg(i order by i) from saop_stab where i = any ($1);
execute saop_c('{1,2,3,4,5,6,7,8,9,10}');
deallocate saop_c;

-- rescan: a stable Param array on the inner side of a nestloop
set plan_cache_mode = force_generic_plan;
prepare saop_rs(int[]) as
  select d.x, count(*) from (values (1),(2),(3)) d(x)
    join saop_stab on saop_stab.i = any ($1)
  group by d.x order by d.x;
execute saop_rs('{1,2,3,4,5,6,7,8,9,10}');
deallocate saop_rs;
reset plan_cache_mode;

-- two hashable ScalarArrayOpExprs in one qual: both must be applied (cf.
-- b136db07c6) and both correct
prepare saop_two(int[], int[]) as
  select array_agg(i order by i) from saop_stab where i = any ($1) or i = any ($2);
execute saop_two('{1,2,3,4,5,6,7,8,9,10}', '{15,16,17,18,19,20,1,2,3,4}');
deallocate saop_two;

-- the planner must NOT hash an array that varies per row: a Var in the array
-- keeps a plain (linear) ScalarArrayOpExpr
explain (costs off)
select i from saop_stab
where i = any (array[i,i+1,i+2,i+3,i+4,i+5,i+6,i+7,i+8]);

-- ... nor an array_agg() in a HAVING clause (a value per group, not per
-- execution): must not be hashed and must not error with "Aggref found in
-- non-Agg plan node"
select i % 3 as g, count(*) from saop_stab
group by i % 3
having (i % 3) = any (array_agg(1))
order by g;

-- ... nor an array built from an outer-query reference in a correlated
-- sub-select: it varies per rescan, so it must stay linear and give the same
-- answer as the below-threshold (never-hashed) form.  convert_saop_to_hashed_saop
-- runs before uplevel Vars become Params, so the check must reject Vars of any
-- level.
select d.k,
       (select count(*) from saop_stab
        where i = any (array[d.k,d.k+1,d.k+2,d.k+3,d.k+4,d.k+5,d.k+6,d.k+7,d.k+8])) as ge9,
       (select count(*) from saop_stab
        where i = any (array[d.k,d.k+1,d.k+2,d.k+3,d.k+4,d.k+5,d.k+6,d.k+7])) as lt9
from (values (1),(8),(15)) d(k)
order by d.k;

rollback;

-- Test with non-strict equality function.
-- We need to create our own type for this.

begin;

create type myint;
create function myintin(cstring) returns myint strict immutable language
  internal as 'int4in';
create function myintout(myint) returns cstring strict immutable language
  internal as 'int4out';
create function myinthash(myint) returns integer strict immutable language
  internal as 'hashint4';

create type myint (input = myintin, output = myintout, like = int4);

create cast (int4 as myint) without function;
create cast (myint as int4) without function;

create function myinteq(myint, myint) returns bool as $$
begin
  if $1 is null and $2 is null then
    return true;
  else
    return $1::int = $2::int;
  end if;
end;
$$ language plpgsql immutable;

create function myintne(myint, myint) returns bool as $$
begin
  return not myinteq($1, $2);
end;
$$ language plpgsql immutable;

create operator = (
  leftarg    = myint,
  rightarg   = myint,
  commutator = =,
  negator    = <>,
  procedure  = myinteq,
  restrict   = eqsel,
  join       = eqjoinsel,
  merges
);

create operator <> (
  leftarg    = myint,
  rightarg   = myint,
  commutator = <>,
  negator    = =,
  procedure  = myintne,
  restrict   = eqsel,
  join       = eqjoinsel,
  merges
);

create operator class myint_ops
default for type myint using hash as
  operator    1   =  (myint, myint),
  function    1   myinthash(myint);

create table inttest (a myint);
insert into inttest values (null), (0::myint), (1::myint);

-- Test EEOP_HASHED_SCALARARRAYOP against EEOP_SCALARARRAYOP.  Ensure the
-- result of non-hashed vs hashed is the same.
select
  a,
  a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
  a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
from inttest;

select
  a,
  a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
  a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
 from inttest;

select
  a,
  a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
  a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
from inttest;

select
  a,
  a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
  a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
from inttest;

-- Now make the equal function return false when given two NULLs
create or replace function myinteq(myint, myint) returns bool as $$
begin
  if $1 is null and $2 is null then
    return false;
  else
    return $1::int = $2::int;
  end if;
end;
$$ language plpgsql immutable;

-- And try the same again to ensure EEOP_HASHED_SCALARARRAYOP does the same
-- thing as EEOP_SCALARARRAYOP.
select
  a,
  a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
  a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
from inttest;

select
  a,
  a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
  a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
 from inttest;

select
  a,
  a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
  a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
from inttest;

select
  a,
  a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
  a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
from inttest;

-- Try again with an equality function that treats NULLs as equal to 0.
create or replace function myinteq(myint, myint) returns bool as $$
begin
  if $1 is null and $2 is null then
    return false;
  else
    return coalesce($1::int,0) = coalesce($2::int, 0);
  end if;
end;
$$ language plpgsql immutable;

select
  a,
  a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
  a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed,
  a in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed_zero,
  a in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed_zero
from inttest;

select
  a,
  a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
  a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
 from inttest;

select
  a,
  a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
  a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed,
  a not in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed_zero,
  a not in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed_zero
from inttest;

select
  a,
  a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
  a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
from inttest;

rollback;

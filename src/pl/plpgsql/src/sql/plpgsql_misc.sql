--
-- Miscellaneous topics
--

-- Verify that we can parse new-style CREATE FUNCTION/PROCEDURE
do
$$
  declare procedure int;  -- check we still recognize non-keywords as vars
  begin
  create function test1() returns int
    begin atomic
      select 2 + 2;
    end;
  create or replace procedure test2(x int)
    begin atomic
      select x + 2;
    end;
  end
$$;

\sf test1
\sf test2

-- Test %TYPE and %ROWTYPE error cases
create table misc_table(f1 int);

do $$ declare x foo%type; begin end $$;
do $$ declare x notice%type; begin end $$;  -- covers unreserved-keyword case
do $$ declare x foo.bar%type; begin end $$;
do $$ declare x foo.bar.baz%type; begin end $$;
do $$ declare x public.foo.bar%type; begin end $$;
do $$ declare x public.misc_table.zed%type; begin end $$;

do $$ declare x foo%rowtype; begin end $$;
do $$ declare x notice%rowtype; begin end $$;  -- covers unreserved-keyword case
do $$ declare x foo.bar%rowtype; begin end $$;
do $$ declare x foo.bar.baz%rowtype; begin end $$;
do $$ declare x public.foo%rowtype; begin end $$;
do $$ declare x public.misc_table%rowtype; begin end $$;

-- Test handling of an unreserved keyword as a variable name
-- and record field name.
do $$
declare
  execute int;
  r record;
begin
  execute := 10;
  raise notice 'execute = %', execute;
  select 1 as strict into r;
  raise notice 'r.strict = %', r.strict;
end $$;

-- Test handling of a reserved keyword as a record field name.

do $$ declare r record;
begin
  select 1 as x, 2 as foreach into r;
  raise notice 'r.x = %', r.x;
  raise notice 'r.foreach = %', r.foreach;  -- fails
end $$;

do $$ declare r record;
begin
  select 1 as x, 2 as foreach into r;
  raise notice 'r.x = %', r.x;
  raise notice 'r."foreach" = %', r."foreach";  -- ok
end $$;

-- Test ORDER BY ALL in PL/pgSQL contexts
-- This tests the PLpgSQL_Expr grammar rule changes for ORDER BY ALL support

-- Create test table for ORDER BY ALL tests
create table plpgsql_order_test (
  a int,
  b text,
  c float
);

insert into plpgsql_order_test values
  (3, 'foo', 1.5),
  (1, 'bar', 2.5),
  (2, 'baz', 1.5),
  (1, 'qux', 1.5);

-- Test ORDER BY ALL in FOR loop
do $$
declare
  r record;
begin
  raise notice 'FOR loop with ORDER BY ALL:';
  for r in select * from plpgsql_order_test order by all
  loop
    raise notice 'a=%, b=%, c=%', r.a, r.b, r.c;
  end loop;
end $$;

-- Test ORDER BY ALL DESC in FOR loop
do $$
declare
  r record;
begin
  raise notice 'FOR loop with ORDER BY ALL DESC:';
  for r in select * from plpgsql_order_test order by all desc
  loop
    raise notice 'a=%, b=%, c=%', r.a, r.b, r.c;
  end loop;
end $$;

-- Test ORDER BY ALL with cursor
do $$
declare
  cur cursor for select * from plpgsql_order_test order by all;
  r record;
begin
  raise notice 'Cursor with ORDER BY ALL:';
  open cur;
  loop
    fetch cur into r;
    exit when not found;
    raise notice 'a=%, b=%, c=%', r.a, r.b, r.c;
  end loop;
  close cur;
end $$;

-- Test ORDER BY ALL ASC NULLS FIRST in cursor
do $$
declare
  cur cursor for select * from plpgsql_order_test order by all asc nulls first;
  r record;
begin
  raise notice 'Cursor with ORDER BY ALL ASC NULLS FIRST:';
  open cur;
  loop
    fetch cur into r;
    exit when not found;
    raise notice 'a=%, b=%, c=%', r.a, r.b, r.c;
  end loop;
  close cur;
end $$;

-- Test ORDER BY ALL in function with RETURN QUERY
create function test_order_by_all_return()
returns table(a int, b text, c float) as $$
begin
  return query select * from plpgsql_order_test order by all;
end;
$$ language plpgsql;

select * from test_order_by_all_return();

-- Test ORDER BY ALL DESC in function with RETURN QUERY
create function test_order_by_all_desc_return()
returns table(a int, b text, c float) as $$
begin
  return query select * from plpgsql_order_test order by all desc;
end;
$$ language plpgsql;

select * from test_order_by_all_desc_return();

-- Test ORDER BY ALL with subset of columns
do $$
declare
  r record;
begin
  raise notice 'ORDER BY ALL with column subset:';
  for r in select a, b from plpgsql_order_test order by all
  loop
    raise notice 'a=%, b=%', r.a, r.b;
  end loop;
end $$;

-- Test ORDER BY ALL NULLS LAST
do $$
declare
  r record;
begin
  raise notice 'ORDER BY ALL NULLS LAST:';
  for r in select * from plpgsql_order_test order by all nulls last
  loop
    raise notice 'a=%, b=%, c=%', r.a, r.b, r.c;
  end loop;
end $$;

-- Clean up
drop function test_order_by_all_return();
drop function test_order_by_all_desc_return();
drop table plpgsql_order_test;

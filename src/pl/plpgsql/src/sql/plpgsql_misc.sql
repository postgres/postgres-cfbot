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

-- unterminated constructs should complain about the missing terminator
do $$ begin if true $$;
do $$ declare x int; begin x := 1 $$;

-- ORDER BY ALL in PL/pgSQL queries (these go through the PLpgSQL_Expr grammar)
create temp table plpgsql_order_all (a int, b text);
insert into plpgsql_order_all values (3, 'foo'), (1, 'bar'), (2, 'baz'), (1, 'qux');

do $$
declare
  r record;
  cur cursor for select * from plpgsql_order_all order by all desc;
begin
  for r in select * from plpgsql_order_all order by all loop
    raise notice 'for: a=%, b=%', r.a, r.b;
  end loop;
  open cur;
  loop
    fetch cur into r;
    exit when not found;
    raise notice 'cursor: a=%, b=%', r.a, r.b;
  end loop;
  close cur;
end $$;

drop table plpgsql_order_all;

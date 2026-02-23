-- input must be a JSON array
do $$
declare x numeric;
begin
  foreach x in array NULL::jsonb -- fail
  loop
    raise notice '%', x;
  end loop;
end;
$$;

do $$
declare x numeric;
begin
  foreach x in array jsonb '10' -- fail
  loop
    raise notice '%', x;
  end loop;
end;
$$;

do $$
declare x numeric;
begin
  foreach x in array jsonb '{}' -- fail
  loop
    raise notice '%', x;
  end loop;
end;
$$;

-- numeric to numeric
do $$
declare x numeric;
begin
  foreach x in array jsonb '[10,20,30,3.14, null]'
  loop
    raise notice '%', x;
  end loop;
end;
$$;

-- numeric to int by cast
do $$
declare x int;
begin
  foreach x in array jsonb '[10,20,30,3.14, null]'
  loop
    raise notice '%', x;
  end loop;
end;
$$;

-- test of FOUND variable
do $$
declare x int;
begin
  foreach x in array jsonb '[10]'
  loop
    raise notice '%', x;
  end loop;
  raise notice 'FOUND: %', found;
end;
$$;

-- test of FOUND variable
do $$
declare x int;
begin
  foreach x in array jsonb '[]'
  loop
    raise notice '%', x;
  end loop;
  raise notice 'FOUND: %', found;
end;
$$;

-- conversion "3.14" to int should to fail due IO cast
do $$
declare x int;
begin
  foreach x in array jsonb '["10",20,30,"3.14"]'
  loop
    raise notice '%', x;
  end loop;
end;
$$;

do $$
declare x boolean;
begin
  foreach x in array jsonb '[true, false]'
  loop
  if x then
    raise notice 'true';
  else
    raise notice 'false';
  end if;
  end loop;
end;
$$;

-- jsonb to jsonb
do $$
declare x jsonb;
begin
  foreach x in array jsonb '[10,20,30,3.14, null, "Hi"]'
  loop
    raise notice '%', x;
  end loop;
end;
$$;

-- jsonb to json
do $$
declare x json;
begin
  foreach x in array jsonb '[10,20,30,3.14, null, "Hi"]'
  loop
    raise notice '%', x;
  end loop;
end;
$$;

-- iteration over composites
do $$
declare x int; y numeric; z varchar;
begin
  foreach x, y, z in array jsonb '[{}, {"z":"Hi"}, {"y": 3.14}, {"z":"Hi", "x":10, "y":3.14}]'
  loop
    raise notice 'x: %, y: %, z: %', x, y, z;
  end loop;
end;
$$;

create type t3 as (x int, y numeric, z varchar);

do $$
declare c t3;
begin
  foreach c in array jsonb '[{}, {"z":"Hi"}, {"y": 3.14}, {"z":"Hi", "x":10, "y":3.14}]'
  loop
    raise notice 'x: %, y: %, z: %', c.x, c.y, c.z;
  end loop;
end;
$$;

do $$
declare c t3;
begin
  foreach c.x, c.y, c.z in array jsonb '[{}, {"z":"Hi"}, {"y": 3.14}, {"z":"Hi", "x":10, "y":3.14}]'
  loop
    raise notice 'x: %, y: %, z: %', c.x, c.y, c.z;
  end loop;
end;
$$;

drop type t3;

-- target can be a array
do $$
declare x int[];
begin
  foreach x in array jsonb '[[1,2,3],[4,5,6]]'
  loop
    raise notice '% % %', x[1], x[2], x[3];
  end loop;
end;
$$;

do $$
declare x varchar[];
begin
  foreach x in array jsonb '[["Hi","Hello"],["Hello","Hi"]]'
  loop
    raise notice '% %', x[1], x[2];
  end loop;
end;
$$;

do $$
declare x varchar[];
begin
  foreach x in array jsonb '[["Hi","Hello"],["Hello","Hi"]]'
  loop
    raise notice '% %', x[1], x[2];
  end loop;
end;
$$;

do $$
declare x int[]; y varchar;
begin
  foreach x, y in array jsonb '[{"x":[1,2,3], "y":"Hi"}, {"x":[4,5,6], "y":"Hi"}]'
  loop
    raise notice '% % %, y: %', x[1], x[2], x[3], y;
  end loop;
end;
$$;

create type t2 as (x int[], y varchar);

do $$
declare c t2;
begin
  foreach c in array jsonb '[{"x":[1,2,3], "y":"Hi"}, {"x":[4,5,6], "y":"Hi"}]'
  loop
    raise notice '% % %, y: %', c.x[1], c.x[2], c.x[3], c.y;
  end loop;
end;
$$;

drop type t2;

-- EXIT and CONTINUE can be triggered by LOOP_RC_PROCESSING
do $$
declare x int;
begin
  foreach x in array jsonb  '[1,2,3,4,5]'
  loop
    exit when x = 3;
    raise notice '%', x;
  end loop;
end;
$$;

do $$
declare x int;
begin
  foreach x in array jsonb '[1,2,3,4,5]'
  loop
    continue when x % 2 = 0;
    raise notice '%', x;
  end loop;
end;
$$;

-- Variable instead of string
DO $$
declare
  x int;
  arr jsonb;
begin
  select jsonb_agg(i) into arr
    from generate_series(1,3) g(i);

  foreach x in array arr
  loop
    raise notice '%', x;
  end loop;
end;
$$;

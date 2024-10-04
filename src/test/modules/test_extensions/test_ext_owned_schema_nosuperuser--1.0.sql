-- Test extension with owned schema (superuser=false)
CREATE FUNCTION owned1() RETURNS text
  LANGUAGE SQL AS $$ SELECT 'owned1'::text $$;

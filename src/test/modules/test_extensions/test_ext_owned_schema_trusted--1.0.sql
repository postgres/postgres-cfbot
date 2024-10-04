-- Test extension with owned schema (trusted=true)
CREATE FUNCTION owned1() RETURNS text
  LANGUAGE SQL AS $$ SELECT 'owned1'::text $$;

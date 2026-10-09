-- Test extension upgrade script
CREATE FUNCTION owned2() RETURNS text
  LANGUAGE SQL AS $$ SELECT 'owned2'::text $$;

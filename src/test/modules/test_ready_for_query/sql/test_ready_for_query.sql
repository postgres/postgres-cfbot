SHOW ready_for_query_message;
SET ready_for_query_message = 'rich';
SHOW ready_for_query_message;
SET ready_for_query_message = 'plain';
SHOW ready_for_query_message;
SET ready_for_query_message = 'invalid'; -- should fail
CREATE EXTENSION test_ready_for_query;
DROP EXTENSION test_ready_for_query;

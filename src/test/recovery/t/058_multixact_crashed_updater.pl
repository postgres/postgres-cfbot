# Copyright (c) 2026, PostgreSQL Global Development Group

# A crashed updater can remain in a tuple's multixact alongside a prepared
# key-share locker.  The updater never committed, so a later non-key update
# must be able to proceed, including at REPEATABLE READ isolation level.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->start;

$node->safe_psql('postgres', q(
    CREATE TABLE mx_crashed (id int PRIMARY KEY, payload text);
    INSERT INTO mx_crashed VALUES (1, 'initial');
));

# Keep the updater open while another transaction adds a compatible locker.
# PREPARE also flushes the preceding heap update and multixact WAL records.
my $updater = $node->background_psql('postgres');
$updater->query_safe('BEGIN');
$updater->query_safe(q(UPDATE mx_crashed SET payload = 'crashed' WHERE id = 1));

$node->safe_psql('postgres', q(
    BEGIN;
    SELECT id FROM mx_crashed WHERE id = 1 FOR KEY SHARE;
    PREPARE TRANSACTION 'mx_crashed_locker';
));

$node->stop('immediate');
$updater->{run}->finish;
$node->start;

is($node->safe_psql('postgres',
        q(SELECT count(*) FROM pg_prepared_xacts WHERE gid = 'mx_crashed_locker')),
    '1', 'key-share locker survived crash recovery');

my ($stdout, $stderr);
my $ret = $node->psql('postgres', q(
    BEGIN ISOLATION LEVEL REPEATABLE READ;
    UPDATE mx_crashed SET payload = 'after crash' WHERE id = 1;
    COMMIT;
), stdout => \$stdout, stderr => \$stderr);
is($ret, 0, 'repeatable read update ignores the crashed updater')
  or diag($stderr);

is($node->safe_psql('postgres',
        q(SELECT payload FROM mx_crashed WHERE id = 1)),
    'after crash', 'row was updated');

$node->safe_psql('postgres', q(ROLLBACK PREPARED 'mx_crashed_locker'));

done_testing();

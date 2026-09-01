
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests for ALTER SUBSCRIPTION ... REFRESH TABLE, which re-copies one or more
# already-subscribed tables on the subscriber without touching publication
# membership or other tables.  The first version requires the subscription
# to be disabled.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher and subscriber nodes
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf',
	"max_prepared_transactions = 10");
# Several subscriptions coexist below, each needing an apply worker plus table
# synchronization workers.
$node_subscriber->append_conf('postgresql.conf',
	"max_logical_replication_workers = 12");
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# Preexisting content on the publisher: two published tables.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_res   (a int primary key, b text);
	CREATE TABLE tab_other (a int primary key, b text);
	INSERT INTO tab_res   SELECT g, 'p' || g FROM generate_series(1, 100) g;
	INSERT INTO tab_other SELECT g, 'q' || g FROM generate_series(1, 100) g;
	CREATE PUBLICATION tap_pub FOR TABLE tab_res, tab_other;
));

# Matching structure on the subscriber, plus objects used for error cases.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_res   (a int primary key, b text);
	CREATE TABLE tab_other (a int primary key, b text);
	CREATE TABLE tab_local (a int primary key);
	CREATE SEQUENCE seq_local;
));

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);

# Wait for initial sync of both tables.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'100', 'initial sync of tab_res');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_other"),
	'100',
	'initial sync of tab_other');

# A small helper: run SQL expected to fail, and check the error message.
sub refresh_should_fail
{
	my ($sql, $pattern, $desc) = @_;
	my ($ret, $stdout, $stderr) = ('', '', '');
	$ret = $node_subscriber->psql(
		'postgres', $sql,
		stdout => \$stdout,
		stderr => \$stderr);
	ok($ret != 0 && $stderr =~ /$pattern/, $desc)
	  or diag("got ret=$ret stderr=$stderr");
}

# REFRESH TABLE is not allowed while the subscription is enabled.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res",
	qr/not allowed for enabled subscriptions/,
	'REFRESH TABLE rejected while enabled');

# Disable the subscription and wait for its workers to stop.
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION tap_sub DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

# A table that is not part of the subscription is rejected.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_local",
	qr/is not part of the subscription/,
	'REFRESH TABLE rejected for table not in subscription');

# A sequence cannot be refreshed as a table.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE seq_local",
	qr/cannot refresh sequence/,
	'REFRESH TABLE rejected for a sequence');

# The command cannot run inside a transaction block.
refresh_should_fail(
	"BEGIN; ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res;",
	qr/cannot run inside a transaction block/,
	'REFRESH TABLE rejected inside a transaction block');

# All-or-nothing: a list containing one bad table aborts the whole command and
# leaves the valid table untouched.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res, tab_local",
	qr/is not part of the subscription/,
	'REFRESH TABLE with a bad table in the list is rejected');
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_res'"
	),
	'r',
	'valid table not reset when another table in the list is invalid');

# Introduce drift on the subscriber, only in tab_res.
$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_res WHERE a <= 40;
	UPDATE tab_res SET b = 'CORRUPT' WHERE a = 60;
));
is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'60', 'drift introduced in tab_res');

# Record the pre-refresh state of the other table.
my $other_state_before = $node_subscriber->safe_psql('postgres',
	"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_other'"
);
is($other_state_before, 'r', 'tab_other is ready before refresh');

# Resync just tab_res.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res");

# Only tab_res is reset to init; tab_other is untouched; local copy truncated.
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_res'"
	),
	'i',
	'tab_res reset to init state');
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_other'"
	),
	'r',
	'tab_other left untouched (still ready)');
is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'0', 'tab_res truncated locally by REFRESH while disabled');

# Re-enable and wait for the single table to re-copy.
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION tap_sub ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'100', 'tab_res re-copied after enable');
is( $node_subscriber->safe_psql(
		'postgres', "SELECT b FROM tab_res WHERE a = 60"),
	'p60',
	'tab_res corruption repaired by resync');

# Full content matches the publisher.
my $pub_md5 = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
my $sub_md5 = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
is($sub_md5, $pub_md5, 'tab_res matches publisher after resync');

# tab_other was never disturbed.
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_other"),
	'100',
	'tab_other intact throughout');

# Ongoing replication still works for the resynced table.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_res VALUES (101, 'p101')");
$node_publisher->wait_for_catchup('tap_sub');
is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM tab_res WHERE a = 101"),
	'1',
	'streaming resumes on resynced table');

# Multiple tables can be resynced in a single command.  Disable, drift both
# tables, and refresh them together.
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION tap_sub DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_res   WHERE a <= 20;
	DELETE FROM tab_other WHERE a <= 20;
));

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res, tab_other");

# Both listed tables are reset to init and truncated locally.
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT string_agg(srsubstate, ',' ORDER BY c.relname) FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname IN ('tab_other', 'tab_res')"
	),
	'i,i',
	'both listed tables reset to init state');
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT (SELECT count(*) FROM tab_res) + (SELECT count(*) FROM tab_other)"
	),
	'0',
	'both listed tables truncated locally');

# Re-enable and wait for both tables to re-copy.
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION tap_sub ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

my $pub_res = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
my $sub_res = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
is($sub_res, $pub_res, 'tab_res matches publisher after multi-table resync');

my $pub_oth = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_other");
my $sub_oth = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_other");
is($sub_oth, $pub_oth,
	'tab_other matches publisher after multi-table resync');

# A partitioned table holds no data itself, so refreshing it has to truncate
# its partitions as well.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_part (a int primary key, b text) PARTITION BY RANGE (a);
	CREATE TABLE tab_part_1 PARTITION OF tab_part FOR VALUES FROM (1) TO (51);
	CREATE TABLE tab_part_2 PARTITION OF tab_part FOR VALUES FROM (51) TO (101);
	INSERT INTO tab_part SELECT g, 'r' || g FROM generate_series(1, 100) g;
	CREATE PUBLICATION tap_pub_part FOR TABLE tab_part
		WITH (publish_via_partition_root = true);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_part (a int primary key, b text) PARTITION BY RANGE (a);
	CREATE TABLE tab_part_1 PARTITION OF tab_part FOR VALUES FROM (1) TO (51);
	CREATE TABLE tab_part_2 PARTITION OF tab_part FOR VALUES FROM (51) TO (101);
));

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_part CONNECTION '$publisher_connstr' PUBLICATION tap_pub_part"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_part');

is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_part"),
	'100',
	'initial sync of partitioned table');

# Publishing via the root means the subscription tracks the root, not the
# individual partitions.
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT string_agg(c.relname, ',' ORDER BY c.relname) FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 'tap_sub_part'"
	),
	'tab_part',
	'subscription tracks the partitioned root');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_part DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_part' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

$node_subscriber->safe_psql('postgres', "DELETE FROM tab_part WHERE a <= 30");

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_part REFRESH TABLE tab_part");

is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT (SELECT count(*) FROM tab_part_1) + (SELECT count(*) FROM tab_part_2)"
	),
	'0',
	'partitions truncated by REFRESH TABLE on the partitioned root');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_part ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_part');

my $pub_part = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_part");
my $sub_part = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_part");
is($sub_part, $pub_part, 'partitioned table matches publisher after resync');

# Without publish_via_partition_root it is the partitions that are tracked, not
# the root, so naming the root should point the user at them.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_leaf (a int primary key, b text) PARTITION BY RANGE (a);
	CREATE TABLE tab_leaf_1 PARTITION OF tab_leaf FOR VALUES FROM (1) TO (51);
	CREATE TABLE tab_leaf_2 PARTITION OF tab_leaf FOR VALUES FROM (51) TO (101);
	INSERT INTO tab_leaf SELECT g, 's' || g FROM generate_series(1, 100) g;
	CREATE PUBLICATION tap_pub_leaf FOR TABLE tab_leaf;
));
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_leaf (a int primary key, b text) PARTITION BY RANGE (a);
	CREATE TABLE tab_leaf_1 PARTITION OF tab_leaf FOR VALUES FROM (1) TO (51);
	CREATE TABLE tab_leaf_2 PARTITION OF tab_leaf FOR VALUES FROM (51) TO (101);
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_leaf CONNECTION '$publisher_connstr' PUBLICATION tap_pub_leaf"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_leaf');

is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT string_agg(c.relname, ',' ORDER BY c.relname) FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 'tap_sub_leaf'"
	),
	'tab_leaf_1,tab_leaf_2',
	'without publish_via_partition_root the partitions are tracked');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_leaf DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_leaf' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

my ($lret, $lout, $lerr) = ('', '', '');
$lret = $node_subscriber->psql(
	'postgres',
	"ALTER SUBSCRIPTION tap_sub_leaf REFRESH TABLE tab_leaf",
	stdout => \$lout,
	stderr => \$lerr);
ok( $lret != 0
	  && $lerr =~ /is not part of the subscription/
	  && $lerr =~ /partitions individually/,
	'naming an untracked partitioned root points at its partitions'
) or diag("got ret=$lret stderr=$lerr");

# Refreshing one partition must leave the sibling partitions alone.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_leaf REFRESH TABLE tab_leaf_1");
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT (SELECT count(*) FROM tab_leaf_1) || '/' || (SELECT count(*) FROM tab_leaf_2)"
	),
	'0/50',
	'refreshing one partition leaves its siblings alone');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_leaf ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_leaf');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_leaf"),
	'100',
	'refreshed partition re-copied after enable');

# Refreshing an inheritance parent must not discard its children.  A child is an
# independent relation: it may be a subscription member in its own right, in
# which case truncating it here would leave rows that nothing copies back.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_inh_p (a int, b text);
	CREATE TABLE tab_inh_c (a int, b text) INHERITS (tab_inh_p);
	INSERT INTO tab_inh_p VALUES (1, 'parent1'), (2, 'parent2');
	INSERT INTO tab_inh_c VALUES (101, 'child1'), (102, 'child2');
	CREATE PUBLICATION tap_pub_inh FOR TABLE tab_inh_p, tab_inh_c;
));
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_inh_p (a int, b text);
	CREATE TABLE tab_inh_c (a int, b text) INHERITS (tab_inh_p);
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_inh CONNECTION '$publisher_connstr' PUBLICATION tap_pub_inh"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_inh');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM ONLY tab_inh_p"),
	'2',
	'inheritance parent synced');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_inh_c"),
	'2',
	'inheritance child synced');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_inh' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh REFRESH TABLE tab_inh_p");

# Only the parent's own rows are discarded, and only the parent is reset.
is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM ONLY tab_inh_p"),
	'0',
	'inheritance parent truncated by REFRESH TABLE');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_inh_c"),
	'2',
	'inheritance child left intact by REFRESH TABLE');
is( $node_subscriber->safe_psql(
		"postgres",
		"SELECT string_agg(c.relname || '=' || r.srsubstate::text, ',' ORDER BY c.relname) FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 'tap_sub_inh'"
	),
	'tab_inh_c=r,tab_inh_p=i',
	'only the named inheritance parent is reset');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_inh');

# The child must still hold its rows: nothing would have copied them back.
is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM ONLY tab_inh_p"),
	'2',
	'inheritance parent re-copied after enable');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_inh_c"),
	'2',
	'inheritance child not lost by refreshing the parent');

# Naming both refreshes both.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_inh' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh REFRESH TABLE tab_inh_p, tab_inh_c");
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_inh_p"),
	'0',
	'naming parent and child discards both');
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_inh');
is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM ONLY tab_inh_p"),
	'2',
	'parent re-copied after refreshing both');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_inh_c"),
	'2',
	'child re-copied after refreshing both');

# An inheritance parent does not route rows into its children, so another
# subscription tracking only the parent must not block refreshing the child.
# The ancestor test has to be restricted to partitions for this: pg_inherits
# records inheritance and partitioning alike, so walking it unconditionally
# would report the inheritance parent as a feeder of the child.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_inh_ponly FOR TABLE ONLY tab_inh_p");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_inh_ponly CONNECTION '$publisher_connstr' PUBLICATION tap_pub_inh_ponly WITH (copy_data = false, enabled = false, create_slot = false, slot_name = NONE)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_inh' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_inh REFRESH TABLE tab_inh_c");
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT r.srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 'tap_sub_inh' AND c.relname = 'tab_inh_c'"
	),
	'i',
	'inheritance child can be refreshed when another subscription tracks only the parent'
);

$node_subscriber->safe_psql('postgres',
	"DROP SUBSCRIPTION tap_sub_inh_ponly");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_inh_ponly");

# Done with the subscriptions above; free their workers before adding more.
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_part");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_leaf");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_inh");

# A table referenced by a foreign key can only be re-synchronized together with
# the referencing table, because the truncate is rejected on the existence of
# the constraint rather than on the referencing rows.  This is what naming
# several tables in one command is for.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_fk_p (id int primary key);
	CREATE TABLE tab_fk_c (id int primary key, pid int references tab_fk_p(id));
	INSERT INTO tab_fk_p SELECT generate_series(1, 5);
	INSERT INTO tab_fk_c SELECT g, g FROM generate_series(1, 5) g;
	CREATE PUBLICATION tap_pub_fk FOR TABLE tab_fk_p, tab_fk_c;
));
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_fk_p (id int primary key);
	CREATE TABLE tab_fk_c (id int primary key, pid int references tab_fk_p(id));
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_fk CONNECTION '$publisher_connstr' PUBLICATION tap_pub_fk"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_fk');
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_fk DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_fk' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub_fk REFRESH TABLE tab_fk_p",
	qr/cannot truncate a table referenced in a foreign key constraint/,
	'REFRESH TABLE of a foreign key target alone is rejected');

is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_fk_p"),
	'5',
	'the foreign key target keeps its rows after the rejected command');
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT count(*) FILTER (WHERE srsubstate <> 'r') FROM pg_subscription_rel r
		   JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 'tap_sub_fk'"
	),
	'0',
	'no state was reset by the rejected command');

# Naming both succeeds, since they are truncated in one command.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_fk REFRESH TABLE tab_fk_p, tab_fk_c");
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_fk_p"),
	'0',
	'foreign key target discarded when named with its referencing table');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_fk ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_fk');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_fk_p"),
	'5',
	'foreign key target re-copied');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_fk_c"),
	'5',
	'referencing table re-copied');

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_fk");

# A relation that a second subscription also populates cannot be
# re-synchronized: its rows would be discarded while that subscription's state
# stays untouched, so nothing would copy them back.  Refuse instead.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_shared (a int primary key, b text);
	INSERT INTO tab_shared SELECT g, 'v' || g FROM generate_series(1, 20) g;
	CREATE PUBLICATION tap_pub_lo FOR TABLE tab_shared WHERE (a < 10);
	CREATE PUBLICATION tap_pub_hi FOR TABLE tab_shared WHERE (a >= 10);
));
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_shared (a int primary key, b text)");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_lo CONNECTION '$publisher_connstr' PUBLICATION tap_pub_lo"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_hi CONNECTION '$publisher_connstr' PUBLICATION tap_pub_hi"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_lo');
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_hi');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM tab_shared"),
	'20',
	'both subscriptions populated the shared table');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_hi DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_hi' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub_hi REFRESH TABLE tab_shared",
	qr/table "tab_shared" is also part of the subscription "tap_sub_lo"/,
	'REFRESH TABLE rejects a table another subscription also populates');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT count(*) FROM tab_shared"),
	'20',
	'the shared table is left untouched by the rejected command');

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_lo");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_hi");

# The same hazard reaches partitions through tuple routing: one subscription
# tracks the partitioned root, another tracks a partition directly.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_cross (a int primary key, b text) PARTITION BY RANGE (a);
	CREATE TABLE tab_cross_1 PARTITION OF tab_cross FOR VALUES FROM (1) TO (51);
	CREATE TABLE tab_cross_2 PARTITION OF tab_cross FOR VALUES FROM (51) TO (101);
	INSERT INTO tab_cross SELECT g, 'w' || g FROM generate_series(1, 100) g;
	CREATE PUBLICATION tap_pub_croot FOR TABLE tab_cross
	    WITH (publish_via_partition_root = true);
	CREATE PUBLICATION tap_pub_cleaf FOR TABLE tab_cross_1;
));
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_cross (a int primary key, b text) PARTITION BY RANGE (a);
	CREATE TABLE tab_cross_1 PARTITION OF tab_cross FOR VALUES FROM (1) TO (51);
	CREATE TABLE tab_cross_2 PARTITION OF tab_cross FOR VALUES FROM (51) TO (101);
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_croot CONNECTION '$publisher_connstr' PUBLICATION tap_pub_croot"
);
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'tap_sub_croot');
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_cleaf CONNECTION '$publisher_connstr' PUBLICATION tap_pub_cleaf WITH (copy_data = false)"
);
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'tap_sub_cleaf');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_croot DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_croot' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

# Naming the root would truncate the partition owned by the other subscription.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub_croot REFRESH TABLE tab_cross",
	qr/table "tab_cross_1" is also part of the subscription "tap_sub_cleaf"/,
	'REFRESH TABLE rejects a root whose partition another subscription populates'
);

is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_cross"),
	'100',
	'the partitioned table is left untouched by the rejected command');

# And the reverse: naming the partition, whose ancestor the other subscription
# tracks and routes tuples into.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_cleaf DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_cleaf' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub_cleaf REFRESH TABLE tab_cross_1",
	qr/table "tab_cross_1" is a partition of "tab_cross", which is part of the subscription "tap_sub_croot"/,
	'REFRESH TABLE rejects a partition whose ancestor another subscription tracks'
);

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_croot");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_cleaf");

# Re-copying a table is not allowed once two_phase is enabled.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_2pc (a int primary key);
	INSERT INTO tab_2pc SELECT generate_series(1, 10);
	CREATE PUBLICATION tap_pub_2pc FOR TABLE tab_2pc;
));
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_2pc (a int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_2pc CONNECTION '$publisher_connstr' PUBLICATION tap_pub_2pc WITH (two_phase = on)"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub_2pc');

is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT subtwophasestate FROM pg_subscription WHERE subname = 'tap_sub_2pc'"
	),
	'e',
	'two_phase is enabled on tap_sub_2pc');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_2pc DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_2pc' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub_2pc REFRESH TABLE tab_2pc",
	qr/not allowed when two_phase is enabled/,
	'REFRESH TABLE rejected when two_phase is enabled');

# The local copy is discarded, so the caller needs TRUNCATE on the table.
#
# Ownership is transferred while the role is still a superuser, because
# handing a subscription to a non-superuser requires a password in the
# connection string.  The privilege being tested is only checked afterwards.
#
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE ROLE regress_refresh_user LOGIN SUPERUSER;
	GRANT pg_create_subscription TO regress_refresh_user;
	GRANT CREATE ON DATABASE postgres TO regress_refresh_user;
	GRANT ALL ON TABLE tab_2pc TO regress_refresh_user;
	REVOKE TRUNCATE ON TABLE tab_2pc FROM regress_refresh_user;
));
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_2pc SET (two_phase = false)");
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_2pc OWNER TO regress_refresh_user");
$node_subscriber->safe_psql('postgres',
	"ALTER ROLE regress_refresh_user NOSUPERUSER");

my ($acl_ret, $acl_out, $acl_err) = ('', '', '');
$acl_ret = $node_subscriber->psql(
	'postgres',
	"ALTER SUBSCRIPTION tap_sub_2pc REFRESH TABLE tab_2pc",
	extra_params => [ '-U', 'regress_refresh_user' ],
	stdout => \$acl_out,
	stderr => \$acl_err);
ok( $acl_ret != 0 && $acl_err =~ /permission denied/,
	'REFRESH TABLE requires TRUNCATE privilege on the table'
) or diag("got ret=$acl_ret stderr=$acl_err");

$node_subscriber->safe_psql('postgres',
	"GRANT TRUNCATE ON TABLE tab_2pc TO regress_refresh_user");
$node_subscriber->safe_psql(
	'postgres',
	"ALTER SUBSCRIPTION tap_sub_2pc REFRESH TABLE tab_2pc",
	extra_params => [ '-U', 'regress_refresh_user' ]);
is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_2pc"),
	'0', 'REFRESH TABLE accepted once TRUNCATE privilege is granted');

# Dropping tap_sub_2pc has to reach the publisher to remove its slot, which
# its now passwordless non-superuser owner cannot do.
$node_subscriber->safe_psql('postgres',
	"ALTER ROLE regress_refresh_user SUPERUSER");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_2pc");
$node_subscriber->safe_psql('postgres', "DROP OWNED BY regress_refresh_user");
$node_subscriber->safe_psql('postgres', "DROP ROLE regress_refresh_user");
$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();

# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# An UPDATE that FLUX performs out of place (a changed exclusion-constraint
# key, or a row that cannot grow on its full page, moves to a new TID) must
# replicate as an UPDATE, as heap's cross-page UPDATE does.  Were it decoded
# as a DELETE plus an INSERT, a publication WITH (publish = 'update') would
# drop it, and the subscriber's UPDATE triggers would not fire.
#
# Publish a heap table and a FLUX table that receive the same changes, to one
# subscriber with heap tables and one with FLUX tables, and require the FLUX
# table to replicate as the heap one does.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $schema = q{
	CREATE TABLE xt_%T% (id int PRIMARY KEY, r int4range, v int,
		EXCLUDE USING gist (r WITH &&)) USING %AM%;
	CREATE TABLE gt_%T% (id int PRIMARY KEY, t text) USING %AM%
		WITH (fillfactor = 100);
	ALTER TABLE gt_%T% ALTER COLUMN t SET STORAGE PLAIN;
};
my $sub_extra = q{
	CREATE TABLE ops (tab text, op text, id int);
	CREATE FUNCTION log_op() RETURNS trigger LANGUAGE plpgsql AS $$
	BEGIN
		IF TG_OP = 'DELETE' THEN
			INSERT INTO public.ops VALUES (TG_TABLE_NAME, TG_OP, OLD.id);
			RETURN OLD;
		END IF;
		INSERT INTO public.ops VALUES (TG_TABLE_NAME, TG_OP, NEW.id);
		RETURN NEW;
	END $$;
};
my $trig = q{
	CREATE TRIGGER log_op BEFORE INSERT OR UPDATE OR DELETE ON %TAB%
		FOR EACH ROW EXECUTE FUNCTION log_op();
	ALTER TABLE %TAB% ENABLE ALWAYS TRIGGER log_op;
};

sub tables_sql
{
	my ($am) = @_;
	my $sql = '';
	for my $t ('heap', 'flux')
	{
		my $s = $schema;
		$s =~ s/%T%/$t/g;
		$s =~ s/%AM%/$am eq 'pub' ? $t : $am/ge;
		$sql .= $s;
	}
	return $sql;
}

my $pub = PostgreSQL::Test::Cluster->new('publisher');
$pub->init(allows_streaming => 'logical');
$pub->start;
$pub->safe_psql('postgres', tables_sql('pub'));
for my $t ('heap', 'flux')
{
	$pub->safe_psql(
		'postgres', qq{
		INSERT INTO xt_$t VALUES (1, '[1,10)', 0), (2, '[20,30)', 0);
		INSERT INTO gt_$t SELECT g, repeat('a', 100) FROM generate_series(1, 200) g;
	});
}
$pub->safe_psql('postgres',
	"CREATE PUBLICATION pub FOR TABLE xt_heap, xt_flux, gt_heap, gt_flux WITH (publish = 'update')"
);
my $connstr = $pub->connstr . ' dbname=postgres';

my %subs;
for my $am ('heap', 'flux')
{
	my $sub = PostgreSQL::Test::Cluster->new("sub_$am");
	$sub->init;
	$sub->start;
	$sub->safe_psql('postgres', tables_sql($am) . $sub_extra);
	for my $tab ('xt_heap', 'xt_flux', 'gt_heap', 'gt_flux')
	{
		(my $s = $trig) =~ s/%TAB%/$tab/g;
		$sub->safe_psql('postgres', $s);
	}
	$sub->safe_psql('postgres',
		"CREATE SUBSCRIPTION sub_$am CONNECTION '$connstr' PUBLICATION pub WITH (slot_name = sub_$am)"
	);
	$sub->wait_for_subscription_sync($pub, "sub_$am");
	$sub->safe_psql('postgres', 'TRUNCATE ops');
	$subs{$am} = $sub;
}

for my $t ('heap', 'flux')
{
	$pub->safe_psql(
		'postgres', qq{
		UPDATE xt_$t SET r = '[1,11)', v = v + 1 WHERE id = 1;
		UPDATE xt_$t SET id = 3, r = '[20,31)' WHERE id = 2;
		UPDATE gt_$t SET t = repeat('b', 2000) WHERE id = 1;
	});
}
is( $pub->safe_psql(
		'postgres',
		"SELECT (ctid::text::point)[0] > 0 FROM gt_flux WHERE id = 1"),
	't',
	'the grown FLUX row moved to another page');

for my $am ('heap', 'flux')
{
	my $sub = $subs{$am};
	$pub->wait_for_catchup("sub_$am");
	for my $t ('heap', 'flux')
	{
		is( $sub->safe_psql(
				'postgres', "SELECT id, r, v FROM xt_$t ORDER BY id"),
			"1|[1,11)|1\n3|[20,31)|0",
			"$am subscriber: moved rows of xt_$t updated");
		is( $sub->safe_psql(
				'postgres',
				"SELECT id, length(t), left(t, 1) FROM gt_$t WHERE id <= 2 ORDER BY id"
			),
			"1|2000|b\n2|100|a",
			"$am subscriber: grown row of gt_$t updated");
		is( $sub->safe_psql(
				'postgres',
				"SELECT string_agg(op || ' ' || id, ', ' ORDER BY op, id) FROM ops WHERE tab IN ('xt_$t', 'gt_$t')"
			),
			'UPDATE 1, UPDATE 1, UPDATE 3',
			"$am subscriber: only UPDATE triggers fired for $t");
	}
}

done_testing();

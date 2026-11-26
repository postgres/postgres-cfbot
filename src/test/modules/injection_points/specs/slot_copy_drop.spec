# Test that copying a replication slot fails cleanly if the source slot is
# concurrently dropped during the copy, both when an earlier slot array entry
# is free for the destination slot and when copying to the same slot name
# reuses the source slot's array entry.

setup
{
	CREATE EXTENSION injection_points;
}
setup
{
	SELECT FROM pg_create_physical_replication_slot('slot_copy_drop_0', false);
	SELECT FROM pg_create_logical_replication_slot('src_slot', 'pgoutput');
}

teardown
{
	SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
	WHERE slot_name IN ('slot_copy_drop_0', 'src_slot', 'dst_slot');
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT FROM injection_points_set_local();
	SELECT FROM injection_points_attach('replication-slot-create-begin', 'wait');
}
step s1_copy
{
	SELECT slot_name FROM pg_copy_logical_replication_slot('src_slot', 'dst_slot');
}
step s1_copy_same_name
{
	SELECT slot_name FROM pg_copy_logical_replication_slot('src_slot', 'src_slot');
}
step s1_check_slots
{
	SELECT slot_name FROM pg_replication_slots
	WHERE slot_name IN ('src_slot', 'dst_slot');
}
teardown
{
	SELECT FROM injection_points_detach('replication-slot-create-begin');
}

session s2
step s2_drop_both
{
	SELECT FROM pg_drop_replication_slot('slot_copy_drop_0');
	SELECT FROM pg_drop_replication_slot('src_slot');
	SELECT FROM pg_switch_wal();
}
step s2_drop_src
{
	SELECT FROM pg_drop_replication_slot('src_slot');
	SELECT FROM pg_switch_wal();
}
step s2_checkpoint
{
	CHECKPOINT;
}
step s2_switch_wal
{
	SELECT FROM pg_switch_wal();
}
step s2_wakeup
{
	SELECT FROM injection_points_wakeup('replication-slot-create-begin');
}

# Verify that dropping the source slot and recycling its WAL while
# pg_copy_logical_replication_slot() is in progress causes the copy to fail
# cleanly without leaving dst_slot behind when an earlier array entry is free.
permutation
	s1_copy
	s2_drop_both
	s2_checkpoint
	s2_switch_wal
	s2_checkpoint
	s2_wakeup
	s1_check_slots

# Same race when copying to the same slot name so that the destination slot
# reuses the freed source slot's array entry.
permutation
	s1_copy_same_name
	s2_drop_src
	s2_checkpoint
	s2_switch_wal
	s2_checkpoint
	s2_wakeup
	s1_check_slots

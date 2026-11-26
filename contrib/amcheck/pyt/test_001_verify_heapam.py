# Copyright (c) 2021-2026, PostgreSQL Global Development Group

"""Port of contrib/amcheck/t/001_verify_heapam.pl.

Exercises verify_heapam(): an uncorrupted table (and a sequence, which is a heap
under the hood) report nothing across all option combinations, while a table
whose first page has had its line pointers corrupted is reported. Data checksums
are disabled so the hand-corrupted page reads back without a checksum error.
"""

import itertools
import re
import struct

# Each line-pointer check in verify_heapam.c that the corrupted first page is
# expected to trip. The values packed into the page are chosen to hit all of
# them.
LINE_POINTER_ERRORS = [
    r"line pointer redirection to item at offset \d+ precedes minimum offset \d+",
    r"line pointer redirection to item at offset \d+ exceeds maximum offset \d+",
    r"line pointer to page offset \d+ is not maximally aligned",
    r"line pointer length \d+ is less than the minimum tuple header size \d+",
    r"line pointer to page offset \d+ with length \d+ ends beyond maximum page offset \d+",
]


def relpath(node, relname):
    """Returns the filesystem path for the named relation."""
    return node.datadir / node.sql("SELECT pg_relation_filepath($1)", relname)


def verify_heapam(node, relation, **options):
    """Run verify_heapam() on ``relation`` and return the messages it reports.

    Options are given as keyword arguments and bound as query parameters, so
    callers say ``skip="all-visible"`` and nothing here has to quote SQL. Only
    the msg column is selected: the block and offset numbers depend on the
    page layout and nothing here asserts on them.
    """
    args = "".join(f", {name} := ${i}" for i, name in enumerate(options, start=2))
    rows = node.sql(
        f"SELECT msg FROM verify_heapam($1{args})",
        relation,
        *options.values(),
        simplify_result=False,
    )
    return [msg for (msg,) in rows]


def assert_line_pointer_errors(msgs):
    """Assert every line-pointer check fired at least once."""
    for pattern in LINE_POINTER_ERRORS:
        assert any(re.search(pattern, m) for m in msgs), f"no message matched {pattern}"


def fresh_test_table(node, relname):
    """(Re)create and populate a test table of the given name."""
    node.sql_batch(
        f"DROP TABLE IF EXISTS {relname} CASCADE",
        f"CREATE TABLE {relname} (a integer, b text)",
        f"ALTER TABLE {relname} SET (autovacuum_enabled=false)",
        f"ALTER TABLE {relname} ALTER b SET STORAGE external",
        f"INSERT INTO {relname} (a, b) "
        f"(SELECT gs, repeat('b',gs*10) FROM generate_series(1,1000) gs)",
        # A couple of locked/updated rows under savepoints, to exercise
        # multixact and update-chain handling.
        "BEGIN",
        "SAVEPOINT s1",
        f"SELECT 1 FROM {relname} WHERE a = 42 FOR UPDATE",
        f"UPDATE {relname} SET b = b WHERE a = 42",
        "RELEASE s1",
        "SAVEPOINT s1",
        f"SELECT 1 FROM {relname} WHERE a = 42 FOR UPDATE",
        f"UPDATE {relname} SET b = b WHERE a = 42",
        "COMMIT",
    )


def corrupt_first_page(node, relname):
    """Stops the test node, corrupts the first page of the named relation, and
    restarts the node.
    """
    path = relpath(node, relname)
    node.stop()
    with open(path, "r+b") as f:
        # Corrupt some line pointers.  The values are chosen to hit the
        # various line-pointer-corruption checks in verify_heapam.c
        # on both little-endian and big-endian architectures.
        f.seek(32)
        f.write(
            struct.pack(
                "<6L",
                0xAAA15550,
                0xAAA0D550,
                0x00010000,
                0x00008000,
                0x0000800F,
                0x001E8000,
            )
        )
    node.start()


def check_all_options_uncorrupted(node, relname):
    """Check various options are stable (don't abort) and do not report
    corruption when running verify_heapam on an uncorrupted test table.

    The relname *must* be an uncorrupted table, or this will fail.
    """
    combinations = itertools.product(
        (True, False),  # on_error_stop
        (True, False),  # check_toast
        ("none", "all-frozen", "all-visible"),  # skip
        (None, 0),  # startblock
        (None, 0),  # endblock
    )
    for on_error_stop, check_toast, skip, startblock, endblock in combinations:
        options = dict(
            on_error_stop=on_error_stop,
            check_toast=check_toast,
            skip=skip,
            startblock=startblock,
            endblock=endblock,
        )
        assert verify_heapam(node, relname, **options) == [], options


def test_verify_heapam(create_pg):
    #
    # Test set-up
    #
    # Data checksums are off so the hand-corrupted page below reads back
    # without tripping a checksum error first.
    node = create_pg(
        "test", initdb_opts=["--no-data-checksums"], conf={"autovacuum": False}
    )
    node.sql("CREATE EXTENSION amcheck")

    #
    # Check a table with data loaded but no corruption, freezing, etc.
    #
    fresh_test_table(node, "test")
    check_all_options_uncorrupted(node, "test")

    #
    # Check a corrupt table
    #
    fresh_test_table(node, "test")
    corrupt_first_page(node, "test")
    assert_line_pointer_errors(verify_heapam(node, "test"))
    assert_line_pointer_errors(verify_heapam(node, "test", skip="all-visible"))
    assert_line_pointer_errors(verify_heapam(node, "test", skip="all-frozen"))
    assert_line_pointer_errors(verify_heapam(node, "test", check_toast=False))
    assert_line_pointer_errors(verify_heapam(node, "test", startblock=0, endblock=0))

    #
    # Check a corrupt table with all-frozen data
    #
    fresh_test_table(node, "test")
    node.sql("VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) test")
    assert verify_heapam(node, "test") == [], "all-frozen not corrupted table"
    corrupt_first_page(node, "test")
    assert_line_pointer_errors(verify_heapam(node, "test"))

    # Skipping all-frozen pages skips the corrupted (frozen) page.
    assert verify_heapam(node, "test", skip="all-frozen") == [], (
        "all-frozen corrupted table skipping all-frozen"
    )

    #
    # Check a sequence with no corruption.  The current implementation of
    # sequences doesn't require its own test setup, since sequences are really
    # just heap tables under-the-hood.  To guard against future implementation
    # changes made without remembering to update verify_heapam, we create and
    # exercise a sequence, checking along the way that it passes corruption
    # checks.
    #
    # Create a test sequence of the given name.
    node.sql_batch(
        "DROP SEQUENCE IF EXISTS test_seq CASCADE",
        "CREATE SEQUENCE test_seq INCREMENT BY 13 MINVALUE 17 START WITH 23",
        "SELECT nextval('test_seq')",
        "SELECT setval('test_seq', currval('test_seq') + nextval('test_seq'))",
    )
    check_all_options_uncorrupted(node, "test_seq")

    # Call SQL functions to increment the sequence
    node.sql("SELECT nextval('test_seq')")
    check_all_options_uncorrupted(node, "test_seq")

    # Call SQL functions to set the sequence
    node.sql("SELECT setval('test_seq', 102)")
    check_all_options_uncorrupted(node, "test_seq")

    # Call SQL functions to reset the sequence
    node.sql("ALTER SEQUENCE test_seq RESTART WITH 51")
    check_all_options_uncorrupted(node, "test_seq")

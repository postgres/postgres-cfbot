--
-- pgbench-h (TPC-H-inspired) Query 19: Discounted Revenue Query
--

\set b1 format_string('Brand#%d%d', random(1, 5), random(1, 5))
\set b2 format_string('Brand#%d%d', random(1, 5), random(1, 5))
\set b3 format_string('Brand#%d%d', random(1, 5), random(1, 5))
\set q1 random(1, 10)
\set q2 random(10, 20)
\set q3 random(20, 30)

SELECT
    COALESCE(sum(l_extendedprice * (1 - l_discount)), 0) AS revenue
FROM
    lineitem,
    part
WHERE
    (
        p_partkey = l_partkey
        AND p_brand = :b1
        AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
        AND l_quantity >= :q1 AND l_quantity <= :q1 + 10
        AND p_size BETWEEN 1 AND 5
        AND l_shipmode IN ('AIR', 'REG AIR')
        AND l_shipinstruct = 'DELIVER IN PERSON'
    )
    OR
    (
        p_partkey = l_partkey
        AND p_brand = :b2
        AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
        AND l_quantity >= :q2 AND l_quantity <= :q2 + 10
        AND p_size BETWEEN 1 AND 10
        AND l_shipmode IN ('AIR', 'REG AIR')
        AND l_shipinstruct = 'DELIVER IN PERSON'
    )
    OR
    (
        p_partkey = l_partkey
        AND p_brand = :b3
        AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
        AND l_quantity >= :q3 AND l_quantity <= :q3 + 10
        AND p_size BETWEEN 1 AND 15
        AND l_shipmode IN ('AIR', 'REG AIR')
        AND l_shipinstruct = 'DELIVER IN PERSON'
    );

--
-- SELECT EXCLUDE/REPLACE/RENAME
--

SET client_min_messages TO 'warning';

DROP TABLE IF EXISTS users;
DROP TABLE IF EXISTS orders;

RESET client_min_messages;

CREATE TABLE users (
    id          INT PRIMARY KEY,
    name        TEXT,
    email       TEXT,
    created_at  TIMESTAMP
);

CREATE TABLE orders (
    id          INT PRIMARY KEY,
    user_id     INT REFERENCES users(id),
    amount      NUMERIC(10,2),
    status      TEXT
);

CREATE SCHEMA s1;
CREATE SCHEMA s2;

CREATE TABLE s1.products (
    id          INT PRIMARY KEY,
    name        TEXT,
    price       NUMERIC(10,2)
);

CREATE TABLE s2.products (
    id          INT PRIMARY KEY,
    name        TEXT,
    price       NUMERIC(10,2),
    description TEXT
);

-- Insert sample data
INSERT INTO users (id, name, email, created_at) VALUES
(1, 'Alice', 'alice@example.com', '2026-01-01 10:00:00'),
(2, 'Bob', 'bob@example.com', '2026-01-02 11:00:00'),
(3, 'Carol', NULL, '2026-01-03 12:00:00');

INSERT INTO orders (id, user_id, amount, status) VALUES
(101, 1, 50.00, 'paid'),
(102, 1, 75.50, 'shipped'),
(103, 2, 20.00, 'cancelled');

INSERT INTO s1.products (id, name, price) VALUES
(1, 'Product A', 10.00),
(2, 'Product B', 15.50);

INSERT INTO s2.products (id, name, price, description) VALUES
(1, 'Product X', 20.00, 'Description for X'),
(2, 'Product Y', 25.75, 'Description for Y');

--
-- EXCLUDE clause
--
-- Simple EXCLUDE
SELECT * (EXCLUDE (email, created_at))
FROM users
ORDER BY id;

-- Exclude all but one column
SELECT * (EXCLUDE (name, email, created_at))
FROM users
ORDER BY id;

-- Exclude all columns
SELECT * (EXCLUDE (id, name, email, created_at))
FROM users;

-- EXCLUDE all using exclude list but overall SELECT list is not empty
SELECT id, users.* (EXCLUDE (id, name, email, created_at))
FROM users
ORDER BY id;

-- Aliasing with EXCLUDE
SELECT * (EXCLUDE (u.email))
FROM users AS u
ORDER BY u.id;

-- Expressions with EXCLUDE
SELECT * (EXCLUDE (name)), 1 + 1 AS two
FROM users
ORDER BY id;

-- JOINs with EXCLUDE
-- Join, unqualified EXCLUDE
SELECT * (EXCLUDE (created_at))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, qualified EXCLUDE
SELECT * (EXCLUDE (users.created_at, orders.amount))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, aliased tables with EXCLUDE
SELECT * (EXCLUDE (u.created_at, o.amount))
FROM users AS u
JOIN orders AS o ON o.user_id = u.id
ORDER BY u.id, o.id;

-- Qualified stars; EXCLUDE is applied to each star separately
SELECT
users.* (EXCLUDE (id)),
orders.* (EXCLUDE (user_id))
FROM users
LEFT JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Schema-qualified star
SELECT s1.products.* (EXCLUDE (price))
FROM s1.products
ORDER BY id;

-- Schema-qualified column
SELECT * (EXCLUDE (s1.products.price))
FROM s1.products
ORDER BY id;

-- Name collision
SELECT * (EXCLUDE (id))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Subqueries with EXCLUDE
SELECT * (EXCLUDE (u.created_at))
FROM (
    SELECT * FROM users
) u
ORDER BY id;

SELECT * (EXCLUDE (j.created_at))
FROM (
    users JOIN orders USING (id)
) AS j;

-- CTEs with EXCLUDE
WITH base_users AS (
    SELECT * FROM users
)
SELECT * (EXCLUDE (base_users.created_at))
FROM base_users
ORDER BY id;

-- Multiple FROM items with EXCLUDE
SELECT * (EXCLUDE (email, status)) FROM users, orders ORDER BY users.id, orders.id;

-- CROSS JOIN with EXCLUDE
SELECT * (EXCLUDE (email, status)) FROM users CROSS JOIN orders ORDER BY users.id, orders.id;

--
-- REPLACE clause
--
-- Simple REPLACE
SELECT * (REPLACE (email WITH 'user@example.com', created_at WITH NULL))
FROM users
ORDER BY id;

-- Aliasing with REPLACE
SELECT * (REPLACE (u.email WITH 'user@example.com'))
FROM users AS u
ORDER BY u.id;

-- Expressions with REPLACE
SELECT * (REPLACE (created_at WITH NULL)), 1 + 1 AS two
FROM users
ORDER BY id;

-- JOINs with REPLACE
-- Join, unqualified REPLACE
SELECT * (REPLACE (created_at WITH created_at + INTERVAL '1 day'))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, qualified REPLACE
SELECT * (REPLACE (users.created_at WITH NULL, orders.amount WITH orders.amount * 2))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, aliased tables with REPLACE
SELECT * (REPLACE (u.created_at WITH NULL, o.amount WITH o.amount * 2))
FROM users AS u
JOIN orders AS o ON o.user_id = u.id
ORDER BY u.id, o.id;

-- Qualified stars; REPLACE is applied to each star separately
SELECT
users.* (REPLACE (id WITH 999)),
orders.* (REPLACE (user_id WITH NULL))
FROM users
LEFT JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Schema-qualified star
SELECT s1.products.* (REPLACE (price WITH price * 1.1))
FROM s1.products
ORDER BY id;

-- Schema-qualified column
SELECT * (REPLACE (s1.products.price WITH s1.products.price * 1.1))
FROM s1.products
ORDER BY id;

-- Name collision
SELECT * (REPLACE (id WITH 999))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Subqueries with REPLACE
SELECT * (REPLACE (u.created_at WITH u.created_at + INTERVAL '1 day'))
FROM (
    SELECT * FROM users
) u
ORDER BY id;

SELECT * (REPLACE (j.created_at WITH j.created_at + INTERVAL '1 day'))
FROM (
    users JOIN orders USING (id)
) AS j;

-- CTEs with REPLACE
WITH base_users AS (
    SELECT * FROM users
)
SELECT * (REPLACE (base_users.created_at WITH NULL))
FROM base_users
ORDER BY id;

-- Multiple FROM items with REPLACE
SELECT * (REPLACE (email WITH 'user@example.com', status WITH 'N/A')) FROM users, orders ORDER BY users.id, orders.id;

-- CROSS JOIN with REPLACE
SELECT * (REPLACE (email WITH 'user@example.com', status WITH 'N/A')) FROM users CROSS JOIN orders ORDER BY users.id, orders.id;

--
-- RENAME clause
--
-- Simple RENAME
SELECT * (RENAME (name AS user_name, email AS user_email))
FROM users
ORDER BY id;

-- Aliasing with RENAME
SELECT * (RENAME (u.email AS user_email))
FROM users AS u
ORDER BY u.id;

-- Expressions with RENAME
SELECT * (RENAME (created_at AS date_created)), 1 + 1 AS two
FROM users
ORDER BY id;

-- JOINs with RENAME
-- Join, unqualified RENAME
SELECT * (RENAME (created_at AS date_created))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, qualified RENAME
SELECT * (RENAME (users.created_at AS date_created, orders.amount AS order_amount))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, aliased tables with RENAME
SELECT * (RENAME (u.created_at AS date_created, o.amount AS order_amount))
FROM users AS u
JOIN orders AS o ON o.user_id = u.id
ORDER BY u.id, o.id;

-- Qualified stars; RENAME is applied to each star separately
SELECT
users.* (RENAME (id AS uid)),
orders.* (RENAME (user_id AS customer_id))
FROM users
LEFT JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Schema-qualified star
SELECT s1.products.* (RENAME (price AS product_price))
FROM s1.products
ORDER BY id;

-- Schema-qualified column
SELECT * (RENAME (s1.products.price AS product_price))
FROM s1.products
ORDER BY id;

-- Name collision
SELECT * (RENAME (id AS _id))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Subqueries with RENAME
SELECT * (RENAME (u.created_at AS date_created))
FROM (
    SELECT * FROM users
) u
ORDER BY id;

SELECT * (RENAME (j.created_at AS date_created))
FROM (
    users JOIN orders USING (id)
) AS j;

-- CTEs with RENAME
WITH base_users AS (
    SELECT * FROM users
)
SELECT * (RENAME (base_users.created_at AS date_created))
FROM base_users
ORDER BY id;

-- Multiple FROM items with RENAME
SELECT * (RENAME (email AS user_email, status AS order_status)) FROM users, orders ORDER BY users.id, orders.id;

-- CROSS JOIN with RENAME
SELECT * (RENAME (email AS user_email, status AS order_status)) FROM users CROSS JOIN orders ORDER BY users.id, orders.id;

--
-- EXCLUDE/REPLACE/RENAME combinations
--
-- EXCLUDE + REPLACE different columns
SELECT * (EXCLUDE (created_at) REPLACE (email WITH 'user@example.com'))
FROM users;

-- EXCLUDE + RENAME different columns
SELECT * (EXCLUDE (created_at) RENAME (email AS user_email))
FROM users;

-- REPLACE + RENAME on same column
SELECT * (REPLACE (email WITH 'user@example.com', name WITH 'Unknown') RENAME (email AS user_email, name AS user_name))
FROM users
ORDER BY id;

-- REPLACE + RENAME on same column with EXCLUDE
SELECT * (EXCLUDE (created_at) REPLACE (email WITH 'user@example.com') RENAME (email AS user_email))
FROM users;

-- All three clauses on different columns
SELECT * (EXCLUDE (created_at) REPLACE (email WITH 'user@example.com') RENAME (name AS user_name))
FROM users
ORDER BY id;

-- JOIN, unqualified
SELECT * (EXCLUDE (created_at) REPLACE (email WITH 'user@example.com') RENAME (amount AS order_amount))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- JOIN, qualified + aliased tables
SELECT * (EXCLUDE (u.created_at) REPLACE (u.email WITH 'user@example.com') RENAME (o.amount AS order_amount))
FROM users AS u
JOIN orders AS o ON o.user_id = u.id
ORDER BY u.id, o.id;

-- Qualified stars with all three
SELECT
users.* (EXCLUDE (email) REPLACE (name WITH 'Unknown') RENAME (id AS uid)),
orders.* (EXCLUDE (status) REPLACE (amount WITH amount * 2) RENAME (id AS oid))
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Schema-qualified star
SELECT s1.products.* (EXCLUDE (price) REPLACE (name WITH 'Product X') RENAME (id AS product_id))
FROM s1.products
ORDER BY id;

-- Schema-qualified column
SELECT * (EXCLUDE (s1.products.price) REPLACE (s1.products.name WITH 'Product X') RENAME (s1.products.id AS product_id))
FROM s1.products
ORDER BY id;

-- Subquery with all three
SELECT * (EXCLUDE (u.created_at) REPLACE (u.email WITH 'user@example.com') RENAME (u.name AS user_name))
FROM (
    SELECT * FROM users
) AS u
ORDER BY id;

-- CTE with all three
WITH base_users AS (
    SELECT * FROM users
)
SELECT * (EXCLUDE (base_users.created_at) REPLACE (base_users.email WITH 'user@example.com') RENAME (base_users.name AS user_name))
FROM base_users
ORDER BY id;

-- CROSS JOIN with all three
SELECT * (EXCLUDE (status) REPLACE (email WITH 'user@example.com') RENAME (users.id AS uid, orders.id AS oid)) FROM users, orders ORDER BY users.id, orders.id;

-- Test 3-part qualification with JOIN on different schema tables
SELECT * (EXCLUDE (s1.products.name) REPLACE (s2.products.name WITH 'Z') RENAME (s1.products.id AS p1_id, s2.products.id AS p2_id))
FROM s1.products
JOIN s2.products ON s1.products.id = s2.products.id
ORDER BY s1.products.id;

-- Test 4-part qualification
SELECT * (EXCLUDE (regression.public.users.email))
FROM users
ORDER BY id;

-- SELECT INTO with star options
CREATE TABLE tbl_a (a int, b int, c int);
CREATE TABLE tbl_b (c int, d int, e int);
INSERT INTO tbl_a VALUES (1, 2, 3), (4, 5, 6);
INSERT INTO tbl_b VALUES (6, 7, 8), (9, 10, 11);

SELECT * (EXCLUDE (b, e)) INTO tbl_copy
FROM tbl_a
JOIN tbl_b ON tbl_a.c = tbl_b.c
ORDER BY tbl_a.a, tbl_b.c; -- error, as c is in both tables

SELECT * (EXCLUDE (ta.c, tb.d)) INTO tbl_copy
FROM tbl_a AS ta
JOIN tbl_b AS tb ON ta.c = tb.c
ORDER BY ta.a, tb.c;

SELECT * FROM tbl_copy;

-- INSERT INTO ... SELECT with star options
INSERT INTO tbl_copy SELECT * (EXCLUDE (ta.c, tb.d))
FROM tbl_a AS ta
JOIN tbl_b AS tb ON ta.c = tb.c
ORDER BY ta.a, tb.c;

SELECT * FROM tbl_copy;

-- RETURNING with star options
INSERT INTO tbl_copy SELECT * (EXCLUDE (b))
FROM tbl_a
RETURNING * (EXCLUDE (c)); -- error, cannot use star options with RETURNING clause

INSERT INTO tbl_copy SELECT * (EXCLUDE (ta.c, tb.d))
FROM tbl_a AS ta
JOIN tbl_b AS tb ON ta.c = tb.c
RETURNING *;

-- Error cases
-- Non-existent columns (error case)
SELECT * (EXCLUDE (does_not_exist)) FROM users;
SELECT * (REPLACE (does_not_exist WITH 'user@example.com')) FROM users;
SELECT * (RENAME (does_not_exist AS user_email)) FROM users;

-- Empty lists (error case)
SELECT * (EXCLUDE ()) FROM users;
SELECT * (REPLACE ()) FROM users;
SELECT * (RENAME ()) FROM users;

-- duplicate column names (error case)
SELECT * (EXCLUDE (id, id)) FROM users;
SELECT * (REPLACE (email WITH 'user@example.com', email WITH NULL)) FROM users;
SELECT * (RENAME (email AS user_email, email AS user_name)) FROM users;
SELECT * ()
FROM users;

-- EXCLUDE + REPLACE same column (error case)
SELECT * (EXCLUDE (id) REPLACE (id WITH 999))
FROM users;

-- EXCLUDE + RENAME same column (error case)
SELECT * (EXCLUDE (id) RENAME (id AS user_id))
FROM users;

-- Cannot use star inside options list (error case)
SELECT * (EXCLUDE (users.*))
FROM users
ORDER BY id;

-- Incorrect use of table alias (error case)
SELECT u.* (EXCLUDE (users.email)) FROM users u;

-- Test dropping a column
ALTER TABLE users DROP COLUMN email;
SELECT * (EXCLUDE (users.email)) FROM users; -- error, as email column no longer exists
SELECT * (EXCLUDE (name)) FROM users;

-- Apply star options to a column from a different table in a multi-table query (error case)
SELECT
users.* (EXCLUDE (orders.amount))
FROM users, orders;

-- Check privilege handling with star options
CREATE USER tuser;
GRANT SELECT (name) ON TABLE users TO tuser;
SET ROLE tuser;
SELECT * (EXCLUDE (id, created_at)) FROM users; -- should succeed, as id and created_at are excluded
SELECT * (REPLACE (created_at WITH NULL)) FROM users; -- should fail, as created_at is not accessible
SELECT * (RENAME (created_at AS date_created)) FROM users; -- should fail, as created_at is not accessible
RESET ROLE;

-- clean up
DROP TABLE orders;
DROP TABLE users;
DROP TABLE tbl_a;
DROP TABLE tbl_b;
DROP TABLE tbl_copy;
DROP TABLE s1.products;
DROP TABLE s2.products;
DROP SCHEMA s1;
DROP SCHEMA s2;
DROP user tuser;

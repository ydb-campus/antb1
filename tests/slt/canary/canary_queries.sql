-- Self-tests of `antb1-slt queries` and `antb1-slt clickbench` (harness.queries.*, harness.clickbench.*; not a
-- test case): our own queries over the sentinel table of canary/tables.txt. The first one is answered, the second
-- one is pending (Unsupported). A redacted report must never print the SQL (it names the sentinel table) or a
-- value (--mutate canary makes antb1's first value a sentinel).

-- features: count_star, table_name
SELECT COUNT(*) /* ANTB1_CANARY_SQL */ FROM antb1_canary_table;

-- features: recursive_cte, count_star, table_name, derived_table
WITH RECURSIVE countdown(n) AS (SELECT COUNT(*) FROM (SELECT * FROM antb1_canary_table LIMIT 3) AS seed UNION ALL SELECT n - 1 FROM countdown WHERE n > 0) SELECT COUNT(*) FROM countdown;

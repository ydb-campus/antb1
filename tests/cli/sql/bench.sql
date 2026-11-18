SELECT COUNT(*) FROM t;
SELECT SUM(ResolutionWidth), MIN(EventDate) FROM t WHERE IsMobile = 1;

WITH RECURSIVE countdown(n) AS (SELECT COUNT(*) FROM (SELECT * FROM u LIMIT 3) AS seed UNION ALL SELECT n - 1 FROM countdown WHERE n > 0) SELECT COUNT(*) FROM countdown;
SELECT COUNT(*) FROM nope;

SELECT region, COUNT(*) AS n FROM events GROUP BY region HAVING n > 10 AND 5 <= SUM(amount) AND MIN(title) NOT LIKE 'x%' AND region IN ('a', 'b') ORDER BY n DESC LIMIT 3

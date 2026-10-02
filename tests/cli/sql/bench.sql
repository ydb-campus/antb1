SELECT COUNT(*) FROM t;
SELECT SUM(ResolutionWidth), MIN(EventDate) FROM t WHERE IsMobile = 1;

SELECT RegionID, row_number() OVER () FROM u;
SELECT COUNT(*) FROM nope;

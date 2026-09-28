SELECT COUNT(*) FROM t;
SELECT SUM(ResolutionWidth), MIN(EventDate) FROM t WHERE IsMobile = 1;

SELECT RegionID FROM t JOIN u USING (RegionID);
SELECT COUNT(*) FROM nope;

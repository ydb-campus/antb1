-- Our own queries over the columns of ClickBench's hits table (never ClickBench query text) for the ctest
-- data.hits0.slice: `antb1-slt queries` runs each one on antb1 and on DuckDB over the `hits` table (hits_0.parquet,
-- or the files of ANTB1_HITS_FILES) and compares the answers; the output is redacted. The file format and the
-- rules are in tests/slt/runner/query_file.h: a query whose features are all declared in
-- tests/slt/supported_features.h must equal DuckDB's answer; the others are pending (antb1 must answer
-- Unsupported) until a slice PR implements and declares their features. A failure names the query by
-- hits0_slice.sql:<line of the query>.

-- features: count_star, table_name, quoted_identifier
SELECT COUNT(*) FROM "hits";

-- features: count_star, table_name, keyword_case, identifier_case, layout
select count ( * )
from HITS -- every row of every file
;

-- features: count_column, integer_columns, table_name
SELECT COUNT(RefererRegionID) FROM hits;

-- features: count_column, varchar_columns, table_name
SELECT COUNT(MobilePhoneModel) FROM hits;

-- features: sum, integer_columns, table_name
SELECT SUM(IsRefresh) FROM hits;

-- features: sum, integer_columns, table_name
SELECT SUM(WatchID) FROM hits;

-- features: avg, integer_columns, table_name
SELECT AVG(Age) FROM hits;

-- features: min, max, multiple_items, integer_columns, table_name
SELECT MIN(ClientIP), MAX(ClientIP) FROM hits;

-- features: min, max, multiple_items, varchar_columns, table_name
SELECT MIN(BrowserCountry), MAX(BrowserCountry) FROM hits;

-- features: min, count_star, multiple_items, date_columns, integer_columns, where, integer_literal, table_name
SELECT MIN(EventDate), COUNT(*) FROM hits WHERE CounterID = 1000;

-- features: max, date_columns, integer_columns, where, integer_literal, table_name
SELECT MAX(EventDate) FROM hits WHERE IsMobile = 1;

-- features: count_star, where, integer_columns, integer_literal, table_name
SELECT COUNT(*) FROM hits WHERE Age >= 30;

-- features: count_star, where, where_and, integer_columns, integer_literal, table_name
SELECT COUNT(*) FROM hits WHERE IsMobile = 1 AND ResolutionWidth < 800;

-- features: count_star, where, literal_first, integer_columns, integer_literal, table_name
SELECT COUNT(*) FROM hits WHERE 1000 < WindowClientHeight;

-- features: count_star, where, integer_columns, decimal_literal, table_name
SELECT COUNT(*) FROM hits WHERE ResolutionDepth > 23.5;

-- features: count_star, where, integer_columns, integer_literal, negative_literal, table_name
SELECT COUNT(*) FROM hits WHERE UserID < -1000000;

-- features: count_star, where, integer_columns, integer_literal, table_name
SELECT COUNT(*) FROM hits WHERE ResolutionWidth > 100000;

-- features: count_star, where, varchar_columns, string_literal, table_name
SELECT COUNT(*) FROM hits WHERE BrowserLanguage = 'en';

-- features: count_star, where, date_columns, date_literal, table_name
SELECT COUNT(*) FROM hits WHERE EventDate >= DATE '2013-07-20';

-- features: count_star, where, date_columns, string_literal, table_name
SELECT COUNT(*) FROM hits WHERE EventDate < '2013-07-10';

-- features: sum, count_column, multiple_items, where, integer_columns, integer_literal, table_name
SELECT SUM(ParamPrice), COUNT(ParamPrice) FROM hits WHERE ParamPrice > 0;

-- features: sum, alias, integer_columns, table_name
SELECT SUM(HistoryLength) AS total_history FROM hits;

-- features: columns, multiple_items, integer_columns, varchar_columns, where, integer_literal, table_name
SELECT WatchID, Title FROM hits WHERE RegionID = 1000;

-- features: columns, integer_columns, limit, table_name
SELECT CounterID FROM hits LIMIT 10;

-- features: star, limit, table_name
SELECT * FROM hits LIMIT 3;

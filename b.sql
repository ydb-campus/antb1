SELECT COUNT(*) FROM trips WHERE 10 < (SELECT SUM(sh_hours) FROM shifts WHERE shifts.sh_driver = trips.tr_driver)

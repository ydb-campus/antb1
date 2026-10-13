SELECT COUNT(*) FROM zones WHERE NOT EXISTS (SELECT zn_id FROM cities WHERE cities.ct_id = zones.zn_city)

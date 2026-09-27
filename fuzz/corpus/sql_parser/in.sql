SELECT COUNT(*) FROM events WHERE region IN (1, -2, 'x') AND day NOT IN (DATE '2024-01-02')

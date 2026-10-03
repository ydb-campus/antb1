# A stand-in for a query that hangs (harness.tpch.run_redacted.timeout): it logs the query as antb1-slt tpch does,
# then loops until run_redacted.cmake's TIMEOUT stops it. One process, so stopping it leaves nothing behind.
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "Q7: running")
while(TRUE)
endwhile()
message("Q7: unsupported")

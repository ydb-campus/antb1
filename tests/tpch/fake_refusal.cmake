# A stand-in for an unredacted refusal (harness.tpch.run_redacted.refusal): a query's outcome line, then antb1's
# message indented under it, as antb1-slt --show-values prints one (tests/slt/runner/ratchet.cc), with a sentinel.
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "Q1: running")
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "Q1: unsupported (0.001 s)")
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "  antb1_canary_refusal is not supported (line 3, column 19)")

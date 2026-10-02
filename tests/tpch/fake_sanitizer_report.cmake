# A stand-in for a sanitized command (harness.tpch.run_redacted.sanitizer): writes a report with a sentinel to the
# log_path that run_redacted.cmake sets in ASAN_OPTIONS, as ASan would, and exits with 1.
cmake_minimum_required(VERSION 3.29) # cmake_language(EXIT)
if(NOT "$ENV{ASAN_OPTIONS}" MATCHES "log_path=([^:]+)")
  message(FATAL_ERROR "fake_sanitizer_report.cmake: no log_path in ASAN_OPTIONS")
endif()
file(WRITE "${CMAKE_MATCH_1}.4242" "ANTB1_CANARY_REPORT 12345\nSUMMARY: AddressSanitizer: antb1 fake report\n")
cmake_language(EXIT 1)

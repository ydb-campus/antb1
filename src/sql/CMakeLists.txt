antb1_add_module(sql SOURCES lexer.cc parser.cc unparse.cc ast.cc PUBLIC_DEPS common)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(sql SOURCES tests/lexer_test.cc tests/parser_test.cc tests/unparse_test.cc)
endif()

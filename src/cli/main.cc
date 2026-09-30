#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "antb1/cli/cli.h"

#include "allocator.h"

int main(int argc, char** argv) {
  antb1::cli::RestartForAllocator(argv);  // returns unless the process restarts
  try {
    const std::vector<std::string> args(argv, argv + argc);
    return antb1::cli::RunCli(args, std::cin, std::cout, std::cerr);
  } catch (const std::exception& e) {
    std::cerr << "antb1: internal error: " << e.what() << '\n';
  } catch (...) {
    std::cerr << "antb1: internal error: unknown exception\n";
  }
  return antb1::cli::kExitInternal;
}

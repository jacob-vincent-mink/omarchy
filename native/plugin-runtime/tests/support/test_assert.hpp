#pragma once

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace omarchy::plugin_runtime::test_support {

template <typename Operation>
int test_main(Operation operation, std::string_view error_prefix = {}) {
  try {
    return operation();
  } catch (const std::exception &error) {
    std::cerr << error_prefix << error.what() << '\n';
    return 1;
  }
}

namespace exit_assertions {
[[noreturn]] inline void fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(1);
}

inline void require(bool condition, std::string_view message) {
  if (!condition)
    fail(message);
}
} // namespace exit_assertions

template <typename Exception, typename Operation>
bool throws_exception(Operation &&operation) {
  try {
    operation();
  } catch (const Exception &) {
    return true;
  }
  return false;
}

inline void require(bool condition, std::string_view message,
                    std::source_location location =
                        std::source_location::current()) {
  if (!condition)
    throw std::runtime_error(std::string(location.file_name()) + ":" +
                             std::to_string(location.line()) + ": " +
                             std::string(message));
}

} // namespace omarchy::plugin_runtime::test_support

#define OMARCHY_CHECK(...)                                                     \
  ::omarchy::plugin_runtime::test_support::require(                            \
      static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)

#define OMARCHY_TEST_STRINGIFY_IMPL(value) #value
#define OMARCHY_TEST_STRINGIFY(value) OMARCHY_TEST_STRINGIFY_IMPL(value)

// Preserve the caller's failure policy (including exit instead of throwing).
#define OMARCHY_CHECK_WITH(check, ...)                                         \
  (check)(static_cast<bool>((__VA_ARGS__)),                                    \
          __FILE__ ":" OMARCHY_TEST_STRINGIFY(__LINE__) ": " #__VA_ARGS__)

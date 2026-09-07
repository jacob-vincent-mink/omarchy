#pragma once

#include <new>

namespace omarchy::plugin_runtime::channel::detail {

template <typename Error>
auto checked_operation(Error &error, auto failure, auto operation) noexcept
    -> decltype(operation()) {
  error = Error::none;
  try {
    return operation();
  } catch (const std::bad_alloc &) {
    error = Error::resource_exhausted;
  } catch (...) {
    error = Error::internal_failure;
  }
  return failure;
}

} // namespace omarchy::plugin_runtime::channel::detail

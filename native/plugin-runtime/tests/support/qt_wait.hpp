#pragma once

#include "test_assert.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

namespace omarchy::plugin_runtime::test_support {

template <typename State, typename Predicate>
auto locked_predicate(State &state, Predicate predicate) {
  return [&state, predicate = std::move(predicate)]() mutable {
    std::lock_guard lock(state.mutex);
    return predicate();
  };
}

template <typename Predicate>
void await(Predicate predicate, std::string_view message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    std::this_thread::yield();
  }
  require(predicate(), message);
}

template <typename Predicate>
void await_without_ui_dispatch(Predicate predicate, std::string_view message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  require(predicate(), message);
}

template <typename Predicate>
bool awaitFor(std::chrono::milliseconds timeout, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

template <typename Predicate> bool await(Predicate predicate) {
  return awaitFor(std::chrono::seconds(2), std::move(predicate));
}

} // namespace omarchy::plugin_runtime::test_support

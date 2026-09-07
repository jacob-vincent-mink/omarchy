#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omarchy::plugin_runtime::bridge::detail {

inline std::string opaque_id(std::string_view prefix) {
  const auto first = QRandomGenerator::system()->generate64();
  const auto second = QRandomGenerator::system()->generate64();
  return std::string(prefix) + '-' +
         QStringLiteral("%1%2").arg(first, 16, 16, QLatin1Char('0'))
             .arg(second, 16, 16, QLatin1Char('0')).toStdString();
}

enum class OperationState : std::uint8_t { pending, succeeded, failed };

struct OperationRecord {
  std::uint64_t serial = 0;
  std::string id;
  OperationState state = OperationState::pending;
  std::chrono::steady_clock::time_point touched;
  std::string result_json;
  std::string error;
  bool consumed = false;

  QString poll(const QString &operation_id, QJsonObject response = {},
               const QJsonObject *result = nullptr) {
    response.insert(QStringLiteral("operationId"), operation_id);
    switch (state) {
    case OperationState::pending:
      response.insert(QStringLiteral("state"), QStringLiteral("pending"));
      break;
    case OperationState::succeeded:
      response.insert(QStringLiteral("state"), QStringLiteral("succeeded"));
      if (result)
        response.insert(QStringLiteral("result"), *result);
      else if (!result_json.empty())
        response.insert(QStringLiteral("result"), QJsonDocument::fromJson(
            QByteArray::fromStdString(result_json)).object());
      break;
    case OperationState::failed:
      response.insert(QStringLiteral("state"), QStringLiteral("failed"));
      response.insert(QStringLiteral("error"), QString::fromStdString(error));
      break;
    }
    touched = std::chrono::steady_clock::now();
    return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
  }
};

// Only bookkeeping is shared. Callers retain their typed authority records,
// reservation policy, expiry policy, and rules for consuming an operation.
template <typename Operation> class OperationStore {
public:
  OperationStore(std::size_t maximum, std::string_view prefix)
      : maximum_(maximum), prefix_(prefix) { values.reserve(maximum); }

  bool full() const noexcept { return values.size() >= maximum_ || next_serial_ == 0; }

  template <typename Begin>
  std::string start(Begin begin, Operation operation = {}) {
    if (full()) return {};
    std::string id;
    do { id = opaque_id(prefix_); } while (find(QString::fromStdString(id)));
    operation.serial = next_serial_++;
    operation.id = id;
    operation.touched = std::chrono::steady_clock::now();
    const auto serial = operation.serial;
    values.push_back(std::move(operation));
    if (!begin(serial)) {
      erase(serial);
      return {};
    }
    return id;
  }

  Operation *find(const QString &id) noexcept {
    const auto found = std::ranges::find(values, id.toStdString(), &Operation::id);
    return found == values.end() ? nullptr : &*found;
  }
  Operation *find(std::uint64_t serial) noexcept {
    const auto found = std::ranges::find(values, serial, &Operation::serial);
    return found == values.end() ? nullptr : &*found;
  }
  void erase(std::uint64_t serial) noexcept {
    std::erase_if(values, [serial](const Operation &entry) { return entry.serial == serial; });
  }
  template <typename Lifetime> void prune(Lifetime lifetime) noexcept {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(values, [&](const Operation &entry) {
      return now - entry.touched > lifetime(entry);
    });
  }

  std::vector<Operation> values;

private:
  std::size_t maximum_;
  std::string prefix_;
  std::uint64_t next_serial_ = 1;
};

} // namespace omarchy::plugin_runtime::bridge::detail

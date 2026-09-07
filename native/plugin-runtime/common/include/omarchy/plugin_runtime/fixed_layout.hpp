#pragma once

#include "big_endian.hpp"
#include <utility>

namespace omarchy::plugin_runtime {

// Reserved bytes and fixed wire tags belong to the layout, not domain state.
template <typename T, T Expected> struct ConstantField {
  static constexpr T value = Expected;
};

// Select a scalar inside a nested domain value without introducing a second,
// flattened representation solely for serialization.
template <auto Parent, auto Child> struct MemberPath {
  template <typename Record>
  constexpr decltype(auto) operator()(Record &record) const {
    return (record.*Parent).*Child;
  }
};

// A declared member sequence generates packed, big-endian mechanical codecs.
// No native struct padding is serialized. Callers validate span bounds first;
// enum values and all domain/security invariants remain caller responsibilities.
template <typename Record, auto... Members> struct FixedLayout {
  template <auto Member, typename T>
  static constexpr decltype(auto) access(T &record) {
    if constexpr (requires { Member.value; })
      return Member.value;
    else if constexpr (std::is_member_object_pointer_v<decltype(Member)>)
      return record.*Member;
    else
      return Member(record);
  }

  template <auto Member>
  using Value = std::remove_cvref_t<decltype(access<Member>(std::declval<Record &>()))>;

  static constexpr std::size_t size = (sizeof(Value<Members>) + ... + 0);

  template <typename T> static auto bits(T value) {
    if constexpr (std::is_enum_v<T>)
      return static_cast<std::underlying_type_t<T>>(value);
    else
      return value;
  }

  static void encode(const Record &record, std::span<std::byte> bytes) {
    std::size_t offset = 0;
    const auto write = [&]<auto Member>() {
      big_endian::put(bytes, offset, bits(access<Member>(record)));
      offset += sizeof(Value<Member>);
    };
    (write.template operator()<Members>(), ...);
  }

  static Record decode(std::span<const std::byte> bytes) {
    Record record{};
    std::size_t offset = 0;
    const auto read = [&]<auto Member>() {
      using T = Value<Member>;
      using Storage = decltype(bits(T{}));
      if constexpr (!requires { Member.value; })
        access<Member>(record) = static_cast<T>(big_endian::get<Storage>(bytes, offset));
      offset += sizeof(T);
    };
    (read.template operator()<Members>(), ...);
    return record;
  }

  // Call explicitly after the outer length check and before using decoded
  // values. Decoding alone never establishes that a packet is valid.
  static bool matches_constants(std::span<const std::byte> bytes) {
    if (bytes.size() < size)
      return false;
    std::size_t offset = 0;
    bool valid = true;
    const auto check = [&]<auto Member>() {
      using T = Value<Member>;
      if constexpr (requires { Member.value; }) {
        using Storage = decltype(bits(T{}));
        valid = valid && big_endian::get<Storage>(bytes, offset) == bits(Member.value);
      }
      offset += sizeof(T);
    };
    (check.template operator()<Members>(), ...);
    return valid;
  }
};

} // namespace omarchy::plugin_runtime

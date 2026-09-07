#include "test.hpp"

#include "omarchy/plugin_runtime/surface/checked_math.hpp"
#include "omarchy/plugin_runtime/surface/shared_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

using namespace omarchy::plugin_runtime::surface;

int main() {
  const auto allocation =
      make_allocation({.id = 0x0102030405060708ULL, .generation = 9}, 320, 96,
                      320, 96, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, allocation.has_value());
  OMARCHY_CHECK_WITH(require, allocation->stride == 1280 && allocation->frame_bytes == 122880);
  OMARCHY_CHECK_WITH(require, allocation->slot_extent == 126976 &&
              allocation->mapping_bytes == 253952);
  OMARCHY_CHECK_WITH(require, slot_base(*allocation, 0) == 0 &&
              slot_base(*allocation, 1) == allocation->slot_extent &&
              !slot_base(*allocation, 2));

  std::array<std::byte, kSlotHeaderSize> header{};
  OMARCHY_CHECK_WITH(require, encode_slot_header(header, *allocation, 2, 17));
  OMARCHY_CHECK_WITH(require, header[16] == std::byte{0x01} && header[23] == std::byte{0x08});
  const auto decoded = decode_slot_header(header);
  OMARCHY_CHECK_WITH(require, decoded && decoded->sequence == 2 && decoded->frame_sequence == 17 &&
              header_matches_allocation(*decoded, *allocation));
  header[88] = std::byte{1};
  OMARCHY_CHECK_WITH(require, !decode_slot_header(header));

  OMARCHY_CHECK_WITH(require, !make_allocation({.id = 0, .generation = 1}, 1, 1, 1, 1, 1, 1, 4096));
  OMARCHY_CHECK_WITH(require, !make_allocation({.id = 1, .generation = 1}, 1, 1, 4097, 1, 4097, 1,
                           4096));
  OMARCHY_CHECK_WITH(require, !make_allocation({.id = 1, .generation = 1}, 2, 2, 2, 2, 1, 1, 3000));
  OMARCHY_CHECK_WITH(require, !make_allocation({.id = 1, .generation = 1}, 2, 2, 2, 2, 2, 2, 4096));
  OMARCHY_CHECK_WITH(require,
      !checked_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}));
  OMARCHY_CHECK_WITH(require, !checked_multiply(std::numeric_limits<std::uint64_t>::max(),
                            std::uint64_t{2}));

  const auto maximum = make_allocation({.id = 1, .generation = 1}, 4096, 4096,
                                       4096, 4096, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, maximum && maximum->frame_bytes == kMaximumFrameBytes);
  auto inconsistent = *allocation;
  ++inconsistent.mapping_bytes;
  OMARCHY_CHECK_WITH(require, !allocation_is_consistent(inconsistent));

  constexpr std::array<std::uint32_t, 6> dimensions{1, 2, 31, 255, 1024, 4096};
  constexpr std::array<std::uint64_t, 3> page_sizes{8, 4096, 65536};
  for (const auto dimension : dimensions) {
    for (const auto page_size : page_sizes) {
      const auto generated =
          make_allocation({.id = dimension, .generation = page_size}, dimension,
                          dimension, dimension, dimension, 1, 1, page_size);
      OMARCHY_CHECK_WITH(require, generated && allocation_is_consistent(*generated) &&
                  generated->mapping_bytes == generated->slot_extent * 2 &&
                  generated->slot_extent % page_size == 0);
    }
  }
}

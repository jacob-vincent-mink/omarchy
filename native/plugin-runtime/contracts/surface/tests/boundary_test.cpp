#include "test.hpp"

#include "omarchy/plugin_runtime/surface/frame_region.hpp"
#include "omarchy/plugin_runtime/surface/frame_transport.hpp"
#include "omarchy/plugin_runtime/surface/shared_layout.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <vector>

using namespace omarchy::plugin_runtime::surface;

// T06 independent boundary-value cases for the worker-presented frame
// transport. Worker-supplied sizes and handles are untrusted; every call must
// either succeed with an exact, bounded frame or be rejected without touching
// host memory. These probe values omitted from the base frame-region test.

int main() {
  // Baseline allocation and a host mapping large enough for two slots.
  const auto allocation = make_allocation({.id = 7, .generation = 2}, 16, 16,
                                          16, 16, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, allocation.has_value());
  auto region = HostFrameRegion::create(*allocation);
  OMARCHY_CHECK_WITH(require, region.has_value());
  const int worker_fd = region->duplicate_worker_fd();
  OMARCHY_CHECK_WITH(require, worker_fd >= 3);
  void *worker_mapping = mmap(nullptr, allocation->mapping_bytes,
                              PROT_READ | PROT_WRITE, MAP_SHARED, worker_fd, 0);
  OMARCHY_CHECK_WITH(require, worker_mapping != MAP_FAILED);
  const auto mapping = std::span(
      static_cast<std::byte *>(worker_mapping),
      static_cast<std::size_t>(allocation->mapping_bytes));

  // (1) A pixel buffer larger than the exact frame size must be rejected; the
  // host copies only exact frame_bytes, never a worker-chosen length.
  std::vector<std::byte> oversize(allocation->frame_bytes + 1, std::byte{0});
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 2, 7,
                                            oversize) == PublishResult::invalid_frame);
  std::vector<std::byte> undersize(allocation->frame_bytes - 1, std::byte{0});
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 2, 7,
                                            undersize) == PublishResult::invalid_frame);

  // (2) An oddly-numbered slot sequence and a zero frame sequence are not
  // valid single-slot publications.
  std::vector<std::byte> pixels(allocation->frame_bytes, std::byte{0x11});
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 3, 7,
                                            pixels) == PublishResult::invalid_sequence);
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 2, 0,
                                            pixels) == PublishResult::invalid_sequence);

  // (3) A worker may not name a slot beyond the fixed two-slot layout.
  OMARCHY_CHECK_WITH(require, !slot_base(*allocation, 2).has_value());
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 2, 2, 7,
                                            pixels) == PublishResult::invalid_slot);

  // (4) Publish valid frames into both slots, then verify each consumer slot
  // accepts its own slot and rejects a stale replay from the other slot.
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 2, 7,
                       pixels) == PublishResult::published);
  auto consumer = FrameConsumer::create(*allocation);
  OMARCHY_CHECK_WITH(require, consumer.has_value());
  OMARCHY_CHECK_WITH(require, consumer->consume(region->host_mapping(),
                       {.surface = allocation->surface, .slot = 0,
                        .slot_sequence = 2, .frame_sequence = 7}) ==
                       ConsumeResult::accepted);
  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 1, 2, 8,
                       pixels) == PublishResult::published);
  OMARCHY_CHECK_WITH(require, consumer->consume(region->host_mapping(),
                       {.surface = allocation->surface, .slot = 1,
                        .slot_sequence = 2, .frame_sequence = 8}) ==
                       ConsumeResult::accepted);
  OMARCHY_CHECK_WITH(require, consumer->last_frame() &&
                       consumer->last_frame()->frame_sequence == 8);
  // A replay of a previously consumed slot-0 notification must be rejected.
  OMARCHY_CHECK_WITH(require, consumer->consume(region->host_mapping(),
                       {.surface = allocation->surface, .slot = 0,
                        .slot_sequence = 2, .frame_sequence = 7}) ==
                       ConsumeResult::invalid_sequence);

  // (5) A notification bound to a different surface must not be accepted by
  // this consumer.
  const auto foreign = make_allocation({.id = 99, .generation = 1}, 16, 16,
                                       16, 16, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, foreign.has_value());
  OMARCHY_CHECK_WITH(require, consumer->consume(region->host_mapping(),
                       {.surface = foreign->surface, .slot = 0,
                        .slot_sequence = 2, .frame_sequence = 9}) ==
                       ConsumeResult::stale_surface);

  // (6) create() must reject an inconsistent allocation rather than build an
  // over/under-sized consumer.
  auto inconsistent = *allocation;
  inconsistent.frame_bytes += 1;
  OMARCHY_CHECK_WITH(require, !FrameConsumer::create(inconsistent).has_value());

  // (7) initialize_frame_mapping must reject a mapping that cannot hold both
  // slots (the double-buffer requires slot_extent * 2, plus header alignment).
  std::vector<std::byte> one_slot(allocation->slot_extent, std::byte{0});
  OMARCHY_CHECK_WITH(require, !initialize_frame_mapping(std::span(one_slot),
                                                       *allocation));

  OMARCHY_CHECK_WITH(require, munmap(worker_mapping, allocation->mapping_bytes) == 0);
  OMARCHY_CHECK_WITH(require, close(worker_fd) == 0);
}

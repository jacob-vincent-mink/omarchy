#include "test.hpp"

#include "omarchy/plugin_runtime/surface/frame_region.hpp"
#include "omarchy/plugin_runtime/surface/frame_transport.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <vector>

using namespace omarchy::plugin_runtime::surface;

int main() {
  const auto allocation =
      make_allocation({.id = 12, .generation = 1}, 8, 4, 8, 4, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, allocation.has_value());
  auto region = HostFrameRegion::create(*allocation);
  OMARCHY_CHECK_WITH(require, region.has_value());
  auto inconsistent = *allocation;
  ++inconsistent.slot_extent;
  OMARCHY_CHECK_WITH(require, !HostFrameRegion::create(inconsistent));
  const int seals = region->seals();
  OMARCHY_CHECK_WITH(require, (seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) ==
                  (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) &&
              (seals & F_SEAL_WRITE) == 0);
  const int worker_fd = region->duplicate_worker_fd();
  OMARCHY_CHECK_WITH(require, worker_fd >= 3 && (fcntl(worker_fd, F_GETFD) & FD_CLOEXEC) != 0);
  void *worker_mapping = mmap(nullptr, allocation->mapping_bytes,
                              PROT_READ | PROT_WRITE, MAP_SHARED, worker_fd, 0);
  OMARCHY_CHECK_WITH(require, worker_mapping != MAP_FAILED);
  std::vector<std::byte> pixels(allocation->frame_bytes, std::byte{0x42});
  OMARCHY_CHECK_WITH(require, publish_frame({static_cast<std::byte *>(worker_mapping),
                         static_cast<std::size_t>(allocation->mapping_bytes)},
                        *allocation, 0, 2, 1,
                        pixels) == PublishResult::published);
  auto consumer = FrameConsumer::create(*allocation);
  OMARCHY_CHECK_WITH(require, consumer.has_value());
  OMARCHY_CHECK_WITH(require, consumer->consume(region->host_mapping(),
                            {.surface = allocation->surface,
                             .slot = 0,
                             .slot_sequence = 2,
                             .frame_sequence = 1}) == ConsumeResult::accepted);
  OMARCHY_CHECK_WITH(require, consumer->last_frame() && consumer->last_frame()->pixels == pixels);
  OMARCHY_CHECK_WITH(require, munmap(worker_mapping, allocation->mapping_bytes) == 0);
  OMARCHY_CHECK_WITH(require, close(worker_fd) == 0);
}

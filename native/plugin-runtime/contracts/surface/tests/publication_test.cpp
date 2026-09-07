#include "test.hpp"

#include "omarchy/plugin_runtime/surface/frame_transport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace omarchy::plugin_runtime::surface;

struct SequenceMutator {
  SequenceMutator(std::span<std::byte> mapping, CopyStage target,
                  std::uint64_t replacement)
      : mapping_(mapping), target_(target), replacement_(replacement) {}

  static void observe(CopyStage stage, void *context) noexcept {
    auto &self = *static_cast<SequenceMutator *>(context);
    if (stage != self.target_) {
      return;
    }
    auto &word = *reinterpret_cast<std::uint64_t *>(self.mapping_.data());
    std::atomic_ref<std::uint64_t>(word).store(self.replacement_,
                                               std::memory_order_release);
  }

  std::span<std::byte> mapping_;
  CopyStage target_;
  std::uint64_t replacement_;
};

struct ReentrantConsumer {
  static void observe(CopyStage, void *context) noexcept {
    auto &self = *static_cast<ReentrantConsumer *>(context);
    if (self.called) {
      return;
    }
    self.called = true;
    self.result = self.consumer->consume(self.mapping, self.notification);
  }

  FrameConsumer *consumer;
  std::span<const std::byte> mapping;
  FrameReady notification;
  ConsumeResult result = ConsumeResult::accepted;
  bool called = false;
};

int main() {
  const auto allocation =
      make_allocation({.id = 7, .generation = 3}, 8, 4, 8, 4, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, allocation.has_value());
  std::vector<std::byte> mapping(allocation->mapping_bytes);
  OMARCHY_CHECK_WITH(require, initialize_frame_mapping(mapping, *allocation));
  std::vector<std::byte> first(allocation->frame_bytes, std::byte{0x11});
  std::vector<std::byte> second(allocation->frame_bytes, std::byte{0x82});
  auto consumer = FrameConsumer::create(*allocation);
  OMARCHY_CHECK_WITH(require, consumer.has_value());
  const auto consume = [&](std::uint32_t slot, std::uint64_t slot_sequence,
                           std::uint64_t frame_sequence, ConsumeResult expected) {
    OMARCHY_CHECK_WITH(require, consumer->consume(
        mapping, {.surface = allocation->surface, .slot = slot,
                  .slot_sequence = slot_sequence,
                  .frame_sequence = frame_sequence}) == expected);
  };
  auto inconsistent = *allocation;
  ++inconsistent.frame_bytes;
  OMARCHY_CHECK_WITH(require, !FrameConsumer::create(inconsistent));

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 2, 1, first) ==
              PublishResult::published);
  consume(0, 2, 1, ConsumeResult::accepted);
  OMARCHY_CHECK_WITH(require, consumer->last_frame() && consumer->last_frame()->pixels == first);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 1, 4, 2, second) ==
              PublishResult::published);
  consume(1, 4, 2, ConsumeResult::accepted);
  OMARCHY_CHECK_WITH(require, consumer->last_frame()->pixels == second);

  consume(0, 2, 1, ConsumeResult::invalid_sequence);
  OMARCHY_CHECK_WITH(require, consumer->consume(mapping, {.surface = {.id = 7, .generation = 2},
                                      .slot = 0,
                                      .slot_sequence = 2,
                                      .frame_sequence = 3}) ==
              ConsumeResult::stale_surface);

  auto &sequence_word = *reinterpret_cast<std::uint64_t *>(mapping.data());
  std::atomic_ref<std::uint64_t>(sequence_word)
      .store(5, std::memory_order_release);
  consume(0, 6, 3, ConsumeResult::concurrent_write);
  OMARCHY_CHECK_WITH(require, consumer->last_frame()->pixels == second);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 6, 3, first) ==
              PublishResult::published);
  mapping[kSlotPixelOffset] = std::byte{0xff};
  consume(0, 6, 3, ConsumeResult::accepted);
  OMARCHY_CHECK_WITH(require, consumer->last_frame()->pixels.size() == allocation->frame_bytes &&
              consumer->last_frame()->pixels.front() == std::byte{0xff});

  consume(0, 6, 4, ConsumeResult::invalid_sequence);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 8, 4, first) ==
              PublishResult::published);

  mapping[67] ^= std::byte{1};
  consume(0, 8, 4, ConsumeResult::allocation_mismatch);
  OMARCHY_CHECK_WITH(require, consumer->last_frame()->frame_sequence == 3);

  const auto truncated =
      std::span<const std::byte>(mapping).first(mapping.size() - 1);
  OMARCHY_CHECK_WITH(require, consumer->consume(truncated, {.surface = allocation->surface,
                                        .slot = 0,
                                        .slot_sequence = 8,
                                        .frame_sequence = 4}) ==
              ConsumeResult::region_too_small);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 10, 5,
                        std::span(first).first(first.size() - 1)) ==
              PublishResult::invalid_frame);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 12, 6, first) ==
              PublishResult::published);
  SequenceMutator header_mutator(mapping, CopyStage::header_copied, 13);
  auto header_race_consumer = FrameConsumer::create(*allocation);
  OMARCHY_CHECK_WITH(require, header_race_consumer &&
              header_race_consumer->consume(mapping,
                                            {.surface = allocation->surface,
                                             .slot = 0,
                                             .slot_sequence = 12,
                                             .frame_sequence = 6},
                                            SequenceMutator::observe,
                                            &header_mutator) ==
                  ConsumeResult::concurrent_write);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 14, 7, second) ==
              PublishResult::published);
  SequenceMutator pixel_mutator(mapping, CopyStage::pixels_copied, 15);
  auto pixel_race_consumer = FrameConsumer::create(*allocation);
  OMARCHY_CHECK_WITH(require, pixel_race_consumer &&
              pixel_race_consumer->consume(mapping,
                                           {.surface = allocation->surface,
                                            .slot = 0,
                                            .slot_sequence = 14,
                                            .frame_sequence = 7},
                                           SequenceMutator::observe,
                                           &pixel_mutator) ==
                  ConsumeResult::concurrent_write);

  OMARCHY_CHECK_WITH(require, publish_frame(mapping, *allocation, 0, 16, 8, first) ==
              PublishResult::published);
  auto reentry_consumer = FrameConsumer::create(*allocation);
  OMARCHY_CHECK_WITH(require, reentry_consumer.has_value());
  const FrameReady reentry_notification{.surface = allocation->surface,
                                        .slot = 0,
                                        .slot_sequence = 16,
                                        .frame_sequence = 8};
  ReentrantConsumer reentry{.consumer = &*reentry_consumer,
                            .mapping = mapping,
                            .notification = reentry_notification};
  OMARCHY_CHECK_WITH(require, reentry_consumer->consume(mapping, reentry_notification,
                                    ReentrantConsumer::observe,
                                    &reentry) == ConsumeResult::accepted &&
              reentry.called &&
              reentry.result == ConsumeResult::consumer_busy &&
              reentry_consumer->last_frame() &&
              reentry_consumer->last_frame()->pixels == first);
}

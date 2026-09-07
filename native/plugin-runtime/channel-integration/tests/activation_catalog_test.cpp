#include "../../tests/support/temporary_directory.hpp"
#include "../../tests/support/concurrent_mutation.hpp"
#include "../../tests/support/test_assert.hpp"

#include "activation_catalog.hpp"

#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace catalog = omarchy::plugin_runtime::channel;

namespace {

using omarchy::plugin_runtime::test_support::require;

std::string record(std::string_view plugin, char digest = 'a') {
  return "format=omarchy-plugin-activation-v2\nplugin=" + std::string(plugin) +
         "\nrevision-directory=revision\nrevision-sha256=" +
         std::string(64, digest) + "\nstate-directory=" +
         std::string(plugin) +
         "\n";
}

template <typename Value>
concept ExposesParsedRecord = requires(const Value &value) { value.record(); };
template <typename Value>
concept ExposesRecordDescriptor = requires(const Value &value) {
  value.inventory_record_fd();
};
template <typename Value>
concept ExposesRootDescriptor = requires(const Value &value) {
  value.activation_root_fd();
};
template <typename Value>
concept ExposesRecordName = requires(const Value &value) {
  value.record_name();
};
template <typename Value>
concept ExposesChangedAlias = requires(const Value &left,
                                       const Value &right) {
  left.changed_from(right);
};
template <typename Value>
concept ExposesPartialRootEpoch = requires(const Value &left,
                                           const Value &right) {
  left.same_root_epoch(right);
};
static_assert(!ExposesParsedRecord<catalog::ActivationCatalogEntry>);
static_assert(!ExposesRecordDescriptor<catalog::ActivationCatalogEntry>);
static_assert(!ExposesRootDescriptor<catalog::ActivationCatalog>);
static_assert(!ExposesRecordName<catalog::ActivationCatalogEntry>);
static_assert(!ExposesChangedAlias<catalog::ActivationCatalogEntry>);
static_assert(!ExposesChangedAlias<catalog::ActivationCatalog>);
static_assert(!ExposesPartialRootEpoch<catalog::ActivationCatalog>);

class Fixture final : public omarchy::plugin_runtime::test_support::TemporaryDirectory {
public:
  void put(std::string_view name, std::string_view plugin = {}) const {
    put_bytes(name, record(plugin.empty() ? name : plugin));
  }

  void put_bytes(std::string_view name, std::string_view bytes) const {
    write_file(root_ / std::string(name), bytes,
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
  }

  void overwrite(std::string_view name, std::string_view bytes) const {
    write_file(root_ / std::string(name), bytes,
               O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
  }

  void erase(std::string_view name) const {
    OMARCHY_CHECK(std::filesystem::remove(root_ / std::string(name)));
  }

  [[nodiscard]] int open_root() const {
    const int descriptor =
        ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    OMARCHY_CHECK(descriptor >= 0);
    return descriptor;
  }

  [[nodiscard]] const std::filesystem::path &root() const { return root_; }

private:
};

std::unique_ptr<catalog::ActivationCatalog>
load(const Fixture &fixture, catalog::ActivationCatalogError &error,
     std::uint32_t uid = static_cast<std::uint32_t>(::getuid())) {
  const int root = fixture.open_root();
  auto result = catalog::ActivationCatalog::load(root, uid, error);
  ::close(root);
  return result;
}

void descriptor_and_format_rejections_are_typed() {
  {
    catalog::ActivationCatalogError error{};
    auto loaded = catalog::ActivationCatalog::load(
        -1, static_cast<std::uint32_t>(::getuid()), error);
    OMARCHY_CHECK(!loaded &&
                error == catalog::ActivationCatalogError::root_untrusted);
  }
  {
    Fixture fixture;
    fixture.put("org.example.valid");
    const int record_fd = ::open((fixture.root() / "org.example.valid").c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    OMARCHY_CHECK(record_fd >= 0);
    catalog::ActivationCatalogError error{};
    auto loaded = catalog::ActivationCatalog::load(
        record_fd, static_cast<std::uint32_t>(::getuid()), error);
    ::close(record_fd);
    OMARCHY_CHECK(!loaded &&
                error == catalog::ActivationCatalogError::root_untrusted);
  }

  for (const std::string &invalid_name :
       {std::string("Org.example.upper"), std::string("org..example"),
        std::string("org.example-"), std::string(129, 'a')}) {
    Fixture fixture;
    fixture.put(invalid_name);
    catalog::ActivationCatalogError error{};
    auto loaded = load(fixture, error);
    OMARCHY_CHECK(!loaded &&
                error ==
                    catalog::ActivationCatalogError::unexpected_entry);
  }

  for (const std::string_view malformed :
       {std::string_view{},
        std::string_view("format=omarchy-plugin-activation-v2\nplugin=")}) {
    Fixture fixture;
    fixture.put_bytes("org.example.malformed", malformed);
    catalog::ActivationCatalogError error{};
    auto loaded = load(fixture, error);
    OMARCHY_CHECK(!loaded &&
                error == catalog::ActivationCatalogError::invalid_record);
  }
}

void valid_catalog_is_opaque_stable_and_sorted() {
  Fixture fixture;
  fixture.put("org.example.zeta");
  fixture.put("org.example.alpha");
  catalog::ActivationCatalogError error{};
  auto loaded = load(fixture, error);
  OMARCHY_CHECK(loaded && error == catalog::ActivationCatalogError::none &&
              loaded->entries().size() == 2);
  OMARCHY_CHECK(loaded->entries()[0].plugin_id() == "org.example.alpha" &&
              loaded->entries()[1].plugin_id() == "org.example.zeta");
  for (const auto &entry : loaded->entries()) {
    OMARCHY_CHECK(!entry.plugin_id().empty());
  }
  OMARCHY_CHECK(loaded->unchanged());

  auto same = load(fixture, error);
  OMARCHY_CHECK(same && same->unchanged() && loaded->same_epoch(*same) &&
              loaded->entries()[0].same_epoch(same->entries()[0]) &&
              loaded->entries()[1].same_epoch(same->entries()[1]));
}

void successful_scans_report_every_inventory_change() {
  {
    Fixture fixture;
    fixture.put("org.example.first");
    catalog::ActivationCatalogError error{};
    auto before = load(fixture, error);
    fixture.put("org.example.second");
    auto after = load(fixture, error);
    OMARCHY_CHECK(before && after && after->entries().size() == 2 &&
                !before->unchanged() && !before->same_epoch(*after));
  }
  {
    Fixture fixture;
    fixture.put("org.example.first");
    fixture.put("org.example.second");
    catalog::ActivationCatalogError error{};
    auto before = load(fixture, error);
    fixture.erase("org.example.second");
    auto after = load(fixture, error);
    OMARCHY_CHECK(before && after && after->entries().size() == 1 &&
                !before->unchanged() && !before->same_epoch(*after));
  }
  {
    Fixture fixture;
    fixture.put("org.example.first");
    catalog::ActivationCatalogError error{};
    auto before = load(fixture, error);
    fixture.overwrite("org.example.first", record("org.example.first", 'b'));
    auto after = load(fixture, error);
    OMARCHY_CHECK(before && after && !before->unchanged() &&
                !before->same_epoch(*after) &&
                !before->entries()[0].same_epoch(after->entries()[0]));
  }
  {
    Fixture fixture;
    fixture.put("org.example.first");
    catalog::ActivationCatalogError error{};
    auto before = load(fixture, error);
    fixture.erase("org.example.first");
    fixture.put("org.example.first");
    auto after = load(fixture, error);
    OMARCHY_CHECK(before && after && !before->same_epoch(*after) &&
                !before->entries()[0].same_epoch(after->entries()[0]));
  }
}

void failed_scan_never_replaces_the_callers_last_good_catalog() {
  Fixture fixture;
  fixture.put("org.example.stable");
  catalog::ActivationCatalogError error{};
  auto last_good = load(fixture, error);
  OMARCHY_CHECK(last_good && last_good->entries().size() == 1);
  const int unexpected =
      ::open((fixture.root() / ".staging").c_str(),
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  OMARCHY_CHECK(unexpected >= 0);
  ::close(unexpected);
  auto rejected = load(fixture, error);
  OMARCHY_CHECK(!rejected &&
              error == catalog::ActivationCatalogError::unexpected_entry &&
              last_good->entries().size() == 1 &&
              last_good->entries()[0].plugin_id() == "org.example.stable" &&
              !last_good->unchanged());
}

void metadata_and_entry_rejections_are_transactional() {
  const auto rejected = [](catalog::ActivationCatalogError expected, auto mutate) {
    Fixture fixture;
    mutate(fixture);
    catalog::ActivationCatalogError error{};
    OMARCHY_CHECK(!load(fixture, error) && error == expected);
  };
  rejected(catalog::ActivationCatalogError::root_untrusted, [](Fixture &fixture) {
    fixture.put("org.example.valid");
    OMARCHY_CHECK(::chmod(fixture.root().c_str(), 0755) == 0);
  });
  {
    Fixture fixture;
    fixture.put("org.example.valid");
    catalog::ActivationCatalogError error{};
    OMARCHY_CHECK(!load(fixture, error, static_cast<std::uint32_t>(::getuid()) + 1) &&
                error == catalog::ActivationCatalogError::root_untrusted);
  }
  rejected(catalog::ActivationCatalogError::invalid_record, [](Fixture &fixture) {
    fixture.put("org.example.valid");
    OMARCHY_CHECK(::chmod((fixture.root() / "org.example.valid").c_str(), 0644) == 0);
  });
  {
    Fixture fixture;
    fixture.put("org.example.valid");
    std::filesystem::create_hard_link(fixture.root() / "org.example.valid",
                                      fixture.root() / "org.example.alias");
    catalog::ActivationCatalogError error{};
    OMARCHY_CHECK(!load(fixture, error));
  }
  rejected(catalog::ActivationCatalogError::unexpected_entry, [](Fixture &fixture) {
    fixture.put("org.example.target");
    std::filesystem::create_symlink("org.example.target",
                                    fixture.root() / "org.example.link");
  });
  rejected(catalog::ActivationCatalogError::unexpected_entry, [](Fixture &fixture) {
    fixture.put("org.example.valid");
    std::filesystem::create_directory(fixture.root() / "org.example.dir");
  });
  rejected(catalog::ActivationCatalogError::unexpected_entry, [](Fixture &fixture) {
    fixture.put("org.example.valid");
    OMARCHY_CHECK(::mkfifo((fixture.root() / "org.example.fifo").c_str(), 0600) == 0);
  });
  rejected(catalog::ActivationCatalogError::unexpected_entry, [](Fixture &fixture) {
    fixture.put("org.example.valid");
    const int unknown = ::open((fixture.root() / ".staging").c_str(),
                               O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    OMARCHY_CHECK(unknown >= 0);
    ::close(unknown);
  });
  rejected(catalog::ActivationCatalogError::invalid_record, [](Fixture &fixture) {
    fixture.put("org.example.actual", "org.example.other");
  });
  {
    Fixture fixture;
    fixture.put("org.example.first");
    fixture.put("org.example.alias", "org.example.first");
    catalog::ActivationCatalogError error{};
    OMARCHY_CHECK(!load(fixture, error));
  }
}

void bounds_and_mutation_fail_closed() {
  {
    Fixture fixture;
    for (std::size_t index = 0;
         index <= catalog::kMaximumActivationCatalogEntries; ++index) {
      fixture.put("org.example.plugin" + std::to_string(index));
    }
    catalog::ActivationCatalogError error{};
    OMARCHY_CHECK(!load(fixture, error) &&
                error == catalog::ActivationCatalogError::bound_exceeded);
  }
  for (const bool directory_change : {false, true}) {
    Fixture fixture;
    fixture.put("org.example.valid");
    const auto path = fixture.root() /
                      (directory_change ? "churn" : "org.example.valid");
    const bool rejected = omarchy::plugin_runtime::test_support::observe_concurrent_mutation(
        [&] {
          if (directory_change) {
            const int descriptor =
                ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (descriptor >= 0) ::close(descriptor);
            ::unlink(path.c_str());
          } else {
            ::chmod(path.c_str(), 0400);
            ::chmod(path.c_str(), 0600);
          }
        },
        [&] {
          catalog::ActivationCatalogError error{};
          return !load(fixture, error);
        }, 100, 100);
    if (directory_change) std::filesystem::remove(path);
    OMARCHY_CHECK(rejected);
  }
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    valid_catalog_is_opaque_stable_and_sorted();
    successful_scans_report_every_inventory_change();
    failed_scan_never_replaces_the_callers_last_good_catalog();
    descriptor_and_format_rejections_are_typed();
    metadata_and_entry_rejections_are_transactional();
    bounds_and_mutation_fail_closed();
    return 0;
  }, "activation catalog test failed: ");
}

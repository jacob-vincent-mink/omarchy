#include "omarchy/plugin_runtime/manifest_identifier.hpp"
#include "omarchy/plugin_runtime/canonical_sha256.hpp"
#include "manifest_contract.hpp"
#include "manifest_json.hpp"

#include <QCryptographicHash>
#include <nlohmann/json.hpp>

#include "omarchy/plugin/wire/surface_name.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>

namespace omarchy::plugins::manifest {
namespace {
using omarchy::plugin_runtime::canonical_manifest_identifier;
using omarchy::plugin_runtime::canonical_sha256;

namespace wire = omarchy::plugin::wire;

using namespace std::literals;

constexpr std::size_t kMaximumFiles = 4096;
constexpr std::uint64_t kMaximumTreeBytes = 64ULL * 1024ULL * 1024ULL;

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

using Json = nlohmann::json;
using Object = Json::object_t;
using Array = Json::array_t;

std::string canonical(const Json &json) { return json.dump(); }

template <typename Value>
std::conditional_t<std::is_same_v<Value, std::int64_t>, Value, const Value &>
as(const Json &json, std::string_view field) {
  static_assert(std::is_same_v<Value, Object> || std::is_same_v<Value, Array> ||
                std::is_same_v<Value, std::string> || std::is_same_v<Value, std::int64_t>);
  constexpr auto kind = std::is_same_v<Value, Object> ? "an object"
      : std::is_same_v<Value, Array> ? "an array"
      : std::is_same_v<Value, std::string> ? "a string" : "an integer";
  const auto *value = json.get_ptr<const Value *>();
  require(value != nullptr, std::string(field) + " must be " + kind);
  return *value;
}

const Json &required(const Object &object, std::string_view key) {
  const auto found = object.find(key);
  require(found != object.end(),
          std::string("missing required field ") + std::string(key));
  return found->second;
}

void known_keys(const Object &object,
                std::initializer_list<std::string_view> allowed,
                std::string_view context) {
  for (const auto &[key, unused] : object) {
    (void)unused;
    require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(),
            std::string("unknown ") + std::string(context) + " field " + key);
  }
}

void bounded_text(std::string_view value, std::size_t maximum,
                  std::string_view field) {
  require(!value.empty() && value.size() <= maximum,
          std::string(field) + " has invalid length");
  require(value.find('\0') == std::string_view::npos,
          std::string(field) + " contains NUL");
}

bool valid_setting_key(std::string_view value) {
  if (value.empty() || value.size() > 128 ||
      !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= 'A' && value.front() <= 'Z'))) {
    return false;
  }
  return std::ranges::all_of(value, [](const unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
  });
}

SettingValue setting_value(const Json &json, std::string_view field) {
  if (const auto *value = json.get_ptr<const bool *>())
    return *value;
  if (const auto *value = json.get_ptr<const std::int64_t *>())
    return *value;
  if (const auto *value = json.get_ptr<const std::string *>()) {
    bounded_text(*value, 4096, field);
    return *value;
  }
  fail(std::string(field) + " must be a boolean, integer, or string");
}

bool valid_setting_value(const SettingDefinition &definition,
                         const SettingValue &value) {
  switch (definition.type) {
  case SettingType::boolean:
    return std::holds_alternative<bool>(value);
  case SettingType::integer: {
    const auto *integer = std::get_if<std::int64_t>(&value);
    return integer != nullptr && definition.minimum && definition.maximum &&
           definition.step && *integer >= *definition.minimum &&
           *integer <= *definition.maximum;
  }
  case SettingType::enumeration: {
    const auto *text = std::get_if<std::string>(&value);
    return text != nullptr &&
           std::ranges::find(definition.options, *text) !=
               definition.options.end();
  }
  }
  return false;
}

bool safe_relative_path(std::string_view value) {
  if (value.empty() || value.size() > 4096 || value.front() == '/' ||
      value.find('\\') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) {
    return false;
  }
  std::filesystem::path path(value);
  for (const auto &part : path) {
    if (part == "." || part == ".." || part.empty())
      return false;
  }
  return path.lexically_normal().generic_string() == value;
}

class Sha256 {
public:
  void update(std::span<const std::byte> bytes) {
    hash_.addData(QByteArrayView(reinterpret_cast<const char *>(bytes.data()),
                                static_cast<qsizetype>(bytes.size())));
  }
  void update(std::string_view bytes) {
    update(std::as_bytes(std::span(bytes)));
  }
  std::string finish() { return hash_.result().toHex().toStdString(); }

private:
  QCryptographicHash hash_{QCryptographicHash::Sha256};
};

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::byte, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = std::byte((value >> ((7 - index) * 8)) & 0xff);
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, value.size());
  hash.update(value);
}

std::vector<CapabilityRequest>
canonical_requests(std::vector<CapabilityRequest> requests) {
  for (auto &request : requests)
    std::ranges::sort(request.operations);
  std::sort(requests.begin(), requests.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.capability, left.required,
                              left.canonical_scope,
                              left.definition_generation,
                              left.definition_digest, left.operations) <
                     std::tie(right.capability, right.required,
                              right.canonical_scope,
                              right.definition_generation,
                              right.definition_digest, right.operations);
            });
  return requests;
}

std::string fingerprint_requests(const std::vector<CapabilityRequest> &input) {
  const auto requests = canonical_requests(input);
  Sha256 hash;
  // A dynamic request's trusted definition and operation set are authority,
  // not display metadata, and therefore participate in consent identity.
  hash.update("OMARCHY-PLUGIN-REQUESTS-V2\0"sv);
  hash_u64(hash, requests.size());
  for (const auto &request : requests) {
    const std::byte requirement{request.required ? std::uint8_t{1}
                                                 : std::uint8_t{0}};
    hash.update(std::span(&requirement, 1));
    hash_field(hash, request.capability);
    hash_field(hash, request.canonical_scope);
    hash_u64(hash, request.definition_generation);
    hash_field(hash, request.definition_digest);
    hash_u64(hash, request.operations.size());
    for (const auto &operation : request.operations)
      hash_field(hash, operation);
  }
  return hash.finish();
}

} // namespace

ManifestV2 parse_manifest_v2(std::string_view bytes) {
  const Json document = detail::parse_manifest_json(bytes);
  const Object &root = as<Object>(document, "manifest");
  known_keys(root,
             {"schemaVersion", "id", "name", "version", "description",
              "author", "license", "homepage", "repository", "keywords",
              "runtime", "surfaces", "settings", "permissions", "dependencies"},
             "manifest");
  require(as<std::int64_t>(required(root, "schemaVersion"), "schemaVersion") == 2,
          "unsupported schemaVersion");

  ManifestV2 result;
  result.id = as<std::string>(required(root, "id"), "id");
  require(canonical_manifest_identifier(result.id), "invalid plugin id");
  result.name = as<std::string>(required(root, "name"), "name");
  bounded_text(result.name, 256, "name");
  result.version = as<std::string>(required(root, "version"), "version");
  bounded_text(result.version, 128, "version");
  if (const auto found = root.find("description"); found != root.end()) {
    result.description = as<std::string>(found->second, "description");
    require(result.description.size() <= 4096, "description is too long");
  }
  for (const auto &[key, maximum, destination] :
       std::array<std::tuple<std::string_view, std::size_t, std::string *>, 4>{
           {{"author", 256, &result.author},
            {"license", 128, &result.license},
            {"homepage", 2048, &result.homepage},
            {"repository", 2048, &result.repository}}}) {
    if (const auto found = root.find(key); found != root.end()) {
      *destination = as<std::string>(found->second, key);
      bounded_text(*destination, maximum, key);
      if (key == "homepage" || key == "repository")
        require(destination->starts_with("https://"),
                std::string(key) + " must use https");
    }
  }
  if (const auto found = root.find("keywords"); found != root.end()) {
    const auto &keywords = as<Array>(found->second, "keywords");
    require(keywords.size() <= 32, "too many keywords");
    for (const auto &keyword : keywords) {
      auto value = as<std::string>(keyword, "keyword");
      bounded_text(value, 64, "keyword");
      require(std::ranges::find(result.keywords, value) ==
                  result.keywords.end(),
              "duplicate keyword");
      result.keywords.push_back(std::move(value));
    }
  }

  if (const auto found = root.find("dependencies"); found != root.end()) {
    const auto &dependencies = as<Object>(found->second, "dependencies");
    known_keys(dependencies, {"aur"}, "dependencies");
    const auto &packages = as<Array>(required(dependencies, "aur"), "dependencies.aur");
    require(packages.size() <= 32, "too many AUR dependencies");
    for (const auto &package : packages) {
      auto name = as<std::string>(package, "AUR dependency");
      const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      };
      require(!name.empty() && name.size() <= 128 && alnum(name.front()) &&
                  std::ranges::all_of(name, [&](char c) {
                    return alnum(c) || c == '@' || c == '.' || c == '_' ||
                           c == '+' || c == '-';
                  }),
              "invalid AUR dependency name");
      require(std::ranges::find(result.aur_dependencies, name) ==
                  result.aur_dependencies.end(), "duplicate AUR dependency");
      result.aur_dependencies.push_back(std::move(name));
    }
  }

  const Object &runtime = as<Object>(required(root, "runtime"), "runtime");
  known_keys(runtime, {"apiVersion", "qml", "surfaceQml"},
             "runtime");
  const auto api_version =
      as<std::int64_t>(required(runtime, "apiVersion"), "runtime.apiVersion");
  require(api_version == 1, "unsupported runtime.apiVersion");
  result.runtime.api_version = static_cast<std::uint32_t>(api_version);
  result.runtime.qml = as<std::string>(required(runtime, "qml"), "runtime.qml");
  require(safe_relative_path(result.runtime.qml), "unsafe runtime.qml path");
  if (const auto found = runtime.find("surfaceQml"); found != runtime.end()) {
    const Object &entries = as<Object>(found->second, "runtime.surfaceQml");
    require(!entries.empty() &&
                entries.size() <= wire::kMaximumPluginSurfaces,
            "runtime.surfaceQml has invalid length");
    for (const auto &[surface, entry] : entries) {
      require(wire::valid_surface_name(surface),
              "runtime.surfaceQml surface has invalid wire name");
      auto qml = as<std::string>(entry, "runtime.surfaceQml entry");
      require(safe_relative_path(qml),
              "unsafe runtime.surfaceQml entry path");
      result.runtime.surface_qml.push_back({surface, std::move(qml)});
    }
  }
  const Json &surfaces = required(root, "surfaces");
  const Object &surface_entries = as<Object>(surfaces, "surfaces");
  require(surface_entries.size() <= wire::kMaximumPluginSurfaces,
          "too many declared surfaces");
  for (const auto &[surface, ignored] : surface_entries) {
    (void)ignored;
    require(wire::valid_surface_name(surface),
            "surface name has invalid wire name");
    result.surface_names.push_back(surface);
  }
  for (const auto &entry : result.runtime.surface_qml) {
    require(surface_entries.contains(entry.surface),
            "runtime.surfaceQml names an undeclared surface");
  }
  if (!result.runtime.surface_qml.empty()) {
    require(result.runtime.surface_qml.size() == surface_entries.size(),
            "runtime.surfaceQml must cover every declared surface");
  }
  result.canonical_surfaces = canonical(surfaces);

  if (const auto found = root.find("settings"); found != root.end()) {
    const auto &settings = as<Object>(found->second, "settings");
    known_keys(settings, {"defaults", "schema"}, "settings");
    const Json &defaults_json = required(settings, "defaults");
    const auto &defaults = as<Object>(defaults_json, "settings.defaults");
    const auto &schema = as<Array>(required(settings, "schema"),
                                  "settings.schema");
    require(!schema.empty() && schema.size() <= 64 &&
                defaults.size() == schema.size(),
            "settings has invalid length");
    for (const auto &[key, value] : defaults) {
      require(valid_setting_key(key), "invalid settings default key");
      result.settings.defaults.emplace(
          key, setting_value(value, "settings default"));
    }
    std::set<std::string, std::less<>> keys;
    for (const auto &item : schema) {
      const auto &definition = as<Object>(item, "settings schema entry");
      known_keys(definition,
                 {"key", "type", "label", "description", "min", "max",
                  "step", "options", "defaultValue"},
                 "settings schema entry");
      SettingDefinition parsed;
      parsed.key = as<std::string>(required(definition, "key"), "settings key");
      require(valid_setting_key(parsed.key), "invalid settings schema key");
      require(keys.insert(parsed.key).second, "duplicate settings schema key");
      parsed.label =
          as<std::string>(required(definition, "label"), "settings label");
      bounded_text(parsed.label, 256, "settings label");
      if (const auto description = definition.find("description");
          description != definition.end()) {
        parsed.description =
            as<std::string>(description->second, "settings description");
        require(parsed.description.size() <= 1024,
                "settings description is too long");
      }
      const auto type =
          as<std::string>(required(definition, "type"), "settings type");
      parsed.default_value = setting_value(
          required(definition, "defaultValue"), "settings defaultValue");
      if (type == "boolean") {
        parsed.type = SettingType::boolean;
        require(!definition.contains("min") && !definition.contains("max") &&
                    !definition.contains("step") &&
                    !definition.contains("options"),
                "boolean setting has incompatible constraints");
      } else if (type == "integer") {
        parsed.type = SettingType::integer;
        parsed.minimum = as<std::int64_t>(required(definition, "min"),
                                    "settings minimum");
        parsed.maximum = as<std::int64_t>(required(definition, "max"),
                                    "settings maximum");
        parsed.step = as<std::int64_t>(required(definition, "step"),
                                 "settings step");
        require(*parsed.minimum <= *parsed.maximum && *parsed.step > 0 &&
                    !definition.contains("options"),
                "integer setting has invalid constraints");
      } else if (type == "enum") {
        parsed.type = SettingType::enumeration;
        require(!definition.contains("min") && !definition.contains("max") &&
                    !definition.contains("step"),
                "enum setting has incompatible constraints");
        const auto &options =
            as<Array>(required(definition, "options"), "settings options");
        require(!options.empty() && options.size() <= 64,
                "enum setting has invalid option count");
        for (const auto &option : options) {
          auto value = as<std::string>(option, "settings option");
          bounded_text(value, 256, "settings option");
          require(std::ranges::find(parsed.options, value) ==
                      parsed.options.end(),
                  "duplicate settings option");
          parsed.options.push_back(std::move(value));
        }
      } else {
        fail("unsupported settings type");
      }
      const auto default_value = result.settings.defaults.find(parsed.key);
      require(default_value != result.settings.defaults.end() &&
                  default_value->second == parsed.default_value &&
                  valid_setting_value(parsed, parsed.default_value),
              "settings default does not satisfy schema");
      result.settings.schema.push_back(std::move(parsed));
    }
    result.settings.canonical_defaults = canonical(defaults_json);
  }

  const Object &permissions =
      as<Object>(required(root, "permissions"), "permissions");
  known_keys(permissions, {"required", "optional"}, "permissions");
  std::set<std::string, std::less<>> capabilities;
  for (const auto &[key, is_required] :
       std::array<std::pair<std::string_view, bool>, 2>{
           {{"required", true}, {"optional", false}}}) {
    const Array &requests = as<Array>(required(permissions, key), key);
    require(requests.size() <= 128, "too many capability requests");
    for (const auto &item : requests) {
      Object request = as<Object>(item, "capability request");
      const std::string capability =
          as<std::string>(required(request, "capability"), "capability");
      require(canonical_manifest_identifier(capability), "invalid capability id");
      require(capabilities.insert(capability).second,
              "duplicate capability request");
      const std::string reason =
          as<std::string>(required(request, "reason"), "reason");
      bounded_text(reason, 1024, "reason");
      std::uint32_t definition_generation = 0;
      std::string definition_digest;
      std::vector<std::string> operations;
      const auto generation_field = request.find("definitionGeneration");
      const auto digest_field = request.find("definitionDigest");
      const auto operations_field = request.find("operations");
        require(generation_field != request.end() &&
                    digest_field != request.end() &&
                    operations_field != request.end(),
                "dynamic capability reference is incomplete");
        const auto generation =
            as<std::int64_t>(generation_field->second, "definitionGeneration");
        require(generation > 0 && generation <= UINT32_MAX,
                "definitionGeneration is out of range");
        definition_generation = static_cast<std::uint32_t>(generation);
        definition_digest =
            as<std::string>(digest_field->second, "definitionDigest");
        require(canonical_sha256(definition_digest), "definitionDigest is invalid");
      {

        const auto &operation_values =
            as<Array>(operations_field->second, "operations");
        require(!operation_values.empty() && operation_values.size() <= 16,
                "operations count is invalid");
        for (const auto &operation : operation_values) {
          auto name = as<std::string>(operation, "operation");
          bounded_text(name, 128, "operation");
          require(std::find(operations.begin(), operations.end(), name) ==
                      operations.end(),
                  "duplicate operation");
          operations.push_back(std::move(name));
        }
        std::sort(operations.begin(), operations.end());
      }
      request.erase("capability");
      request.erase("reason");
      request.erase("definitionGeneration");
      request.erase("definitionDigest");
      request.erase("operations");
      result.requests.push_back({.capability = capability,
                                 .reason = reason,
                                 .canonical_scope = canonical(Json(request)),
                                 .definition_generation = definition_generation,
                                 .definition_digest = definition_digest,
                                 .operations = std::move(operations),
                                 .required = is_required});
    }
  }
  result.canonical_json = canonical(document);
  return result;
}

bool validate_settings_entry(
    const ManifestV2 &manifest,
    const std::map<std::string, SettingValue, std::less<>> &entry) noexcept {
  try {
    if (entry.size() != manifest.settings.schema.size())
      return false;
    for (const auto &definition : manifest.settings.schema) {
      const auto found = entry.find(definition.key);
      if (found == entry.end() || !valid_setting_value(definition,
                                                       found->second))
        return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<std::map<std::string, SettingValue, std::less<>>>
parse_settings_entry(const ManifestV2 &manifest,
                     std::string_view bytes) noexcept {
  try {
    const auto document = detail::parse_manifest_json(bytes);
    const auto &object = as<Object>(document, "settings entry");
    std::map<std::string, SettingValue, std::less<>> entry;
    for (const auto &[key, value] : object)
      entry.emplace(key, setting_value(value, "settings entry value"));
    if (!validate_settings_entry(manifest, entry))
      return std::nullopt;
    return entry;
  } catch (...) {
    return std::nullopt;
  }
}

std::string canonical_settings_entry(
    const std::map<std::string, SettingValue, std::less<>> &entry) {
  Object object;
  for (const auto &[key, value] : entry) {
    object.emplace(key, std::visit([](const auto &item) { return Json(item); },
                                   value));
  }
  return canonical(Json(std::move(object)));
}

std::vector<CapabilityRequest>
canonical_capability_requests(std::vector<CapabilityRequest> requests) {
  return canonical_requests(std::move(requests));
}

void TreeContents::add(TreeEntry entry) {
  require(safe_relative_path(entry.relative), "unsafe plugin tree path");
  const std::filesystem::path relative(entry.relative);
  require(*relative.begin() != ".git", ".git entry in plugin tree contents");
  require(entries_.size() < kMaximumFiles, "plugin tree has too many files");
  require(entry.bytes.size() <= kMaximumTreeBytes - total_bytes_,
          "plugin tree is too large");
  total_bytes_ += entry.bytes.size();
  entries_.push_back(std::move(entry));
}

std::uint64_t TreeContents::remaining_bytes() const noexcept {
  return kMaximumTreeBytes - total_bytes_;
}

const TreeEntry *TreeContents::find(std::string_view relative) const noexcept {
  const auto found =
      std::ranges::find(entries_, relative, &TreeEntry::relative);
  return found == entries_.end() ? nullptr : &*found;
}

ContentIdentity identify_tree_contents(TreeContents contents,
                                       const ManifestV2 &manifest) {
  auto &files = contents.entries_;
  std::ranges::sort(files, {}, &TreeEntry::relative);
  require(!files.empty(), "plugin tree is empty");
  require(std::adjacent_find(files.begin(), files.end(),
                             [](const TreeEntry &left, const TreeEntry &right) {
                               return left.relative == right.relative;
                             }) == files.end(),
          "duplicate plugin tree path");

  const auto *manifest_file = contents.find("manifest.json");
  require(manifest_file != nullptr, "plugin tree has no manifest.json");
  require(contents.find(manifest.runtime.qml) != nullptr,
          "runtime.qml does not exist");
  for (const auto &entry : manifest.runtime.surface_qml)
    require(contents.find(entry.qml) != nullptr,
            "runtime.surfaceQml entry does not exist");
  Sha256 tree;
  tree.update("OMARCHY-PLUGIN-TREE-V1\0"sv);
  hash_u64(tree, files.size());
  for (const auto &[relative, bytes, executable] : files) {
    hash_field(tree, relative);
    const std::byte mode{executable ? std::uint8_t{1} : std::uint8_t{0}};
    tree.update(std::span(&mode, 1));
    hash_field(tree, bytes);
  }
  require(parse_manifest_v2(manifest_file->bytes) == manifest,
          "manifest model does not match the hashed manifest.json");
  return {.tree_sha256 = tree.finish(),
          .manifest_sha256 = sha256_hex(manifest_file->bytes),
          .request_sha256 = fingerprint_requests(manifest.requests)};
}

std::string requested_capability_fingerprint(
    const std::vector<CapabilityRequest> &requests) {
  return fingerprint_requests(requests);
}

std::string sha256_hex(std::span<const std::byte> bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.finish();
}

std::string sha256_hex(std::string_view bytes) {
  return sha256_hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

} // namespace omarchy::plugins::manifest

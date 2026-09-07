#include "capability_definition_loader.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace omarchy::plugins::definitions;

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string json_string(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  for (const char byte : value) {
    if (byte == '"' || byte == '\\')
      output.push_back('\\');
    output.push_back(byte);
  }
  output.push_back('"');
  return output;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error(
          "usage: generate-builtin-catalog OUTPUT-DIRECTORY");
    const std::filesystem::path output_root(argv[1]);
    const auto definitions_root = output_root / "capabilities.d";
    std::filesystem::create_directories(definitions_root);

    const auto catalog = packaged_definitions();
    std::string index = "{\n  \"schemaVersion\": 1,\n  \"definitions\": [\n";
    for (std::size_t position = 0; position < catalog.size(); ++position) {
      const auto &definition = catalog[position];
      const auto document = canonical_definition_document(definition, 1);
      if (document.empty())
        throw std::runtime_error("definition document exceeded its contract");
      write_file(definitions_root / (std::string(definition.canonical_name.view()) + ".capability"),
                 document);
      index += "    {\"capability\":" + json_string(definition.canonical_name.view()) +
               ",\"definitionGeneration\":1,\"definitionDigest\":" +
               json_string(definition_digest(definition).view()) +
               ",\"contractDigest\":" +
               json_string(definition.adapter.contract_digest.view()) + "}";
      index += position + 1 == catalog.size() ? "\n" : ",\n";
    }
    index += "  ],\n  \"manifestReferencesRequireExactPins\": true\n}\n";
    write_file(output_root / "capability-catalog-v1.json", index);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "catalog generation failed: " << error.what() << '\n';
    return 1;
  }
}

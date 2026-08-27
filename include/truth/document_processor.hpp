#pragma once

#include "truth/types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace truth {

class DocumentProcessor {
 public:
  ParsedDocument extract(const std::filesystem::path& path,
                         const std::string& original_name,
                         const std::string& mime_type) const;

  std::vector<Chunk> chunk(const ParsedDocument& document,
                           std::size_t target_chars = 900,
                           std::size_t overlap_chars = 140) const;

  static std::string normalize_text(const std::string& input);
  static bool is_supported(const std::string& filename);
  static std::string support_summary();

 private:
  ParsedDocument extract_plain(const std::filesystem::path& path,
                               const std::string& extension) const;
  ParsedDocument extract_legacy_ppt(const std::filesystem::path& path) const;
  ParsedDocument extract_pdf(const std::filesystem::path& path) const;
  ParsedDocument extract_office_zip(const std::filesystem::path& path,
                                    const std::string& extension) const;
};

}  // namespace truth


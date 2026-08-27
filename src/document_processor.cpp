#include "truth/document_processor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>

#ifdef TRUTH_HAS_POPPLER
#include <poppler-document.h>
#include <poppler-page.h>
#endif

#ifdef TRUTH_HAS_LIBZIP
#include <zip.h>
#endif

namespace truth {
namespace {

std::string lower_extension(const std::string& filename) {
  std::string extension = std::filesystem::path(filename).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return extension;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("无法读取上传的文件");
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::string xml_decode(std::string text) {
  const std::pair<const char*, const char*> replacements[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
  for (const auto& [encoded, decoded] : replacements) {
    std::size_t position = 0;
    while ((position = text.find(encoded, position)) != std::string::npos) {
      text.replace(position, std::char_traits<char>::length(encoded), decoded);
      position += std::char_traits<char>::length(decoded);
    }
  }
  return text;
}

std::string xml_text_runs(const std::string& xml, const std::string& tag) {
  const std::regex expression("<" + tag + R"((?:\s[^>]*)?>([\s\S]*?)</)" + tag + ">",
                              std::regex::icase);
  std::string result;
  for (std::sregex_iterator match(xml.begin(), xml.end(), expression), end; match != end; ++match) {
    if (!result.empty()) result.push_back(' ');
    result += xml_decode((*match)[1].str());
  }
  return result;
}

bool is_utf8_continuation(unsigned char value) { return (value & 0xC0u) == 0x80u; }

std::size_t safe_utf8_boundary(const std::string& text, std::size_t position) {
  position = std::min(position, text.size());
  while (position > 0 && position < text.size() &&
         is_utf8_continuation(static_cast<unsigned char>(text[position]))) {
    --position;
  }
  return position;
}

std::string printable_strings(const std::string& bytes) {
  std::string result;
  std::string current;
  for (unsigned char value : bytes) {
    if ((value >= 32 && value <= 126) || value == '\n' || value == '\t') {
      current.push_back(static_cast<char>(value));
    } else {
      if (current.size() >= 5) {
        result += current;
        result.push_back('\n');
      }
      current.clear();
    }
  }
  if (current.size() >= 5) result += current;

  // Older PowerPoint files often contain UTF-16LE strings.
  current.clear();
  for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
    const unsigned char low = static_cast<unsigned char>(bytes[index]);
    const unsigned char high = static_cast<unsigned char>(bytes[index + 1]);
    if (high == 0 && low >= 32 && low <= 126) {
      current.push_back(static_cast<char>(low));
    } else {
      if (current.size() >= 5) {
        result += '\n';
        result += current;
      }
      current.clear();
    }
  }
  return result;
}

#ifdef TRUTH_HAS_LIBZIP
std::string zip_entry(zip_t* archive, const std::string& name) {
  zip_stat_t stat{};
  if (zip_stat(archive, name.c_str(), ZIP_FL_ENC_GUESS, &stat) != 0) return {};
  zip_file_t* file = zip_fopen(archive, name.c_str(), ZIP_FL_ENC_GUESS);
  if (!file) return {};
  std::string bytes(static_cast<std::size_t>(stat.size), '\0');
  const zip_int64_t read = zip_fread(file, bytes.data(), stat.size);
  zip_fclose(file);
  if (read < 0) return {};
  bytes.resize(static_cast<std::size_t>(read));
  return bytes;
}
#endif

}  // namespace

bool DocumentProcessor::is_supported(const std::string& filename) {
  const std::string extension = lower_extension(filename);
  return extension == ".pdf" || extension == ".pptx" || extension == ".ppt" ||
         extension == ".docx" || extension == ".txt" || extension == ".md" ||
         extension == ".csv";
}

std::string DocumentProcessor::support_summary() {
  std::string result = "TXT、Markdown、CSV、旧版 PPT";
#ifdef TRUTH_HAS_POPPLER
  result += "、PDF";
#else
  result += "；PDF 需要安装 poppler-cpp";
#endif
#ifdef TRUTH_HAS_LIBZIP
  result += "、PPTX、DOCX";
#else
  result += "；PPTX/DOCX 需要安装 libzip";
#endif
  return result;
}

ParsedDocument DocumentProcessor::extract(const std::filesystem::path& path,
                                          const std::string& original_name,
                                          const std::string&) const {
  const std::string extension = lower_extension(original_name);
  if (extension == ".txt" || extension == ".md" || extension == ".csv") {
    return extract_plain(path, extension.substr(1));
  }
  if (extension == ".pdf") return extract_pdf(path);
  if (extension == ".pptx" || extension == ".docx") {
    return extract_office_zip(path, extension);
  }
  if (extension == ".ppt") return extract_legacy_ppt(path);
  throw std::runtime_error("暂不支持该文件格式。支持：" + support_summary());
}

ParsedDocument DocumentProcessor::extract_plain(const std::filesystem::path& path,
                                                 const std::string& extension) const {
  std::string text = read_file(path);
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  text = normalize_text(text);
  if (text.empty()) throw std::runtime_error("文件中没有可读取的文字");
  return ParsedDocument{extension, 1, {{1, std::move(text)}}};
}

ParsedDocument DocumentProcessor::extract_legacy_ppt(const std::filesystem::path& path) const {
  std::string text = normalize_text(printable_strings(read_file(path)));
  if (text.size() < 20) {
    throw std::runtime_error("旧版 PPT 未提取到足够文字，建议另存为 PPTX 或 PDF 后重试");
  }
  return ParsedDocument{"ppt", 1, {{1, std::move(text)}}};
}

ParsedDocument DocumentProcessor::extract_pdf(const std::filesystem::path& path) const {
#ifdef TRUTH_HAS_POPPLER
  std::unique_ptr<poppler::document> document(poppler::document::load_from_file(path.string()));
  if (!document) throw std::runtime_error("PDF 无法打开或已加密");
  ParsedDocument result;
  result.source_type = "pdf";
  result.page_count = document->pages();
  for (int index = 0; index < document->pages(); ++index) {
    std::unique_ptr<poppler::page> page(document->create_page(index));
    if (!page) continue;
    std::string text = normalize_text(page->text().to_utf8());
    if (!text.empty()) result.pages.push_back({index + 1, std::move(text)});
  }
  if (result.pages.empty()) {
    throw std::runtime_error("PDF 中没有可提取文字；扫描版 PDF 需要先进行 OCR");
  }
  return result;
#else
  (void)path;
  throw std::runtime_error("当前构建未启用 PDF 解析，请安装 poppler-cpp 或使用 Docker 版本");
#endif
}

ParsedDocument DocumentProcessor::extract_office_zip(const std::filesystem::path& path,
                                                      const std::string& extension) const {
#ifdef TRUTH_HAS_LIBZIP
  int error = 0;
  zip_t* archive = zip_open(path.string().c_str(), ZIP_RDONLY, &error);
  if (!archive) throw std::runtime_error("Office 文件无法打开，文件可能已损坏");

  ParsedDocument result;
  result.source_type = extension == ".pptx" ? "pptx" : "docx";
  if (extension == ".docx") {
    std::string text = normalize_text(xml_text_runs(zip_entry(archive, "word/document.xml"), "w:t"));
    if (!text.empty()) result.pages.push_back({1, std::move(text)});
  } else {
    const std::regex slide_pattern(R"(^ppt/slides/slide([0-9]+)\.xml$)", std::regex::icase);
    std::map<int, std::string> slides;
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index) {
      const char* raw_name = zip_get_name(archive, index, 0);
      if (!raw_name) continue;
      std::smatch match;
      const std::string name(raw_name);
      if (std::regex_match(name, match, slide_pattern)) {
        std::string text = normalize_text(xml_text_runs(zip_entry(archive, name), "a:t"));
        if (!text.empty()) slides[std::stoi(match[1].str())] = std::move(text);
      }
    }
    for (auto& [page, text] : slides) result.pages.push_back({page, std::move(text)});
    result.page_count = slides.empty() ? 0 : slides.rbegin()->first;
  }
  zip_close(archive);
  if (result.page_count == 0) result.page_count = static_cast<int>(result.pages.size());
  if (result.pages.empty()) throw std::runtime_error("Office 文件中没有可读取的文字");
  return result;
#else
  (void)path;
  (void)extension;
  throw std::runtime_error("当前构建未启用 PPTX/DOCX 解析，请安装 libzip 或使用 Docker 版本");
#endif
}

std::string DocumentProcessor::normalize_text(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  bool previous_space = false;
  int consecutive_newlines = 0;

  for (std::size_t index = 0; index < input.size(); ++index) {
    unsigned char value = static_cast<unsigned char>(input[index]);
    if (value == '\r') continue;
    if (value == '\n') {
      while (!output.empty() && output.back() == ' ') output.pop_back();
      if (consecutive_newlines < 2 && !output.empty()) output.push_back('\n');
      ++consecutive_newlines;
      previous_space = false;
      continue;
    }
    consecutive_newlines = 0;
    if (value == '\t' || value == ' ' || value == '\f' || value == '\v') {
      if (!previous_space && !output.empty() && output.back() != '\n') output.push_back(' ');
      previous_space = true;
      continue;
    }
    if (value < 32) continue;
    output.push_back(static_cast<char>(value));
    previous_space = false;
  }
  while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) output.pop_back();
  return output;
}

std::vector<Chunk> DocumentProcessor::chunk(const ParsedDocument& document,
                                            std::size_t target_chars,
                                            std::size_t overlap_chars) const {
  if (target_chars < 160) target_chars = 160;
  overlap_chars = std::min(overlap_chars, target_chars / 3);
  std::vector<Chunk> chunks;
  int chunk_index = 0;

  for (const auto& page : document.pages) {
    const std::string text = normalize_text(page.text);
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t end = safe_utf8_boundary(text, std::min(start + target_chars, text.size()));
      if (end < text.size()) {
        const std::size_t minimum = start + target_chars * 3 / 5;
        const std::size_t newline = text.rfind('\n', end);
        const std::size_t period = text.rfind('.', end);
        const std::size_t question = text.rfind('?', end);
        const std::size_t space = text.rfind(' ', end);
        std::size_t boundary = 0;
        bool found_boundary = false;
        for (const std::size_t candidate : {newline, period, question, space}) {
          if (candidate != std::string::npos && (!found_boundary || candidate > boundary)) {
            boundary = candidate;
            found_boundary = true;
          }
        }
        if (found_boundary && boundary >= minimum) end = boundary + 1;
      }
      if (end <= start) end = safe_utf8_boundary(text, std::min(start + target_chars, text.size()));
      std::string content = normalize_text(text.substr(start, end - start));
      if (!content.empty()) {
        Chunk chunk;
        chunk.content = std::move(content);
        chunk.page = page.page;
        chunk.chunk_index = chunk_index++;
        chunk.token_estimate = static_cast<int>(chunk.content.size() / 3 + 1);
        chunks.push_back(std::move(chunk));
      }
      if (end >= text.size()) break;
      const std::size_t next = safe_utf8_boundary(text, end > overlap_chars ? end - overlap_chars : end);
      start = next > start ? next : end;
    }
  }
  if (chunks.empty()) throw std::runtime_error("资料未能生成有效的知识片段");
  return chunks;
}

}  // namespace truth

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace truth {

struct PageText {
  int page{1};
  std::string text;
};

struct ParsedDocument {
  std::string source_type;
  int page_count{1};
  std::vector<PageText> pages;
};

struct Document {
  std::int64_t id{0};
  std::string name;
  std::string mime_type;
  std::string object_path;
  std::string source_type;
  std::string status{"ready"};
  int page_count{0};
  int chunk_count{0};
  std::string created_at;
};

struct Chunk {
  std::int64_t id{0};
  std::int64_t document_id{0};
  std::string document_name;
  std::string content;
  int page{1};
  int chunk_index{0};
  int token_estimate{0};
  std::vector<float> embedding;
  std::string embedding_provider{"local"};
  double score{0.0};
};

struct SourceReference {
  std::string document_name;
  int page{1};
  std::string excerpt;
  double score{0.0};
};

struct ChatResult {
  std::string answer;
  std::vector<SourceReference> sources;
  bool used_ai{false};
};

struct QuizQuestion {
  std::string question;
  std::vector<std::string> options;
  int answer_index{0};
  std::string explanation;
  SourceReference source;
};

struct QuizResult {
  std::string title;
  std::vector<QuizQuestion> questions;
  bool used_ai{false};
};

struct DailyStudyTime {
  std::string date;
  std::int64_t seconds{0};
};

struct StudyTimeSummary {
  std::int64_t today_seconds{0};
  std::int64_t total_seconds{0};
  int active_days{0};
  std::vector<DailyStudyTime> recent;
};

struct AppStats {
  int document_count{0};
  int chunk_count{0};
  int quiz_attempt_count{0};
  double quiz_average{0.0};
};

}  // namespace truth


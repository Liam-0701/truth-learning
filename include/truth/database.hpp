#pragma once

#include "truth/types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <sqlite3.h>

namespace truth {

class Database {
 public:
  explicit Database(const std::filesystem::path& path);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void initialize();
  std::int64_t insert_document(Document document, const std::vector<Chunk>& chunks);
  std::vector<Document> list_documents();
  std::optional<std::string> delete_document(std::int64_t id);
  std::vector<Chunk> load_chunks(const std::vector<std::int64_t>& document_ids = {});

  void save_quiz_attempt(int score, int total);
  AppStats stats();

  void add_study_seconds(const std::string& date, std::int64_t seconds);
  StudyTimeSummary study_time(const std::string& today, int recent_days = 7);

 private:
  void exec(const char* sql);

  sqlite3* db_{nullptr};
  std::mutex mutex_;
};

}  // namespace truth


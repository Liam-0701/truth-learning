#pragma once

#include "truth/database.hpp"
#include "truth/document_processor.hpp"
#include "truth/embeddings.hpp"
#include "truth/openai_client.hpp"
#include "truth/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace truth {

class StudyService {
 public:
  StudyService(std::filesystem::path database_path,
               std::filesystem::path upload_directory,
               std::string api_key,
               std::string chat_model = "gpt-5-mini");

  void initialize();
  Document ingest(const std::string& filename,
                  const std::string& mime_type,
                  const std::string& bytes);
  std::vector<Document> documents();
  bool remove_document(std::int64_t id);

  ChatResult chat(const std::string& question,
                  const std::vector<std::int64_t>& document_ids = {});
  QuizResult quiz(const std::string& topic,
                  int count,
                  const std::vector<std::int64_t>& document_ids = {});
  void save_quiz_attempt(int score, int total);

  AppStats stats();
  void add_study_seconds(const std::string& date, std::int64_t seconds);
  StudyTimeSummary study_time(const std::string& date);

 private:
  std::vector<Chunk> retrieve(const std::string& query,
                              const std::vector<std::int64_t>& document_ids,
                              std::size_t limit = 6);
  ChatResult fallback_chat(const std::string& question, const std::vector<Chunk>& chunks) const;
  QuizResult fallback_quiz(const std::string& topic,
                           int count,
                           const std::vector<Chunk>& chunks) const;
  std::string context_block(const std::vector<Chunk>& chunks) const;

  Database database_;
  std::filesystem::path upload_directory_;
  DocumentProcessor processor_;
  OpenAIClient openai_;
  EmbeddingEngine embeddings_;
  std::string chat_model_;
};

}  // namespace truth


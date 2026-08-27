#include "truth/database.hpp"
#include "truth/document_processor.hpp"
#include "truth/embeddings.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

int main() {
  using truth::DocumentProcessor;
  using truth::EmbeddingEngine;

  const auto vector_a = EmbeddingEngine::local_embedding("遗忘曲线与间隔重复学习");
  const auto vector_b = EmbeddingEngine::local_embedding("通过间隔重复对抗遗忘曲线");
  const auto vector_c = EmbeddingEngine::local_embedding("量子力学的波函数");
  assert(vector_a.size() == 256);
  assert(EmbeddingEngine::cosine_similarity(vector_a, vector_a) > 0.99);
  assert(EmbeddingEngine::lexical_similarity("间隔重复", "使用间隔重复巩固记忆") > 0.0);
  assert(EmbeddingEngine::cosine_similarity(vector_a, vector_b) >
         EmbeddingEngine::cosine_similarity(vector_a, vector_c));

  truth::ParsedDocument parsed{"txt", 1, {{1, std::string(2400, 'a')}}};
  const auto chunks = DocumentProcessor{}.chunk(parsed, 500, 80);
  assert(chunks.size() >= 5);
  assert(chunks.front().page == 1);
  assert(chunks.front().chunk_index == 0);

  const auto temporary = std::filesystem::temp_directory_path() / "truth-learning-core-test.db";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
  {
    truth::Database database(temporary);
    database.initialize();
    database.add_study_seconds("2026-08-27", 1800);
    database.add_study_seconds("2026-08-27", 900);
    const auto summary = database.study_time("2026-08-27");
    assert(summary.today_seconds == 2700);
    assert(summary.total_seconds == 2700);
    assert(summary.active_days == 1);
  }
  std::filesystem::remove(temporary, ignored);
  std::filesystem::remove(temporary.string() + "-wal", ignored);
  std::filesystem::remove(temporary.string() + "-shm", ignored);

  std::cout << "Truth Learning core tests passed\n";
  return 0;
}

#pragma once

#include "truth/openai_client.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace truth {

class EmbeddingEngine {
 public:
  explicit EmbeddingEngine(const OpenAIClient& client,
                           std::string model = "text-embedding-3-small",
                           std::size_t dimensions = 512);

  std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts,
                                                   std::string& provider) const;
  std::vector<float> embed_query(const std::string& text, const std::string& provider) const;

  static std::vector<float> local_embedding(const std::string& text,
                                            std::size_t dimensions = 256);
  static double cosine_similarity(const std::vector<float>& left,
                                  const std::vector<float>& right);
  static double lexical_similarity(const std::string& query, const std::string& content);

 private:
  std::vector<std::vector<float>> openai_embeddings(const std::vector<std::string>& texts) const;

  const OpenAIClient& client_;
  std::string model_;
  std::size_t dimensions_;
};

}  // namespace truth


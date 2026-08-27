#include "truth/embeddings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>

namespace truth {
namespace {

std::uint64_t fnv1a(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::vector<std::string> features(const std::string& text) {
  std::vector<std::string> result;
  std::string ascii;
  std::vector<std::string> non_ascii;

  auto flush_ascii = [&] {
    if (ascii.size() >= 2) result.push_back(ascii);
    ascii.clear();
  };

  for (std::size_t index = 0; index < text.size();) {
    const unsigned char first = static_cast<unsigned char>(text[index]);
    if (first < 128) {
      if (std::isalnum(first)) {
        ascii.push_back(static_cast<char>(std::tolower(first)));
      } else {
        flush_ascii();
      }
      ++index;
      continue;
    }
    flush_ascii();
    std::size_t length = 1;
    if ((first & 0xE0u) == 0xC0u) length = 2;
    else if ((first & 0xF0u) == 0xE0u) length = 3;
    else if ((first & 0xF8u) == 0xF0u) length = 4;
    length = std::min(length, text.size() - index);
    non_ascii.push_back(text.substr(index, length));
    index += length;
  }
  flush_ascii();

  for (std::size_t index = 0; index < non_ascii.size(); ++index) {
    result.push_back(non_ascii[index]);
    if (index + 1 < non_ascii.size()) result.push_back(non_ascii[index] + non_ascii[index + 1]);
  }
  if (result.empty() && !text.empty()) result.push_back(text);
  return result;
}

void normalize(std::vector<float>& vector) {
  double length = 0.0;
  for (float value : vector) length += static_cast<double>(value) * value;
  if (length <= 0.0) return;
  const double inverse = 1.0 / std::sqrt(length);
  for (float& value : vector) value = static_cast<float>(value * inverse);
}

}  // namespace

EmbeddingEngine::EmbeddingEngine(const OpenAIClient& client, std::string model, std::size_t dimensions)
    : client_(client), model_(std::move(model)), dimensions_(dimensions) {}

std::vector<std::vector<float>> EmbeddingEngine::embed_documents(
    const std::vector<std::string>& texts, std::string& provider) const {
  if (client_.enabled()) {
    try {
      auto result = openai_embeddings(texts);
      provider = "openai:" + model_;
      return result;
    } catch (...) {
      // Local embeddings keep the application usable during temporary API failures.
    }
  }
  provider = "local:hash-v1";
  std::vector<std::vector<float>> result;
  result.reserve(texts.size());
  for (const auto& text : texts) result.push_back(local_embedding(text));
  return result;
}

std::vector<float> EmbeddingEngine::embed_query(const std::string& text,
                                                const std::string& provider) const {
  if (provider.starts_with("openai:") && client_.enabled()) {
    try {
      auto result = openai_embeddings({text});
      if (!result.empty()) return std::move(result.front());
    } catch (...) {
    }
  }
  return local_embedding(text);
}

std::vector<std::vector<float>> EmbeddingEngine::openai_embeddings(
    const std::vector<std::string>& texts) const {
  if (texts.empty()) return {};
  nlohmann::json request = {
      {"model", model_}, {"input", texts}, {"dimensions", dimensions_}, {"encoding_format", "float"}};
  const auto response = client_.post("/v1/embeddings", request);
  if (!response.contains("data") || !response["data"].is_array()) {
    throw std::runtime_error("Embeddings 响应缺少 data");
  }
  std::vector<std::vector<float>> result;
  result.reserve(response["data"].size());
  for (const auto& item : response["data"]) {
    result.push_back(item.at("embedding").get<std::vector<float>>());
  }
  if (result.size() != texts.size()) throw std::runtime_error("Embeddings 数量不匹配");
  return result;
}

std::vector<float> EmbeddingEngine::local_embedding(const std::string& text,
                                                    std::size_t dimensions) {
  dimensions = std::max<std::size_t>(32, dimensions);
  std::vector<float> vector(dimensions, 0.0f);
  for (const auto& feature : features(text)) {
    const std::uint64_t hash = fnv1a(feature);
    const std::size_t index = static_cast<std::size_t>(hash % dimensions);
    const float sign = ((hash >> 17u) & 1u) ? 1.0f : -1.0f;
    vector[index] += sign;
  }
  normalize(vector);
  return vector;
}

double EmbeddingEngine::cosine_similarity(const std::vector<float>& left,
                                          const std::vector<float>& right) {
  if (left.empty() || left.size() != right.size()) return 0.0;
  double dot = 0.0;
  double left_length = 0.0;
  double right_length = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    dot += static_cast<double>(left[index]) * right[index];
    left_length += static_cast<double>(left[index]) * left[index];
    right_length += static_cast<double>(right[index]) * right[index];
  }
  if (left_length <= 0.0 || right_length <= 0.0) return 0.0;
  return dot / (std::sqrt(left_length) * std::sqrt(right_length));
}

double EmbeddingEngine::lexical_similarity(const std::string& query, const std::string& content) {
  const auto query_features = features(query);
  const auto content_features = features(content);
  if (query_features.empty() || content_features.empty()) return 0.0;
  const std::set<std::string> query_set(query_features.begin(), query_features.end());
  const std::set<std::string> content_set(content_features.begin(), content_features.end());
  std::size_t matches = 0;
  for (const auto& feature : query_set) {
    if (content_set.contains(feature)) ++matches;
  }
  return static_cast<double>(matches) / std::sqrt(
      static_cast<double>(query_set.size()) * static_cast<double>(content_set.size()));
}

}  // namespace truth

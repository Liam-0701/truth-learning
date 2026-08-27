#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace truth {

class OpenAIClient {
 public:
  explicit OpenAIClient(std::string api_key);

  bool enabled() const;
  nlohmann::json post(const std::string& endpoint, const nlohmann::json& body) const;
  std::string response_text(const nlohmann::json& response) const;

 private:
  std::string api_key_;
};

}  // namespace truth


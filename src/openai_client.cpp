#include "truth/openai_client.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

namespace truth {
namespace {

std::once_flag curl_once;

std::size_t write_response(char* data, std::size_t size, std::size_t count, void* userdata) {
  const std::size_t bytes = size * count;
  static_cast<std::string*>(userdata)->append(data, bytes);
  return bytes;
}

std::string base_url() {
  const char* configured = std::getenv("OPENAI_BASE_URL");
  std::string value = configured && *configured ? configured : "https://api.openai.com";
  while (!value.empty() && value.back() == '/') value.pop_back();
  return value;
}

}  // namespace

OpenAIClient::OpenAIClient(std::string api_key) : api_key_(std::move(api_key)) {
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool OpenAIClient::enabled() const { return !api_key_.empty(); }

nlohmann::json OpenAIClient::post(const std::string& endpoint, const nlohmann::json& body) const {
  if (!enabled()) throw std::runtime_error("未配置 OPENAI_API_KEY");

  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("无法初始化 HTTP 客户端");

  const std::string url = base_url() + endpoint;
  const std::string payload = body.dump();
  std::string response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + api_key_;
  headers = curl_slist_append(headers, authorization.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Truth-Learning/1.0");

  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    throw std::runtime_error(std::string("OpenAI 请求失败：") + curl_easy_strerror(result));
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(response);
  } catch (...) {
    throw std::runtime_error("OpenAI 返回了无法解析的响应");
  }
  if (status < 200 || status >= 300) {
    std::string message = parsed.value("error", nlohmann::json::object()).value("message", "未知错误");
    throw std::runtime_error("OpenAI API 错误（" + std::to_string(status) + "）：" + message);
  }
  return parsed;
}

std::string OpenAIClient::response_text(const nlohmann::json& response) const {
  if (response.contains("output_text") && response["output_text"].is_string()) {
    return response["output_text"].get<std::string>();
  }
  std::string result;
  if (!response.contains("output") || !response["output"].is_array()) return result;
  for (const auto& output : response["output"]) {
    if (!output.contains("content") || !output["content"].is_array()) continue;
    for (const auto& content : output["content"]) {
      if (content.contains("text") && content["text"].is_string()) {
        if (!result.empty()) result.push_back('\n');
        result += content["text"].get<std::string>();
      }
    }
  }
  return result;
}

}  // namespace truth


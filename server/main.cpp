#include "truth/document_processor.hpp"
#include "truth/service.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using nlohmann::json;

std::string environment(const char* name, const std::string& fallback = {}) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}

std::string local_date() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::ostringstream output;
  output << std::put_time(&local, "%Y-%m-%d");
  return output.str();
}

json document_json(const truth::Document& document) {
  return {{"id", document.id}, {"name", document.name}, {"mime_type", document.mime_type},
          {"source_type", document.source_type}, {"status", document.status},
          {"page_count", document.page_count}, {"chunk_count", document.chunk_count},
          {"created_at", document.created_at}};
}

json source_json(const truth::SourceReference& source) {
  return {{"document_name", source.document_name}, {"page", source.page},
          {"excerpt", source.excerpt}, {"score", source.score}};
}

json quiz_json(const truth::QuizResult& quiz) {
  json questions = json::array();
  for (const auto& question : quiz.questions) {
    questions.push_back({{"question", question.question}, {"options", question.options},
                         {"answer_index", question.answer_index},
                         {"explanation", question.explanation},
                         {"source", source_json(question.source)}});
  }
  return {{"title", quiz.title}, {"questions", questions}, {"used_ai", quiz.used_ai}};
}

void send_json(httplib::Response& response, const json& value, int status = 200) {
  response.status = status;
  response.set_content(value.dump(), "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
}

json parse_body(const httplib::Request& request) {
  if (request.body.empty()) return json::object();
  try {
    return json::parse(request.body);
  } catch (...) {
    throw std::runtime_error("请求 JSON 格式不正确");
  }
}

template <typename Handler>
auto safe(Handler handler) {
  return [handler = std::move(handler)](const httplib::Request& request,
                                        httplib::Response& response) {
    try {
      handler(request, response);
    } catch (const std::exception& error) {
      send_json(response, {{"error", error.what()}}, 400);
    } catch (...) {
      send_json(response, {{"error", "服务器发生未知错误"}}, 500);
    }
  };
}

}  // namespace

int main() {
  try {
    const std::filesystem::path database_path = environment("TRUTH_DB_PATH", "./data/truth-learning.db");
    const std::filesystem::path upload_directory = environment("TRUTH_UPLOAD_DIR", "./uploads");
    const std::filesystem::path public_directory = environment("TRUTH_PUBLIC_DIR", "./public");
    const std::string api_key = environment("OPENAI_API_KEY");
    const std::string chat_model = environment("OPENAI_CHAT_MODEL", "gpt-5-mini");
    const int port = std::stoi(environment("PORT", "8080"));

    truth::StudyService service(database_path, upload_directory, api_key, chat_model);
    service.initialize();

    httplib::Server server;
    server.set_payload_max_length(20ULL * 1024ULL * 1024ULL);
    server.set_read_timeout(120, 0);
    server.set_write_timeout(120, 0);

    server.Get("/api/health", safe([&](const httplib::Request&, httplib::Response& response) {
      send_json(response, {{"status", "ok"}, {"name", "Truth Learning"},
                           {"document_support", truth::DocumentProcessor::support_summary()},
                           {"openai_enabled", !api_key.empty()}});
    }));

    server.Get("/api/documents", safe([&](const httplib::Request&, httplib::Response& response) {
      json items = json::array();
      for (const auto& document : service.documents()) items.push_back(document_json(document));
      send_json(response, {{"documents", items}});
    }));

    server.Post("/api/documents", safe([&](const httplib::Request& request,
                                            httplib::Response& response) {
      if (!request.form.has_file("file")) throw std::runtime_error("请选择要上传的文件");
      const auto file = request.form.get_file("file");
      if (file.content.size() > 15ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("单个文件不能超过 15 MB");
      }
      const auto document = service.ingest(file.filename, file.content_type, file.content);
      send_json(response, {{"document", document_json(document)}}, 201);
    }));

    server.Delete("/api/documents", safe([&](const httplib::Request& request,
                                              httplib::Response& response) {
      if (!request.has_param("id")) throw std::runtime_error("缺少资料 id");
      const auto id = std::stoll(request.get_param_value("id"));
      if (!service.remove_document(id)) {
        send_json(response, {{"error", "资料不存在"}}, 404);
        return;
      }
      send_json(response, {{"ok", true}});
    }));

    server.Post("/api/chat", safe([&](const httplib::Request& request, httplib::Response& response) {
      const json body = parse_body(request);
      const auto document_ids = body.value("document_ids", std::vector<std::int64_t>{});
      const auto result = service.chat(body.value("question", ""), document_ids);
      json sources = json::array();
      for (const auto& source : result.sources) sources.push_back(source_json(source));
      send_json(response, {{"answer", result.answer}, {"sources", sources},
                           {"used_ai", result.used_ai}});
    }));

    server.Post("/api/quiz", safe([&](const httplib::Request& request, httplib::Response& response) {
      const json body = parse_body(request);
      const auto document_ids = body.value("document_ids", std::vector<std::int64_t>{});
      send_json(response, quiz_json(service.quiz(body.value("topic", ""), body.value("count", 3),
                                                 document_ids)));
    }));

    server.Post("/api/quiz/attempt", safe([&](const httplib::Request& request,
                                              httplib::Response& response) {
      const json body = parse_body(request);
      service.save_quiz_attempt(body.value("score", -1), body.value("total", 0));
      send_json(response, {{"ok", true}});
    }));

    server.Get("/api/stats", safe([&](const httplib::Request&, httplib::Response& response) {
      const auto stats = service.stats();
      send_json(response, {{"document_count", stats.document_count}, {"chunk_count", stats.chunk_count},
                           {"quiz_attempt_count", stats.quiz_attempt_count},
                           {"quiz_average", stats.quiz_average}});
    }));

    server.Get("/api/study-time", safe([&](const httplib::Request& request,
                                            httplib::Response& response) {
      const std::string date = request.has_param("date") ? request.get_param_value("date") : local_date();
      const auto summary = service.study_time(date);
      json recent = json::array();
      for (const auto& day : summary.recent) recent.push_back({{"date", day.date}, {"seconds", day.seconds}});
      send_json(response, {{"today_seconds", summary.today_seconds},
                           {"total_seconds", summary.total_seconds}, {"active_days", summary.active_days},
                           {"recent", recent}});
    }));

    server.Post("/api/study-time", safe([&](const httplib::Request& request,
                                             httplib::Response& response) {
      const json body = parse_body(request);
      service.add_study_seconds(body.value("date", local_date()), body.value("seconds", 0));
      send_json(response, {{"ok", true}});
    }));

    if (!std::filesystem::exists(public_directory / "index.html")) {
      throw std::runtime_error("找不到前端目录：" + public_directory.string());
    }
    if (!server.set_mount_point("/", public_directory.string())) {
      throw std::runtime_error("无法挂载前端目录：" + public_directory.string());
    }

    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
      if (response.status == 404) send_json(response, {{"error", "页面或接口不存在"}}, 404);
    });

    std::cout << "Truth Learning is running at http://127.0.0.1:" << port << '\n';
    std::cout << "Documents: " << truth::DocumentProcessor::support_summary() << '\n';
    std::cout << "OpenAI: " << (api_key.empty() ? "local fallback" : "enabled") << '\n';
    if (!server.listen("0.0.0.0", port)) {
      std::cerr << "Unable to listen on port " << port << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Startup error: " << error.what() << '\n';
    return 1;
  }
}

#include "truth/service.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace truth {
namespace {

std::string safe_filename(std::string filename) {
  filename = std::filesystem::path(filename).filename().string();
  filename = std::regex_replace(filename, std::regex(R"([^A-Za-z0-9._\-])"), "_");
  if (filename.empty()) filename = "document.txt";
  return filename;
}

std::string unique_prefix() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::mt19937_64 generator(static_cast<std::uint64_t>(now));
  std::ostringstream output;
  output << std::hex << now << '-' << generator();
  return output.str();
}

std::string excerpt(const std::string& text, std::size_t maximum = 210) {
  if (text.size() <= maximum) return text;
  std::size_t end = maximum;
  while (end > 0 && end < text.size() &&
         (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
    --end;
  }
  return text.substr(0, end) + "…";
}

SourceReference source_from(const Chunk& chunk) {
  return {chunk.document_name, chunk.page, excerpt(chunk.content), chunk.score};
}

std::string strip_json_fence(std::string value) {
  const auto first = value.find('{');
  const auto last = value.rfind('}');
  if (first == std::string::npos || last == std::string::npos || last < first) return value;
  return value.substr(first, last - first + 1);
}

}  // namespace

StudyService::StudyService(std::filesystem::path database_path,
                           std::filesystem::path upload_directory,
                           std::string api_key,
                           std::string chat_model)
    : database_(database_path),
      upload_directory_(std::move(upload_directory)),
      openai_(std::move(api_key)),
      embeddings_(openai_),
      chat_model_(std::move(chat_model)) {}

void StudyService::initialize() {
  std::filesystem::create_directories(upload_directory_);
  database_.initialize();
}

Document StudyService::ingest(const std::string& filename,
                              const std::string& mime_type,
                              const std::string& bytes) {
  if (filename.empty()) throw std::runtime_error("文件名不能为空");
  if (!DocumentProcessor::is_supported(filename)) {
    throw std::runtime_error("不支持该文件格式。支持：" + DocumentProcessor::support_summary());
  }
  if (bytes.empty()) throw std::runtime_error("上传的文件为空");

  const std::filesystem::path stored_path = upload_directory_ / (unique_prefix() + '-' + safe_filename(filename));
  {
    std::ofstream output(stored_path, std::ios::binary);
    if (!output) throw std::runtime_error("无法保存上传文件");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("保存上传文件失败");
  }

  try {
    ParsedDocument parsed = processor_.extract(stored_path, filename, mime_type);
    std::vector<Chunk> chunks = processor_.chunk(parsed);
    std::vector<std::string> texts;
    texts.reserve(chunks.size());
    for (const auto& chunk : chunks) texts.push_back(chunk.content);

    std::string provider;
    auto vectors = embeddings_.embed_documents(texts, provider);
    if (vectors.size() != chunks.size()) throw std::runtime_error("向量生成数量不匹配");
    for (std::size_t index = 0; index < chunks.size(); ++index) {
      chunks[index].embedding = std::move(vectors[index]);
      chunks[index].embedding_provider = provider;
    }

    Document document;
    document.name = filename;
    document.mime_type = mime_type.empty() ? "application/octet-stream" : mime_type;
    document.object_path = stored_path.string();
    document.source_type = parsed.source_type;
    document.page_count = parsed.page_count;
    document.chunk_count = static_cast<int>(chunks.size());
    document.id = database_.insert_document(document, chunks);

    for (const auto& saved : database_.list_documents()) {
      if (saved.id == document.id) return saved;
    }
    return document;
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(stored_path, ignored);
    throw;
  }
}

std::vector<Document> StudyService::documents() { return database_.list_documents(); }

bool StudyService::remove_document(std::int64_t id) {
  const auto object_path = database_.delete_document(id);
  if (!object_path) return false;
  std::error_code ignored;
  std::filesystem::remove(*object_path, ignored);
  return true;
}

std::vector<Chunk> StudyService::retrieve(const std::string& query,
                                          const std::vector<std::int64_t>& document_ids,
                                          std::size_t limit) {
  auto chunks = database_.load_chunks(document_ids);
  if (chunks.empty()) return {};
  std::map<std::string, std::vector<float>> query_vectors;
  for (auto& chunk : chunks) {
    if (!query_vectors.contains(chunk.embedding_provider)) {
      query_vectors[chunk.embedding_provider] = embeddings_.embed_query(query, chunk.embedding_provider);
    }
    const double semantic = EmbeddingEngine::cosine_similarity(
        query_vectors[chunk.embedding_provider], chunk.embedding);
    const double lexical = EmbeddingEngine::lexical_similarity(query, chunk.content);
    chunk.score = semantic * 0.74 + lexical * 0.26;
  }
  std::stable_sort(chunks.begin(), chunks.end(), [](const Chunk& left, const Chunk& right) {
    return left.score > right.score;
  });
  if (chunks.size() > limit) chunks.resize(limit);
  return chunks;
}

std::string StudyService::context_block(const std::vector<Chunk>& chunks) const {
  std::ostringstream context;
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    context << "[资料" << index + 1 << "｜" << chunks[index].document_name << "｜第"
            << chunks[index].page << "页]\n" << chunks[index].content << "\n\n";
  }
  return context.str();
}

ChatResult StudyService::fallback_chat(const std::string& question,
                                       const std::vector<Chunk>& chunks) const {
  ChatResult result;
  if (chunks.empty()) {
    result.answer = "我还没有可检索的学习资料。请先上传课件或笔记，再向我提问。";
    return result;
  }
  std::ostringstream answer;
  answer << "根据你上传的资料，与“" << question << "”最相关的内容可以这样理解：\n\n";
  for (std::size_t index = 0; index < std::min<std::size_t>(3, chunks.size()); ++index) {
    answer << index + 1 << ". " << excerpt(chunks[index].content, 300) << "（《"
           << chunks[index].document_name << "》第" << chunks[index].page << "页）\n";
    result.sources.push_back(source_from(chunks[index]));
  }
  answer << "\n学习建议：先用自己的话复述以上要点，再回到原页核对。"
            "当前使用本地检索式回答；配置 OPENAI_API_KEY 后会获得更完整的讲解。";
  result.answer = answer.str();
  return result;
}

ChatResult StudyService::chat(const std::string& question,
                              const std::vector<std::int64_t>& document_ids) {
  if (question.empty()) throw std::runtime_error("问题不能为空");
  auto chunks = retrieve(question, document_ids);
  if (!openai_.enabled() || chunks.empty()) return fallback_chat(question, chunks);

  try {
    const std::string instructions =
        "你是 Truth Learning 的安静、严谨的中文学习导师。只能根据给出的资料回答。"
        "若资料不足，明确说不知道，不要编造。先给直接答案，再分点教学，最后给一个主动回忆问题。"
        "引用时使用 [资料1] 这样的标记。";
    const std::string input = "学生问题：" + question + "\n\n可用资料：\n" + context_block(chunks);
    const nlohmann::json response = openai_.post(
        "/v1/responses",
        {{"model", chat_model_}, {"instructions", instructions}, {"input", input},
         {"max_output_tokens", 1200}});
    ChatResult result;
    result.answer = openai_.response_text(response);
    if (result.answer.empty()) return fallback_chat(question, chunks);
    result.used_ai = true;
    for (const auto& chunk : chunks) result.sources.push_back(source_from(chunk));
    return result;
  } catch (...) {
    return fallback_chat(question, chunks);
  }
}

QuizResult StudyService::fallback_quiz(const std::string& topic,
                                       int count,
                                       const std::vector<Chunk>& chunks) const {
  QuizResult result;
  result.title = topic.empty() ? "资料回忆练习" : topic + " · 回忆练习";
  if (chunks.empty()) return result;
  count = std::clamp(count, 1, 5);
  for (int index = 0; index < count; ++index) {
    const auto& chunk = chunks[static_cast<std::size_t>(index) % chunks.size()];
    QuizQuestion question;
    const std::string stems[] = {
        "下列哪项最接近资料原意？", "下列哪项可以由资料直接支持？", "复习这一部分时，哪项内容最值得主动回忆？"};
    question.question = "根据《" + chunk.document_name + "》第" + std::to_string(chunk.page) +
                        "页，" + stems[index % 3];
    question.options = {excerpt(chunk.content, 190), "资料认为这个概念在任何条件下都不成立。",
                        "资料没有讨论任何相关概念。", "资料要求忽略前提，只记住结论。"};
    const int shift = index % 4;
    std::rotate(question.options.begin(), question.options.begin() + shift, question.options.end());
    question.answer_index = (4 - shift) % 4;
    question.explanation = "正确选项直接来自上传资料中的相关片段；复习时请同时注意它的前提和语境。";
    question.source = source_from(chunk);
    result.questions.push_back(std::move(question));
  }
  return result;
}

QuizResult StudyService::quiz(const std::string& topic,
                              int count,
                              const std::vector<std::int64_t>& document_ids) {
  count = std::clamp(count, 1, 5);
  const std::string query = topic.empty() ? "核心概念 重点 定义 原理" : topic;
  auto chunks = retrieve(query, document_ids, std::max<std::size_t>(6, count));
  if (!openai_.enabled() || chunks.empty()) return fallback_quiz(topic, count, chunks);

  try {
    nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"title", {{"type", "string"}}},
          {"questions",
           {{"type", "array"},
            {"minItems", count},
            {"maxItems", count},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"question", {{"type", "string"}}},
                {"options",
                 {{"type", "array"}, {"minItems", 4}, {"maxItems", 4},
                  {"items", {{"type", "string"}}}}},
                {"answer_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
                {"explanation", {{"type", "string"}}},
                {"source_index", {{"type", "integer"}, {"minimum", 1},
                                  {"maximum", static_cast<int>(chunks.size())}}}}},
              {"required", {"question", "options", "answer_index", "explanation", "source_index"}}}}}}}},
        {"required", {"title", "questions"}}};

    const std::string instructions =
        "你是中文教学设计师。严格依据资料生成四选一题，考理解而非字面陷阱。"
        "每题只有一个正确答案，解释必须说明原因。source_index 对应给定资料编号。";
    const std::string input = "主题：" + (topic.empty() ? "资料核心内容" : topic) +
                              "\n题目数量：" + std::to_string(count) + "\n\n" + context_block(chunks);
    const nlohmann::json request = {
        {"model", chat_model_}, {"instructions", instructions}, {"input", input},
        {"max_output_tokens", 2200},
        {"text", {{"format", {{"type", "json_schema"}, {"name", "quiz"},
                               {"strict", true}, {"schema", schema}}}}}};
    const auto response = openai_.post("/v1/responses", request);
    const auto parsed = nlohmann::json::parse(strip_json_fence(openai_.response_text(response)));

    QuizResult result;
    result.title = parsed.value("title", topic.empty() ? "资料练习" : topic);
    result.used_ai = true;
    for (const auto& item : parsed.at("questions")) {
      QuizQuestion question;
      question.question = item.at("question").get<std::string>();
      question.options = item.at("options").get<std::vector<std::string>>();
      question.answer_index = item.at("answer_index").get<int>();
      question.explanation = item.at("explanation").get<std::string>();
      const int source_index = std::clamp(item.at("source_index").get<int>() - 1, 0,
                                          static_cast<int>(chunks.size()) - 1);
      question.source = source_from(chunks[static_cast<std::size_t>(source_index)]);
      result.questions.push_back(std::move(question));
    }
    if (result.questions.empty()) return fallback_quiz(topic, count, chunks);
    return result;
  } catch (...) {
    return fallback_quiz(topic, count, chunks);
  }
}

void StudyService::save_quiz_attempt(int score, int total) {
  if (total <= 0 || score < 0 || score > total) throw std::runtime_error("Quiz 成绩无效");
  database_.save_quiz_attempt(score, total);
}

AppStats StudyService::stats() { return database_.stats(); }

void StudyService::add_study_seconds(const std::string& date, std::int64_t seconds) {
  if (!std::regex_match(date, std::regex(R"(^\d{4}-\d{2}-\d{2}$)"))) {
    throw std::runtime_error("日期格式应为 YYYY-MM-DD");
  }
  if (seconds < 1 || seconds > 21600) throw std::runtime_error("单次学习时长应为 1 秒到 6 小时");
  database_.add_study_seconds(date, seconds);
}

StudyTimeSummary StudyService::study_time(const std::string& date) {
  if (!std::regex_match(date, std::regex(R"(^\d{4}-\d{2}-\d{2}$)"))) {
    throw std::runtime_error("日期格式应为 YYYY-MM-DD");
  }
  return database_.study_time(date);
}

}  // namespace truth

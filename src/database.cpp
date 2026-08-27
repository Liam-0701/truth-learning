#include "truth/database.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace truth {
namespace {

void check(int code, sqlite3* db, const std::string& context) {
  if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW) {
    throw std::runtime_error(context + ": " + sqlite3_errmsg(db));
  }
}

class Statement {
 public:
  Statement(sqlite3* db, const std::string& sql) : db_(db) {
    check(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr), db, "prepare SQL");
  }

  ~Statement() { sqlite3_finalize(statement_); }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  sqlite3_stmt* get() const { return statement_; }

  void text(int index, const std::string& value) {
    check(sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT), db_, "bind text");
  }

  void integer(int index, std::int64_t value) {
    check(sqlite3_bind_int64(statement_, index, value), db_, "bind integer");
  }

  void real(int index, double value) {
    check(sqlite3_bind_double(statement_, index, value), db_, "bind real");
  }

  void blob(int index, const std::vector<float>& values) {
    const auto bytes = static_cast<int>(values.size() * sizeof(float));
    check(sqlite3_bind_blob(statement_, index, values.data(), bytes, SQLITE_TRANSIENT), db_, "bind blob");
  }

  bool row() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) return true;
    check(result, db_, "step SQL");
    return false;
  }

  void done() { check(sqlite3_step(statement_), db_, "execute SQL"); }

 private:
  sqlite3* db_;
  sqlite3_stmt* statement_{nullptr};
};

std::string column_text(sqlite3_stmt* statement, int column) {
  const auto* value = sqlite3_column_text(statement, column);
  return value ? reinterpret_cast<const char*>(value) : "";
}

}  // namespace

Database::Database(const std::filesystem::path& path) {
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  const int result = sqlite3_open_v2(path.string().c_str(), &db_, flags, nullptr);
  if (result != SQLITE_OK) {
    const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown SQLite error";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("open database: " + message);
  }
  sqlite3_busy_timeout(db_, 5000);
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
}

void Database::exec(const char* sql) {
  char* error = nullptr;
  const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    throw std::runtime_error("execute SQL: " + message);
  }
}

void Database::initialize() {
  std::lock_guard lock(mutex_);
  exec("PRAGMA journal_mode=WAL;");
  exec("PRAGMA foreign_keys=ON;");
  exec(R"sql(
    CREATE TABLE IF NOT EXISTS documents (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT NOT NULL,
      mime_type TEXT NOT NULL,
      object_path TEXT NOT NULL,
      source_type TEXT NOT NULL,
      status TEXT NOT NULL DEFAULT 'ready',
      page_count INTEGER NOT NULL DEFAULT 0,
      chunk_count INTEGER NOT NULL DEFAULT 0,
      created_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
    );

    CREATE TABLE IF NOT EXISTS chunks (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
      content TEXT NOT NULL,
      page INTEGER NOT NULL DEFAULT 1,
      chunk_index INTEGER NOT NULL,
      token_estimate INTEGER NOT NULL DEFAULT 0,
      embedding BLOB NOT NULL,
      embedding_provider TEXT NOT NULL,
      UNIQUE(document_id, chunk_index)
    );

    CREATE INDEX IF NOT EXISTS idx_chunks_document_id ON chunks(document_id);

    CREATE TABLE IF NOT EXISTS quiz_attempts (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      score INTEGER NOT NULL,
      total INTEGER NOT NULL,
      created_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
    );

    CREATE TABLE IF NOT EXISTS study_time_daily (
      date TEXT PRIMARY KEY,
      seconds INTEGER NOT NULL DEFAULT 0 CHECK(seconds >= 0),
      updated_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
    );
  )sql");
}

std::int64_t Database::insert_document(Document document, const std::vector<Chunk>& chunks) {
  std::lock_guard lock(mutex_);
  exec("BEGIN IMMEDIATE;");
  try {
    Statement insert_document(db_, R"sql(
      INSERT INTO documents(name, mime_type, object_path, source_type, status, page_count, chunk_count)
      VALUES (?, ?, ?, ?, ?, ?, ?)
    )sql");
    insert_document.text(1, document.name);
    insert_document.text(2, document.mime_type);
    insert_document.text(3, document.object_path);
    insert_document.text(4, document.source_type);
    insert_document.text(5, document.status);
    insert_document.integer(6, document.page_count);
    insert_document.integer(7, static_cast<std::int64_t>(chunks.size()));
    insert_document.done();

    const std::int64_t document_id = sqlite3_last_insert_rowid(db_);
    for (const auto& chunk : chunks) {
      Statement insert_chunk(db_, R"sql(
        INSERT INTO chunks(document_id, content, page, chunk_index, token_estimate,
                           embedding, embedding_provider)
        VALUES (?, ?, ?, ?, ?, ?, ?)
      )sql");
      insert_chunk.integer(1, document_id);
      insert_chunk.text(2, chunk.content);
      insert_chunk.integer(3, chunk.page);
      insert_chunk.integer(4, chunk.chunk_index);
      insert_chunk.integer(5, chunk.token_estimate);
      insert_chunk.blob(6, chunk.embedding);
      insert_chunk.text(7, chunk.embedding_provider);
      insert_chunk.done();
    }

    exec("COMMIT;");
    return document_id;
  } catch (...) {
    try {
      exec("ROLLBACK;");
    } catch (...) {
    }
    throw;
  }
}

std::vector<Document> Database::list_documents() {
  std::lock_guard lock(mutex_);
  Statement query(db_, R"sql(
    SELECT id, name, mime_type, object_path, source_type, status, page_count, chunk_count, created_at
    FROM documents ORDER BY id DESC
  )sql");
  std::vector<Document> result;
  while (query.row()) {
    auto* row = query.get();
    result.push_back(Document{
        sqlite3_column_int64(row, 0), column_text(row, 1), column_text(row, 2),
        column_text(row, 3), column_text(row, 4), column_text(row, 5),
        sqlite3_column_int(row, 6), sqlite3_column_int(row, 7), column_text(row, 8)});
  }
  return result;
}

std::optional<std::string> Database::delete_document(std::int64_t id) {
  std::lock_guard lock(mutex_);
  Statement find(db_, "SELECT object_path FROM documents WHERE id = ?");
  find.integer(1, id);
  if (!find.row()) return std::nullopt;
  const std::string object_path = column_text(find.get(), 0);

  Statement remove(db_, "DELETE FROM documents WHERE id = ?");
  remove.integer(1, id);
  remove.done();
  return object_path;
}

std::vector<Chunk> Database::load_chunks(const std::vector<std::int64_t>& document_ids) {
  std::lock_guard lock(mutex_);
  std::string sql = R"sql(
    SELECT c.id, c.document_id, d.name, c.content, c.page, c.chunk_index,
           c.token_estimate, c.embedding, c.embedding_provider
    FROM chunks c JOIN documents d ON d.id = c.document_id
  )sql";
  if (!document_ids.empty()) {
    sql += " WHERE c.document_id IN (";
    for (std::size_t index = 0; index < document_ids.size(); ++index) {
      if (index) sql += ',';
      sql += '?';
    }
    sql += ')';
  }
  sql += " ORDER BY c.id";

  Statement query(db_, sql);
  for (std::size_t index = 0; index < document_ids.size(); ++index) {
    query.integer(static_cast<int>(index + 1), document_ids[index]);
  }

  std::vector<Chunk> result;
  while (query.row()) {
    auto* row = query.get();
    Chunk chunk;
    chunk.id = sqlite3_column_int64(row, 0);
    chunk.document_id = sqlite3_column_int64(row, 1);
    chunk.document_name = column_text(row, 2);
    chunk.content = column_text(row, 3);
    chunk.page = sqlite3_column_int(row, 4);
    chunk.chunk_index = sqlite3_column_int(row, 5);
    chunk.token_estimate = sqlite3_column_int(row, 6);
    const void* data = sqlite3_column_blob(row, 7);
    const int byte_count = sqlite3_column_bytes(row, 7);
    if (data && byte_count > 0 && byte_count % static_cast<int>(sizeof(float)) == 0) {
      chunk.embedding.resize(static_cast<std::size_t>(byte_count) / sizeof(float));
      std::memcpy(chunk.embedding.data(), data, static_cast<std::size_t>(byte_count));
    }
    chunk.embedding_provider = column_text(row, 8);
    result.push_back(std::move(chunk));
  }
  return result;
}

void Database::save_quiz_attempt(int score, int total) {
  std::lock_guard lock(mutex_);
  Statement insert(db_, "INSERT INTO quiz_attempts(score, total) VALUES (?, ?)");
  insert.integer(1, score);
  insert.integer(2, total);
  insert.done();
}

AppStats Database::stats() {
  std::lock_guard lock(mutex_);
  AppStats result;
  Statement counts(db_, R"sql(
    SELECT
      (SELECT COUNT(*) FROM documents),
      (SELECT COUNT(*) FROM chunks),
      (SELECT COUNT(*) FROM quiz_attempts),
      COALESCE((SELECT AVG(CASE WHEN total > 0 THEN score * 100.0 / total END) FROM quiz_attempts), 0)
  )sql");
  if (counts.row()) {
    result.document_count = sqlite3_column_int(counts.get(), 0);
    result.chunk_count = sqlite3_column_int(counts.get(), 1);
    result.quiz_attempt_count = sqlite3_column_int(counts.get(), 2);
    result.quiz_average = sqlite3_column_double(counts.get(), 3);
  }
  return result;
}

void Database::add_study_seconds(const std::string& date, std::int64_t seconds) {
  if (seconds <= 0) return;
  std::lock_guard lock(mutex_);
  Statement upsert(db_, R"sql(
    INSERT INTO study_time_daily(date, seconds) VALUES (?, ?)
    ON CONFLICT(date) DO UPDATE SET
      seconds = study_time_daily.seconds + excluded.seconds,
      updated_at = datetime('now', 'localtime')
  )sql");
  upsert.text(1, date);
  upsert.integer(2, seconds);
  upsert.done();
}

StudyTimeSummary Database::study_time(const std::string& today, int recent_days) {
  std::lock_guard lock(mutex_);
  StudyTimeSummary result;

  Statement totals(db_, R"sql(
    SELECT
      COALESCE(SUM(CASE WHEN date = ? THEN seconds ELSE 0 END), 0),
      COALESCE(SUM(seconds), 0),
      COUNT(CASE WHEN seconds > 0 THEN 1 END)
    FROM study_time_daily
  )sql");
  totals.text(1, today);
  if (totals.row()) {
    result.today_seconds = sqlite3_column_int64(totals.get(), 0);
    result.total_seconds = sqlite3_column_int64(totals.get(), 1);
    result.active_days = sqlite3_column_int(totals.get(), 2);
  }

  Statement recent(db_, "SELECT date, seconds FROM study_time_daily ORDER BY date DESC LIMIT ?");
  recent.integer(1, std::max(1, recent_days));
  while (recent.row()) {
    result.recent.push_back({column_text(recent.get(), 0), sqlite3_column_int64(recent.get(), 1)});
  }
  std::reverse(result.recent.begin(), result.recent.end());
  return result;
}

}  // namespace truth


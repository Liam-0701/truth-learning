cmake_minimum_required(VERSION 3.24)

project(truth_learning VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CTest)

option(TRUTH_ENABLE_PDF "Enable PDF text extraction when poppler-cpp is available" ON)
option(TRUTH_ENABLE_OFFICE "Enable PPTX/DOCX extraction when libzip is available" ON)

set(HTTPLIB_COMPILE OFF CACHE BOOL "Use header-only cpp-httplib" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "TLS is handled by libcurl" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "HTTP server does not need TLS" FORCE)
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "Avoid Windows resolver API mismatch" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "Keep server dependencies small" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "Keep server dependencies small" FORCE)
FetchContent_Declare(
  cpp_httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.27.0.tar.gz
  URL_HASH SHA256=cc57615af359efda816122dcfca37bcbb9f1591396f50a1fd1ad70bbe6050581
)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
)
FetchContent_MakeAvailable(cpp_httplib nlohmann_json)

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND AND TRUTH_ENABLE_PDF)
  pkg_check_modules(POPPLER_CPP QUIET IMPORTED_TARGET poppler-cpp)
endif()

if(PkgConfig_FOUND AND TRUTH_ENABLE_OFFICE)
  pkg_check_modules(LIBZIP QUIET IMPORTED_TARGET libzip)
endif()

add_library(truth_learning_core
  src/database.cpp
  src/document_processor.cpp
  src/embeddings.cpp
  src/openai_client.cpp
  src/service.cpp
)

target_include_directories(truth_learning_core PUBLIC include)
target_link_libraries(truth_learning_core
  PUBLIC
    SQLite::SQLite3
    CURL::libcurl
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(POPPLER_CPP_FOUND)
  target_compile_definitions(truth_learning_core PUBLIC TRUTH_HAS_POPPLER=1)
  target_link_libraries(truth_learning_core PUBLIC PkgConfig::POPPLER_CPP)
  message(STATUS "PDF extraction: enabled")
else()
  message(STATUS "PDF extraction: disabled (install poppler-cpp to enable)")
endif()

if(LIBZIP_FOUND)
  target_compile_definitions(truth_learning_core PUBLIC TRUTH_HAS_LIBZIP=1)
  target_link_libraries(truth_learning_core PUBLIC PkgConfig::LIBZIP)
  message(STATUS "PPTX/DOCX extraction: enabled")
else()
  message(STATUS "PPTX/DOCX extraction: disabled (install libzip to enable)")
endif()

if(MSVC)
  target_compile_options(truth_learning_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(truth_learning_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(truth_learning_server server/main.cpp)
target_link_libraries(truth_learning_server PRIVATE truth_learning_core httplib::httplib)

if(WIN32)
  target_compile_definitions(truth_learning_server PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_link_libraries(truth_learning_server PRIVATE ws2_32)
endif()

if(BUILD_TESTING)
  add_executable(truth_learning_tests tests/core_tests.cpp)
  target_link_libraries(truth_learning_tests PRIVATE truth_learning_core)
  add_test(NAME truth_learning_core_tests COMMAND truth_learning_tests)
endif()

install(TARGETS truth_learning_server RUNTIME DESTINATION bin)
install(DIRECTORY public/ DESTINATION share/truth-learning/public)

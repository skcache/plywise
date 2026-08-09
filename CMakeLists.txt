cmake_minimum_required(VERSION 3.25)

project(personal_chess_tutor VERSION 0.3.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PCT_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(PCT_ENABLE_THREAD_SANITIZER "Enable the thread data-race sanitizer" OFF)
option(PCT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PCT_BUILD_BENCHMARKS "Build reproducible microbenchmarks" ON)
option(PCT_ENABLE_POSTGRES_ADAPTER "Build the hosted PostgreSQL repository adapter when libpq is available" ON)
option(PCT_ENABLE_OIDC_AUTH "Build the hosted OIDC token verifier when OpenSSL is available" ON)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")
configure_file(resources/catalog.json resources/catalog.json COPYONLY)
configure_file(resources/openings.json resources/openings.json COPYONLY)
configure_file(resources/tactical-corpus.json resources/tactical-corpus.json COPYONLY)

add_library(pct_core
    src/common/json.cpp
    src/app/repository.cpp
    src/app/job_manager.cpp
    src/app/ingest_manager.cpp
    src/chess/board.cpp
    src/chess/bitboard.cpp
    src/chess/fen.cpp
    src/chess/san.cpp
    src/chess/pgn.cpp
    src/import/import_service.cpp
    src/import/chesscom_archive_client.cpp
    src/engine/stockfish.cpp
    src/engine/pool.cpp
    src/analysis/analyzer.cpp
    src/analysis/browser_observation.cpp
    src/analysis/opening_book.cpp
    src/training/training.cpp
    src/training/tactical_corpus.cpp
    src/storage/event_log.cpp
    src/service/http_server.cpp
    src/service/hosted_runtime.cpp
)

target_include_directories(pct_core PUBLIC include)
target_link_libraries(pct_core PUBLIC Threads::Threads CURL::libcurl)

if(PCT_ENABLE_OIDC_AUTH)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_sources(pct_core PRIVATE src/service/oidc_token_verifier.cpp)
        target_link_libraries(pct_core PUBLIC OpenSSL::Crypto)
        target_compile_definitions(pct_core PUBLIC PCT_HAS_OIDC=1)
    else()
        message(STATUS "OpenSSL was not found; hosted OIDC token verification is disabled")
    endif()
endif()

if(PCT_ENABLE_POSTGRES_ADAPTER)
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        target_sources(pct_core PRIVATE src/app/postgres_repository.cpp)
        target_include_directories(pct_core PRIVATE ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(pct_core PUBLIC ${PostgreSQL_LIBRARIES} OpenSSL::Crypto)
        target_compile_definitions(pct_core PUBLIC PCT_HAS_POSTGRES=1)
    else()
        message(STATUS "libpq was not found; hosted PostgreSQL adapter is disabled")
    endif()
endif()
target_compile_definitions(
    pct_core PRIVATE
        PCT_RESOURCE_CATALOG_PATH="${CMAKE_CURRENT_BINARY_DIR}/resources/catalog.json"
        PCT_OPENING_BOOK_PATH="${CMAKE_CURRENT_BINARY_DIR}/resources/openings.json")

if(MSVC)
    target_compile_options(pct_core PRIVATE /W4)
else()
    target_compile_options(pct_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(PCT_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(pct_core PRIVATE /WX)
    else()
        target_compile_options(pct_core PRIVATE -Werror)
    endif()
endif()

if(PCT_ENABLE_SANITIZERS AND PCT_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "Address/undefined and thread sanitizers must use separate builds")
endif()

if(PCT_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(pct_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pct_core PUBLIC -fsanitize=address,undefined)
endif()

if(PCT_ENABLE_THREAD_SANITIZER AND NOT MSVC)
    target_compile_options(pct_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(pct_core PUBLIC -fsanitize=thread)
endif()

add_executable(personal-chess-tutor src/main.cpp)
target_link_libraries(personal-chess-tutor PRIVATE pct_core)

add_executable(pct-cli src/cli.cpp)
target_link_libraries(pct-cli PRIVATE pct_core)

if(PCT_BUILD_BENCHMARKS)
    add_executable(pct-benchmarks benchmarks/benchmark_main.cpp)
    target_link_libraries(pct-benchmarks PRIVATE pct_core)
endif()

if(BUILD_TESTING)
    add_executable(pct-tests
        tests/test_main.cpp
        tests/chess_tests.cpp
        tests/bitboard_tests.cpp
        tests/pgn_tests.cpp
        tests/import_tests.cpp
        tests/engine_tests.cpp
        tests/pool_tests.cpp
        tests/analysis_tests.cpp
        tests/browser_observation_tests.cpp
        tests/storage_tests.cpp
        tests/json_tests.cpp
        tests/repository_tests.cpp
        tests/job_tests.cpp
        tests/api_tests.cpp
        tests/chesscom_archive_tests.cpp
        tests/chesscom_repository_tests.cpp
        tests/chesscom_api_tests.cpp
        tests/ingest_manager_tests.cpp
        tests/training_tests.cpp
        tests/corpus_tests.cpp
        tests/phase2_e2e_tests.cpp
        tests/phase3_e2e_tests.cpp
        tests/hosted_vertical_slice_tests.cpp
    )
    if(OpenSSL_FOUND AND PCT_ENABLE_OIDC_AUTH)
        target_sources(pct-tests PRIVATE tests/oidc_token_verifier_tests.cpp)
    endif()
    if(PostgreSQL_FOUND AND PCT_ENABLE_POSTGRES_ADAPTER)
        target_sources(pct-tests PRIVATE tests/postgres_repository_tests.cpp)
    endif()
    target_link_libraries(pct-tests PRIVATE pct_core)
    add_executable(pct-fake-stockfish tests/fake_stockfish.cpp)
    add_executable(pct-crash-stockfish tests/crash_stockfish.cpp)
    add_executable(pct-slow-stockfish tests/slow_stockfish.cpp)
    add_dependencies(pct-tests pct-fake-stockfish pct-crash-stockfish pct-slow-stockfish)
    target_compile_definitions(pct-tests PRIVATE
        PCT_FAKE_STOCKFISH_PATH="$<TARGET_FILE:pct-fake-stockfish>"
        PCT_CRASH_STOCKFISH_PATH="$<TARGET_FILE:pct-crash-stockfish>"
        PCT_SLOW_STOCKFISH_PATH="$<TARGET_FILE:pct-slow-stockfish>"
        PCT_TACTICAL_CORPUS_PATH="${CMAKE_CURRENT_BINARY_DIR}/resources/tactical-corpus.json")
    add_executable(pct-ingest-tests
        tests/test_main.cpp
        tests/chesscom_archive_tests.cpp
        tests/chesscom_repository_tests.cpp
        tests/chesscom_api_tests.cpp
        tests/ingest_manager_tests.cpp
    )
    target_link_libraries(pct-ingest-tests PRIVATE pct_core)
    add_executable(pct-integration-tests
        tests/test_main.cpp
        tests/chesscom_api_tests.cpp
        tests/ingest_manager_tests.cpp
    )
    target_link_libraries(pct-integration-tests PRIVATE pct_core)
    add_test(NAME pct-tests COMMAND pct-tests)
    add_test(NAME pct-ingest-tests COMMAND pct-ingest-tests)
    add_test(NAME pct-integration-tests COMMAND pct-integration-tests)
endif()

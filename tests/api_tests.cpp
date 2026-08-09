#include "test.hpp"

#include "pct/service/http_server.hpp"

#include <filesystem>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace pct;

namespace {

class ApiEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken) override {
        chess::Board board = chess::Board::from_fen(request.fen);
        const auto moves = board.legal_moves();
        const std::string best = moves.empty() ? "(none)" : chess::uci(moves.front());
        return {{{1, request.depth, 0, std::nullopt, 1, 1, {best}}}, best, {}};
    }
};

struct ApiFixture {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("pct-api-" + std::to_string(::getpid()) + ".log");
    storage::EventLog log;
    app::Repository repository;
    import::ImportService importer;
    ApiEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer;
    app::JobManager jobs;
    service::Api api;

    ApiFixture(service::Api::Diagnostics diagnostics = {},
               service::Api::AdvancedDrills advanced_drills = {})
        : log((std::filesystem::remove(path), path)), repository(log), analyzer(engine, cache),
          jobs(repository, analyzer),
          api(importer, repository, jobs, std::move(diagnostics), std::move(advanced_drills)) {}
    ~ApiFixture() = default;
};

} // namespace

TEST_CASE("API imports PGN and returns navigable game data immediately") {
    ApiFixture fixture;
    const std::string body = json::dump(json::Value::Object{
        {"pgn", "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"},
    });
    const auto imported = fixture.api.handle(service::Request{"POST", "/api/import", {}, body});
    CHECK_EQ(imported.status, 202);
    const auto imported_body = json::parse(imported.body);
    CHECK(!imported_body.as_object().contains("job"));
    CHECK(fixture.jobs.list().empty());
    const std::string id = imported_body.at("game_id").as_string();
    const auto game = fixture.api.handle(service::Request{"GET", "/api/games/" + id, {}, {}});
    CHECK_EQ(game.status, 200);
    CHECK_EQ(json::parse(game.body).at("game").at("plies").as_array().size(), 2ULL);
    const auto move =
        fixture.api.handle(service::Request{"GET", "/api/games/" + id + "/moves/0", {}, {}});
    CHECK_EQ(move.status, 200);
    CHECK_EQ(json::parse(move.body).at("san").as_string(), "e4");
}

TEST_CASE("API variation contract validates moves and preserves sibling branches") {
    ApiFixture fixture;
    const std::string import_body = json::dump(json::Value::Object{
        {"pgn", "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"},
    });
    const auto imported = fixture.api.handle(
        service::Request{"POST", "/api/import", {}, import_body});
    const std::string game_id = json::parse(imported.body).at("game_id").as_string();
    const std::string base = "/api/games/" + game_id + "/variations";

    const auto created = fixture.api.handle(service::Request{
        "POST", base, {},
        json::dump(json::Value::Object{{"root_ply", 0}, {"root_position", "after"}})});
    CHECK_EQ(created.status, 201);
    const auto created_body = json::parse(created.body);
    const std::string variation_id = created_body.at("id").as_string();
    CHECK_EQ(created_body.at("root_position").as_string(), "after");
    CHECK_EQ(created_body.at("nodes").as_array().size(), 1ULL);

    const auto initial = fixture.api.handle(service::Request{
        "POST", base, {},
        json::dump(json::Value::Object{{"root_ply", 0}, {"root_position", "before"}})});
    CHECK_EQ(initial.status, 201);
    const auto initial_body = json::parse(initial.body);
    CHECK_EQ(initial_body.at("root_position").as_string(), "before");
    const std::string initial_path = base + "/" + initial_body.at("id").as_string();
    const auto initial_move = fixture.api.handle(service::Request{
        "POST", initial_path + "/moves", {},
        json::dump(json::Value::Object{{"node_id", 0}, {"uci", "d2d4"}})});
    CHECK_EQ(initial_move.status, 200);
    CHECK_EQ(json::parse(initial_move.body).at("nodes").as_array().size(), 2ULL);

    const std::string variation_path = base + "/" + variation_id;
    const auto first = fixture.api.handle(service::Request{
        "POST", variation_path + "/moves", {},
        json::dump(json::Value::Object{{"node_id", 0}, {"uci", "e7e5"}})});
    CHECK_EQ(first.status, 200);
    CHECK_EQ(json::parse(first.body).at("nodes").as_array().size(), 2ULL);
    const auto branch_analysis = fixture.api.handle(
        service::Request{"POST", variation_path + "/analysis", {}, "{}"});
    CHECK_EQ(branch_analysis.status, 200);
    CHECK(!json::parse(branch_analysis.body).at("best_move").as_string().empty());
    CHECK_EQ(json::parse(branch_analysis.body).at("lines").as_array().size(), 1ULL);

    const auto reset = fixture.api.handle(
        service::Request{"POST", variation_path + "/reset", {}, "{}"});
    CHECK_EQ(reset.status, 200);
    CHECK_EQ(json::parse(reset.body).at("current_node_id").as_size(), 0ULL);
    const auto sibling = fixture.api.handle(service::Request{
        "POST", variation_path + "/moves", {},
        json::dump(json::Value::Object{{"node_id", 0}, {"uci", "c7c5"}})});
    CHECK_EQ(sibling.status, 200);
    const auto sibling_body = json::parse(sibling.body);
    CHECK_EQ(sibling_body.at("nodes").as_array().size(), 3ULL);
    CHECK_EQ(sibling_body.at("nodes").as_array().front().at("children").as_array().size(), 2ULL);

    const auto illegal = fixture.api.handle(service::Request{
        "POST", variation_path + "/moves", {},
        json::dump(json::Value::Object{{"node_id", 0}, {"uci", "e2e4"}})});
    CHECK_EQ(illegal.status, 400);
    CHECK_EQ(json::parse(illegal.body).at("code").as_string(), "illegal_move");

    const auto listed = fixture.api.handle(service::Request{"GET", base, {}, {}});
    CHECK_EQ(listed.status, 200);
    CHECK_EQ(json::parse(listed.body).at("variations").as_array().size(), 2ULL);
    CHECK_EQ(fixture.api.handle(service::Request{"GET", variation_path, {}, {}}).status, 200);
    CHECK_EQ(fixture.api.handle(service::Request{"DELETE", variation_path, {}, {}}).status, 200);
    CHECK_EQ(fixture.api.handle(service::Request{"GET", variation_path, {}, {}}).status, 404);
}

TEST_CASE("API retry contract validates and persists analyzed move attempts") {
    ApiFixture fixture;
    const std::string import_body = json::dump(json::Value::Object{
        {"pgn", "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"},
    });
    const auto imported = fixture.api.handle(
        service::Request{"POST", "/api/import", {}, import_body});
    const std::string game_id = json::parse(imported.body).at("game_id").as_string();
    const auto stored = fixture.repository.get(game_id);
    CHECK(stored.has_value());
    analysis::GameAnalysis completed;
    completed.game_id = game_id;
    analysis::MoveAssessment move;
    move.ply = 0;
    move.played_uci = "e2e4";
    move.best_uci = "e2e4";
    move.acceptable_alternatives = {"d2d4"};
    move.fen_before = stored->imported.game.plies[0].fen_before;
    move.fen_after = stored->imported.game.plies[0].fen_after;
    completed.moves.push_back(move);
    fixture.repository.save_analysis(completed);

    const std::string retry_path = "/api/games/" + game_id + "/moves/0/retry";
    const auto accepted = fixture.api.handle(service::Request{
        "POST", retry_path, {}, json::dump(json::Value::Object{{"uci", "d2d4"}})});
    CHECK_EQ(accepted.status, 201);
    CHECK(json::parse(accepted.body).at("accepted").as_bool());
    const auto legal_other = fixture.api.handle(service::Request{
        "POST", retry_path, {}, json::dump(json::Value::Object{{"uci", "a2a3"}})});
    CHECK_EQ(legal_other.status, 201);
    CHECK(!json::parse(legal_other.body).at("accepted").as_bool());
    const auto illegal = fixture.api.handle(service::Request{
        "POST", retry_path, {}, json::dump(json::Value::Object{{"uci", "e7e5"}})});
    CHECK_EQ(illegal.status, 400);
    CHECK_EQ(json::parse(illegal.body).at("code").as_string(), "illegal_move");
    const auto attempts = fixture.api.handle(service::Request{
        "GET", "/api/games/" + game_id + "/retry-attempts", {}, {}});
    CHECK_EQ(attempts.status, 200);
    CHECK_EQ(json::parse(attempts.body).at("attempts").as_array().size(), 2ULL);
}

TEST_CASE("API validates request shape and unknown resources") {
    ApiFixture fixture;
    CHECK_EQ(fixture.api.handle(service::Request{"POST", "/api/import", {}, "{}"}).status, 400);
    CHECK_EQ(fixture.api.handle(service::Request{"GET", "/api/games/missing", {}, {}}).status, 404);
    CHECK_EQ(fixture.api.handle(service::Request{"GET", "/api/unknown", {}, {}}).status, 404);
}

TEST_CASE("API distinguishes liveness from engine readiness") {
    ApiFixture fixture;
    service::Api degraded(fixture.importer, fixture.repository, fixture.jobs, {}, {}, nullptr, [] {
        return service::Readiness{true, false, false, "/missing/stockfish"};
    });
    const auto health = degraded.handle(service::Request{"GET", "/api/health", {}, {}});
    CHECK_EQ(health.status, 200);
    CHECK_EQ(json::parse(health.body).at("status").as_string(), "ok");
    CHECK(!json::parse(health.body).at("local_only").as_bool());

    const auto ready = degraded.handle(service::Request{"GET", "/api/ready", {}, {}});
    CHECK_EQ(ready.status, 503);
    const auto body = json::parse(ready.body);
    CHECK_EQ(body.at("status").as_string(), "not_ready");
    CHECK_EQ(body.at("components").at("api").as_string(), "ready");
    CHECK_EQ(body.at("components").at("storage").as_string(), "ready");
    CHECK_EQ(body.at("components").at("engine").as_string(), "unavailable");
}

TEST_CASE("API hosted mode fails closed and scopes authenticated requests") {
    ApiFixture fixture;
    const service::AuthConfig auth{
        true,
        [](std::string_view token) -> std::optional<app::OwnerId> {
            if (token == "local-token")
                return app::OwnerId::local();
            if (token == "other-owner")
                return app::OwnerId::account("account-02");
            return std::nullopt;
        },
    };
    service::Api hosted(fixture.importer, fixture.repository, fixture.jobs, {}, {}, nullptr, {},
                        auth);

    const auto health = hosted.handle(service::Request{"GET", "/api/health", {}, {}});
    CHECK_EQ(health.status, 200);
    CHECK(json::parse(health.body).at("auth_required").as_bool());
    CHECK(!json::parse(health.body).as_object().contains("games"));

    const auto missing = hosted.handle(service::Request{"GET", "/api/games", {}, {}});
    CHECK_EQ(missing.status, 401);
    CHECK_EQ(missing.headers.at("WWW-Authenticate"), "Bearer");
    CHECK_EQ(json::parse(missing.body).at("code").as_string(), "auth_required");

    const auto malformed = hosted.handle(
        service::Request{"GET", "/api/games", {{"authorization", "Basic secret"}}, {}});
    CHECK_EQ(malformed.status, 401);
    CHECK_EQ(json::parse(malformed.body).at("code").as_string(), "invalid_token");

    const auto invalid = hosted.handle(
        service::Request{"GET", "/api/games", {{"authorization", "Bearer wrong"}}, {}});
    CHECK_EQ(invalid.status, 401);

    const auto wrong_owner = hosted.handle(
        service::Request{"GET", "/api/games", {{"authorization", "Bearer other-owner"}}, {}});
    CHECK_EQ(wrong_owner.status, 403);
    CHECK_EQ(json::parse(wrong_owner.body).at("code").as_string(), "forbidden");

    const auto valid = hosted.handle(
        service::Request{"GET", "/api/games", {{"authorization", "Bearer local-token"}}, {}});
    CHECK_EQ(valid.status, 200);

    service::Api unavailable(fixture.importer, fixture.repository, fixture.jobs, {}, {}, nullptr,
                             {}, service::AuthConfig{true, {}});
    const auto unavailable_response =
        unavailable.handle(service::Request{"GET", "/api/games", {}, {}});
    CHECK_EQ(unavailable_response.status, 503);
    CHECK_EQ(json::parse(unavailable_response.body).at("code").as_string(), "auth_unavailable");
}

TEST_CASE("API scoped auth routes each request through its resolved owner resources") {
    ApiFixture default_scope;
    ApiFixture resolved_scope;
    const service::AuthConfig auth{
        true,
        [](std::string_view token) -> std::optional<app::OwnerId> {
            if (token == "resolved-owner")
                return app::OwnerId::local();
            if (token == "missing-scope")
                return app::OwnerId::account("account-without-runtime");
            return std::nullopt;
        },
        [&](const app::OwnerId& owner) -> std::optional<service::ApiScope> {
            if (owner == app::OwnerId::local())
                return service::ApiScope{&resolved_scope.repository, &resolved_scope.jobs};
            if (owner.kind() == app::OwnerKind::Account)
                return service::ApiScope{&default_scope.repository, &default_scope.jobs};
            return std::nullopt;
        },
    };
    service::Api scoped(default_scope.importer, default_scope.repository, default_scope.jobs,
                        {}, {}, nullptr, {}, auth);
    const std::string body = json::dump(json::Value::Object{
        {"pgn", "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"},
    });

    const auto imported = scoped.handle(
        service::Request{"POST", "/api/import", {{"authorization", "Bearer resolved-owner"}}, body});
    CHECK_EQ(imported.status, 202);
    CHECK_EQ(default_scope.repository.size(), 0ULL);
    CHECK_EQ(resolved_scope.repository.size(), 1ULL);

    const auto listed = scoped.handle(
        service::Request{"GET", "/api/games", {{"authorization", "Bearer resolved-owner"}}, {}});
    CHECK_EQ(listed.status, 200);
    CHECK_EQ(json::parse(listed.body).at("games").as_array().size(), 1ULL);

    const auto unavailable = scoped.handle(service::Request{
        "GET", "/api/games", {{"authorization", "Bearer missing-scope"}}, {}});
    CHECK_EQ(unavailable.status, 503);
    CHECK_EQ(json::parse(unavailable.body).at("code").as_string(), "storage_unavailable");
}

TEST_CASE("API path router decodes identifiers safely") {
    ApiFixture fixture;
    const auto response = fixture.api.handle(
        service::Request{"GET", "/api/drills/game%3A0", {}, {}});
    CHECK_EQ(response.status, 404);
    CHECK_EQ(fixture.api.handle(service::Request{"GET", "/api/drills/bad%XX", {}, {}}).status,
             400);
}

TEST_CASE("API exposes settings mistakes and analysis contracts") {
    ApiFixture fixture;
    const auto settings = fixture.api.handle(service::Request{"GET", "/api/settings", {}, {}});
    CHECK_EQ(settings.status, 200);
    CHECK_EQ(json::parse(settings.body).at("bind_address").as_string(), "127.0.0.1");
    const auto mistakes = fixture.api.handle(service::Request{"GET", "/api/mistakes", {}, {}});
    CHECK_EQ(mistakes.status, 200);
    CHECK(json::parse(mistakes.body).at("mistakes").as_array().empty());
}

TEST_CASE("API exposes bounded diagnostics and persists supplemental drills") {
    ApiFixture fixture(
        [] { return json::Value::Object{{"engine_workers", 2}}; },
        [] {
            training::Drill drill;
            drill.id = "corpus:test";
            drill.source_game_id = "corpus:test";
            drill.fen = chess::Board::initial().to_fen();
            drill.category = "Fork";
            drill.phase = "opening";
            drill.explanation = "Validated test puzzle";
            drill.solutions = {"a2a3"};
            drill.source_type = "public_corpus";
            drill.provenance = "https://database.lichess.org/#puzzles | CC0";
            drill.corpus_version = "test-1";
            drill.validation_evidence = {"legal", "verifier A", "verifier B"};
            return std::vector<training::Drill>{drill};
        });
    const auto diagnostics =
        fixture.api.handle(service::Request{"GET", "/api/diagnostics", {}, {}});
    CHECK_EQ(diagnostics.status, 200);
    const auto diagnostic_body = json::parse(diagnostics.body);
    CHECK_EQ(diagnostic_body.at("engine_workers").as_size(), 2ULL);
    CHECK_EQ(diagnostic_body.at("job_queue_capacity").as_size(), 256ULL);
    CHECK(diagnostic_body.as_object().contains("analysis_cache_evictions"));

    const auto generated = fixture.api.handle(
        service::Request{"POST", "/api/drills/supplemental", {}, "{}"});
    CHECK_EQ(generated.status, 200);
    CHECK_EQ(json::parse(generated.body).at("added").as_size(), 1ULL);
    const auto stored = fixture.repository.drill("corpus:test");
    CHECK(stored.has_value());
    CHECK_EQ(stored->source_type, "public_corpus");
}

TEST_CASE("API exposes phase two profile drills resources and batch contracts") {
    ApiFixture fixture;
    CHECK_EQ(fixture.api.handle(service::Request{"GET", "/api/drills", {}, {}}).status, 200);
    const auto profile = fixture.api.handle(service::Request{"GET", "/api/profile", {}, {}});
    CHECK_EQ(profile.status, 200);
    CHECK_EQ(json::parse(profile.body).at("projection_version").as_string(), "profile-1");
    CHECK_EQ(fixture.api.handle(service::Request{"GET", "/api/resources", {}, {}}).status, 200);
    const std::string pgn = "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0";
    const std::string body = json::dump(json::Value::Object{
        {"pgns", json::Value::Array{pgn, pgn, "invalid"}},
    });
    const auto batch = fixture.api.handle(service::Request{"POST", "/api/import/batch", {}, body});
    CHECK_EQ(batch.status, 202);
    const auto result = json::parse(batch.body);
    CHECK_EQ(result.at("discovered").as_size(), 3ULL);
    CHECK_EQ(result.at("imported").as_size(), 1ULL);
    CHECK_EQ(result.at("duplicates").as_size(), 1ULL);
    CHECK_EQ(result.at("failed").as_size(), 1ULL);
    CHECK_EQ(result.at("batch_id").as_string(), "batch-1");
    const auto batches = fixture.api.handle(service::Request{"GET", "/api/batches", {}, {}});
    CHECK_EQ(batches.status, 200);
    CHECK_EQ(json::parse(batches.body).at("batches").as_array().size(), 1ULL);
    CHECK(json::parse(batches.body).as_object().contains("cache_hits"));
}

TEST_CASE("batch import applies bounded backpressure") {
    ApiFixture fixture;
    json::Value::Array pgns;
    for (int index = 0; index < 101; ++index)
        pgns.emplace_back("invalid");
    const auto response = fixture.api.handle(service::Request{
        "POST", "/api/import/batch", {},
        json::dump(json::Value::Object{{"pgns", std::move(pgns)}})});
    CHECK_EQ(response.status, 400);
}

TEST_CASE("storage maintenance API creates snapshots and performs verified compaction") {
    ApiFixture fixture;
    const auto snapshot =
        fixture.api.handle(service::Request{"POST", "/api/storage/snapshot", {}, "{}"});
    CHECK_EQ(snapshot.status, 200);
    CHECK(json::parse(snapshot.body).at("created").as_bool());
    const auto compact =
        fixture.api.handle(service::Request{"POST", "/api/storage/compact", {}, "{}"});
    CHECK_EQ(compact.status, 200);
    CHECK(json::parse(compact.body).at("compacted").as_bool());
}

TEST_CASE("drill session and hint endpoints reject unknown drills") {
    ApiFixture fixture;
    CHECK_EQ(fixture.api.handle(
                 service::Request{"POST", "/api/drills/missing/session", {}, "{}"})
                 .status,
             404);
    CHECK_EQ(fixture.api.handle(
                 service::Request{"POST", "/api/drills/missing/hint", {}, "{}"})
                 .status,
             404);
}

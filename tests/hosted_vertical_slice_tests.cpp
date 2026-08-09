#include "test.hpp"

#include "pct/app/hosted_identity.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/import/import_service.hpp"
#include "pct/service/http_server.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

using namespace pct;

#ifdef PCT_HAS_POSTGRES

namespace {

class VerticalSliceEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken) override {
        chess::Board board = chess::Board::from_fen(request.fen);
        const auto legal_moves = board.legal_moves();
        const std::string best = legal_moves.empty() ? "(none)" : chess::uci(legal_moves.front());
        engine::AnalysisResult result;
        result.best_move = best;
        for (int multipv = 1; multipv <= request.multipv; ++multipv) {
            result.lines.push_back({multipv, request.depth, 120 - multipv, std::nullopt, 100,
                                    1, {best}});
        }
        return result;
    }
};

std::string vertical_slice_pgn() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return "[Event \"Hosted vertical " + std::to_string(stamp) +
           "\"]\n[White \"Hosted user\"]\n[Black \"Saved review\"]\n"
           "[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 1-0";
}

struct ScopedApi {
    service::Api api;

    ScopedApi(import::ImportService& importer, app::IRepository& default_repository,
              app::JobManager& default_jobs, app::IRepository& alice_repository,
              app::JobManager& alice_jobs, app::IRepository& bob_repository,
              app::JobManager& bob_jobs, std::string alice_id, std::string bob_id)
        : api(importer, default_repository, default_jobs, {}, {}, nullptr, {}, [&]() {
              service::AuthConfig auth;
              auth.required = true;
              auth.verify = [alice_id, bob_id](std::string_view token)
                  -> std::optional<app::OwnerId> {
                  if (token == "alice-token")
                      return app::OwnerId::account(alice_id);
                  if (token == "bob-token")
                      return app::OwnerId::account(bob_id);
                  return std::nullopt;
              };
              auth.resolve_scope = [&, alice_id, bob_id](const app::OwnerId& owner)
                  -> std::optional<service::ApiScope> {
                  if (owner == app::OwnerId::account(alice_id))
                      return service::ApiScope{&alice_repository, &alice_jobs};
                  if (owner == app::OwnerId::account(bob_id))
                      return service::ApiScope{&bob_repository, &bob_jobs};
                  return std::nullopt;
              };
              return auth;
          }()) {}
};

service::Request account_request(std::string method, std::string path, std::string body = {},
                                 std::string token = "alice-token") {
    return service::Request{std::move(method), std::move(path),
                            {{"authorization", "Bearer " + std::move(token)}},
                            std::move(body)};
}

} // namespace

TEST_CASE("hosted vertical slice persists an account review and denies another owner") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    VerticalSliceEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 3, 2});
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    app::HostedIdentityStore identity(connection);
    const auto alice = identity.ensure_account("test", "vertical-alice-" + std::to_string(stamp));
    const auto bob = identity.ensure_account("test", "vertical-bob-" + std::to_string(stamp));
    app::PostgresRepository alice_repository(connection, alice.owner());
    app::PostgresRepository bob_repository(connection, bob.owner());
    app::JobManager alice_jobs(alice_repository, analyzer, app::JobManagerOptions{1, 8, 1});
    app::JobManager bob_jobs(bob_repository, analyzer, app::JobManagerOptions{1, 8, 1});
    import::ImportService importer;
    ScopedApi hosted(importer, alice_repository, alice_jobs, alice_repository, alice_jobs,
                     bob_repository, bob_jobs, alice.id, bob.id);

    const std::string pgn = vertical_slice_pgn();
    const auto imported = hosted.api.handle(account_request(
        "POST", "/api/import", json::dump(json::Value::Object{{"pgn", pgn}})));
    CHECK_EQ(imported.status, 202);
    const std::string game_id = json::parse(imported.body).at("game_id").as_string();

    const auto alice_games = hosted.api.handle(account_request("GET", "/api/games"));
    CHECK_EQ(alice_games.status, 200);
    CHECK(!json::parse(alice_games.body).at("games").as_array().empty());
    const auto bob_games = hosted.api.handle(account_request("GET", "/api/games", {}, "bob-token"));
    CHECK_EQ(bob_games.status, 200);
    CHECK(json::parse(bob_games.body).at("games").as_array().empty());

    const auto started = hosted.api.handle(
        account_request("POST", "/api/games/" + game_id + "/analysis"));
    CHECK_EQ(started.status, 202);
    const auto job_id = static_cast<std::uint64_t>(json::parse(started.body).at("id").as_number());
    for (int attempt = 0; attempt < 400; ++attempt) {
        const auto job = alice_jobs.get(job_id);
        if (job && (job->status == app::JobStatus::Complete ||
                    job->status == app::JobStatus::Failed))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto completed_job = alice_jobs.get(job_id);
    CHECK(completed_job.has_value());
    CHECK(completed_job->status == app::JobStatus::Complete);

    const auto review = hosted.api.handle(
        account_request("GET", "/api/games/" + game_id + "/analysis"));
    CHECK_EQ(review.status, 200);
    CHECK(json::parse(review.body).as_object().contains("moves"));

    const auto variation = hosted.api.handle(account_request(
        "POST", "/api/games/" + game_id + "/variations",
        json::dump(json::Value::Object{{"root_ply", 0}, {"root_position", "before"}})));
    CHECK_EQ(variation.status, 201);
    const std::string variation_id = json::parse(variation.body).at("id").as_string();
    const auto branch = hosted.api.handle(account_request(
        "POST", "/api/games/" + game_id + "/variations/" + variation_id + "/moves",
        json::dump(json::Value::Object{{"node_id", 0}, {"uci", "e2e4"}})));
    CHECK_EQ(branch.status, 200);
    const auto retry = hosted.api.handle(account_request(
        "POST", "/api/games/" + game_id + "/moves/0/retry",
        json::dump(json::Value::Object{{"uci", "e2e4"}})));
    CHECK_EQ(retry.status, 201);

    const auto bob_cannot_read = hosted.api.handle(
        account_request("GET", "/api/games/" + game_id, {}, "bob-token"));
    CHECK_EQ(bob_cannot_read.status, 404);

    app::PostgresRepository reopened_repository(connection, alice.owner());
    app::JobManager reopened_jobs(reopened_repository, analyzer, app::JobManagerOptions{1, 8, 1});
    ScopedApi reopened(importer, reopened_repository, reopened_jobs, reopened_repository,
                       reopened_jobs, bob_repository, bob_jobs, alice.id, bob.id);
    const auto reopened_review = reopened.api.handle(
        account_request("GET", "/api/games/" + game_id + "/analysis"));
    CHECK_EQ(reopened_review.status, 200);
    const auto reopened_variations = reopened.api.handle(
        account_request("GET", "/api/games/" + game_id + "/variations"));
    CHECK_EQ(reopened_variations.status, 200);
    CHECK_EQ(json::parse(reopened_variations.body).at("variations").as_array().size(), 1ULL);
    const auto reopened_attempts = reopened.api.handle(
        account_request("GET", "/api/games/" + game_id + "/retry-attempts"));
    CHECK_EQ(reopened_attempts.status, 200);
    CHECK_EQ(json::parse(reopened_attempts.body).at("attempts").as_array().size(), 1ULL);
}

#endif

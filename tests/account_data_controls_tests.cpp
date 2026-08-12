#include "test.hpp"

#include "pct/app/hosted_identity.hpp"
#include "pct/app/job_manager.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/import/import_service.hpp"
#include "pct/service/http_server.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace pct;

#ifdef PCT_HAS_POSTGRES

namespace {

class ControlsEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken) override {
        engine::AnalysisResult result;
        result.best_move = "e2e4";
        result.lines.push_back({1, request.depth, 20, std::nullopt, 1, 1, {"e2e4"}});
        return result;
    }
};

std::string controls_pgn() {
    return "[White \"Controls\"]\n[Black \"Owner\"]\n[Result \"1-0\"]\n\n"
           "1. e4 e5 2. Nf3 Nc6 1-0";
}

service::Request account_request(std::string path, std::string body,
                                 std::string fresh_token = {}) {
    std::map<std::string, std::string> headers{{"authorization", "Bearer account-token"}};
    if (!fresh_token.empty())
        headers.emplace("x-plywise-reauth-token", std::move(fresh_token));
    return service::Request{"POST", std::move(path), std::move(headers), std::move(body)};
}

} // namespace

TEST_CASE("hosted account export is complete, idempotent, and deletion requires fresh auth") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    app::HostedIdentityStore identity(connection);
    const std::string subject = "controls-" + std::to_string(stamp);
    const auto account = identity.ensure_account("test", subject);
    app::PostgresRepository repository(connection, account.owner());
    ControlsEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 3, 2});
    app::JobManager jobs(repository, analyzer, app::JobManagerOptions{1, 8, 1});
    import::ImportService importer;

    const auto imported = importer.from_pgn(controls_pgn());
    CHECK(repository.add(imported) == app::AddResult::Added);
    analysis::GameAnalysis review;
    review.game_id = imported.game.identity;
    review.opening = "Controls Opening";
    repository.save_analysis(review);

    std::array<unsigned char, 32> guest_token{};
    guest_token.fill(0x7a);
    const auto expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() +
                            60 * 60 * 1000;
    const std::string guest_id = "guest-controls-" + std::to_string(stamp);
    static_cast<void>(identity.create_guest_session(guest_id, guest_token, expires_at));
    static_cast<void>(identity.claim_guest(guest_id, account.id, "claim-controls-1"));

    service::AuthConfig auth;
    auth.required = true;
    auth.verify = [account_id = account.id](std::string_view token)
        -> std::optional<app::OwnerId> {
        if (token == "account-token")
            return app::OwnerId::account(account_id);
        return std::nullopt;
    };
    auth.resolve_scope = [&](const app::OwnerId& owner) -> std::optional<service::ApiScope> {
        if (owner == account.owner())
            return service::ApiScope{&repository, &jobs};
        return std::nullopt;
    };
    auth.verify_fresh = [account_id = account.id](std::string_view token)
        -> std::optional<app::OwnerId> {
        if (token == "fresh-token")
            return app::OwnerId::account(account_id);
        return std::nullopt;
    };
    auth.export_account = [&identity](const app::OwnerId& owner, std::string_view key) {
        const auto result = identity.export_account(std::string(owner.value()), std::string(key));
        return service::AccountExportResult{result.request_id, std::move(result.data),
                                            result.completed_at_ms};
    };
    auth.delete_account = [&identity](const app::OwnerId& owner, std::string_view key) {
        const auto result = identity.delete_account(std::string(owner.value()), std::string(key));
        return service::AccountDeletionResult{result.request_id, result.receipt_token,
                                               result.completed_at_ms,
                                               result.backup_retention_until_ms};
    };
    service::Api api(importer, repository, jobs, {}, {}, nullptr, {}, auth);

    const std::string export_body =
        json::dump(json::Value::Object{{"idempotency_key", "export-controls-1"}});
    const auto exported = api.handle(account_request("/api/account/export", export_body));
    CHECK_EQ(exported.status, 200);
    const json::Value export_json = json::parse(exported.body);
    CHECK_EQ(export_json.at("status").as_string(), "completed");
    CHECK(!export_json.at("request_id").as_string().empty());
    const json::Value data = export_json.at("data");
    CHECK_EQ(data.at("export_version").as_number(), 2.0);
    CHECK_EQ(data.at("account").at("id").as_string(), account.id);
    CHECK_EQ(data.at("games").as_array().size(), 1ULL);
    CHECK_EQ(data.at("analysis_runs").as_array().size(), 1ULL);
    CHECK_EQ(data.at("reviews").as_array().size(), 1ULL);
    CHECK(data.at("practice_items").is_array());
    CHECK(data.at("practice_outcomes").is_array());
    CHECK(data.at("settings").is_array());

    const auto exported_again = api.handle(account_request("/api/account/export", export_body));
    CHECK_EQ(exported_again.status, 200);
    CHECK_EQ(json::parse(exported_again.body).at("request_id").as_string(),
             export_json.at("request_id").as_string());

    const auto export_limited = api.handle(account_request(
        "/api/account/export",
        json::dump(json::Value::Object{{"idempotency_key", "export-controls-2"}})));
    CHECK_EQ(export_limited.status, 429);
    CHECK_EQ(json::parse(export_limited.body).at("code").as_string(), "quota_exceeded");

    const std::string deletion_body = json::dump(json::Value::Object{
        {"idempotency_key", "delete-controls-1"},
        {"confirm", true},
    });
    const auto missing_fresh = api.handle(account_request("/api/account/delete", deletion_body));
    CHECK_EQ(missing_fresh.status, 401);
    CHECK_EQ(json::parse(missing_fresh.body).at("code").as_string(),
             "fresh_authorization_required");

    const auto invalid_fresh =
        api.handle(account_request("/api/account/delete", deletion_body, "wrong-token"));
    CHECK_EQ(invalid_fresh.status, 401);
    CHECK_EQ(json::parse(invalid_fresh.body).at("code").as_string(),
             "fresh_authorization_invalid");

    const auto deleted =
        api.handle(account_request("/api/account/delete", deletion_body, "fresh-token"));
    CHECK_EQ(deleted.status, 200);
    const json::Value deletion_json = json::parse(deleted.body);
    CHECK_EQ(deletion_json.at("status").as_string(), "completed");
    CHECK(!deletion_json.at("receipt_token").as_string().empty());
    CHECK_EQ(deletion_json.at("backup_retention_days").as_int(), 30);
    CHECK_THROWS(repository.list());

    const auto deleted_at_ms =
        static_cast<std::int64_t>(deletion_json.at("deleted_at_ms").as_number());
    CHECK_THROWS(identity.ensure_account("test", subject, deleted_at_ms - 1));
    const auto restored = identity.ensure_account("test", subject, deleted_at_ms + 1);
    CHECK_EQ(restored.id, account.id);
    const auto deletion_replay =
        api.handle(account_request("/api/account/delete", deletion_body, "fresh-token"));
    CHECK_EQ(deletion_replay.status, 200);
    const json::Value replay_json = json::parse(deletion_replay.body);
    CHECK_EQ(replay_json.at("request_id").as_string(), deletion_json.at("request_id").as_string());
    CHECK(replay_json.at("receipt_token").is_null());
    CHECK_EQ(repository.size(), 0ULL);
}

#endif

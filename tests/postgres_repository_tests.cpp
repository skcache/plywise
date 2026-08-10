#include "test.hpp"

#include "pct/app/hosted_identity.hpp"
#include "pct/app/hosted_browser_observations.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/import/import_service.hpp"
#include "pct/service/hosted_runtime.hpp"
#include "pct/service/http_server.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace pct;

#ifdef PCT_HAS_POSTGRES

namespace {

int connect_loopback(std::uint16_t port) {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

std::string receive_available(int socket_fd) {
    std::string result;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pollfd descriptor{socket_fd, POLLIN | POLLHUP, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready <= 0)
            continue;
        char buffer[4096];
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        result.append(buffer, static_cast<std::size_t>(count));
        if (result.find("\r\n\r\n") != std::string::npos)
            break;
    }
    return result;
}

std::string receive_until_quiet(int socket_fd) {
    std::string result;
    int quiet_attempts = 0;
    for (int attempt = 0; attempt < 30 && quiet_attempts < 3; ++attempt) {
        pollfd descriptor{socket_fd, POLLIN | POLLHUP, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready <= 0) {
            ++quiet_attempts;
            continue;
        }
        char buffer[4096];
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        result.append(buffer, static_cast<std::size_t>(count));
        quiet_attempts = 0;
    }
    return result;
}

} // namespace

TEST_CASE("PostgreSQL repository keeps owner-scoped games and reviews durable") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"Postgres\"]\n[Black \"Owner\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    app::PostgresRepository first(connection, app::OwnerId::account("account_test_a"));
    CHECK(first.add(imported) == app::AddResult::Added);
    CHECK(first.add(imported) == app::AddResult::Duplicate);
    auto reformatted = importer.from_pgn(
        "[White \"Postgres\"]\n[Black \"Owner\"]\n[Result \"1-0\"]\n\n"
        "1. e4   e5 1-0");
    reformatted.source_url = "https://example.test/game/reformatted";
    CHECK_EQ(reformatted.game.identity, imported.game.identity);
    CHECK(first.add(reformatted) == app::AddResult::Duplicate);
    CHECK_EQ(first.size(), 1ULL);
    CHECK(first.get(imported.game.identity).has_value());

    first.record_job_state(imported.game.identity, "queued");
    CHECK_EQ(first.recoverable_analysis_jobs().size(), 1ULL);

    analysis::GameAnalysis analysis;
    analysis.game_id = imported.game.identity;
    analysis.opening = "Test Opening";
    analysis.accuracy = 87.5;
    first.save_analysis(analysis);
    first.record_job_state(imported.game.identity, "complete");
    CHECK(first.recoverable_analysis_jobs().empty());

    app::PostgresRepository reopened(connection, app::OwnerId::account("account_test_a"));
    const auto stored = reopened.get(imported.game.identity);
    CHECK(stored.has_value());
    CHECK(stored->analysis.has_value());
    CHECK_EQ(stored->analysis->opening, "Test Opening");

    app::PostgresRepository other(connection, app::OwnerId::account("account_test_b"));
    CHECK(!other.get(imported.game.identity).has_value());
    CHECK_EQ(other.size(), 0ULL);

    app::PostgresRepository guest(connection, app::OwnerId::guest("guest_test"));
    CHECK(guest.add(imported) == app::AddResult::Added);
    CHECK_EQ(guest.size(), 1ULL);
    guest.record_job_state(imported.game.identity, "queued");
    guest.save_analysis(analysis);
    const auto guest_stored = guest.get(imported.game.identity);
    CHECK(guest_stored.has_value());
    CHECK(guest_stored->analysis.has_value());
    CHECK(guest.recoverable_analysis_jobs().empty());

    app::PostgresRepository expired(connection, app::OwnerId::guest("guest_expired"));
    CHECK_THROWS(expired.size());
}

TEST_CASE("PostgreSQL imported identity decisions stay owner scoped and durable") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[Event \"Identity boundary\"]\n[White \"Identity A\"]\n"
        "[Black \"Identity B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    app::PostgresRepository owner_a(connection, app::OwnerId::account("account_test_a"));
    CHECK(owner_a.add(imported) == app::AddResult::Added);
    CHECK(!owner_a.player_identity().has_value());
    owner_a.save_player_identity(training::PlayerIdentity{
        std::string(training::player_identity_contract_version), imported.game.identity,
        "Identity A", "pgn", training::PlayerIdentityDecision::Confirmed, 0});
    CHECK_EQ(owner_a.player_identity()->player_name, "Identity A");

    app::PostgresRepository reopened(connection, app::OwnerId::account("account_test_a"));
    CHECK(reopened.player_identity().has_value());
    CHECK(reopened.player_identity()->decision == training::PlayerIdentityDecision::Confirmed);

    app::PostgresRepository owner_b(connection, app::OwnerId::account("account_test_b"));
    CHECK(!owner_b.player_identity().has_value());
    CHECK_THROWS(owner_b.save_player_identity(training::PlayerIdentity{
        std::string(training::player_identity_contract_version), imported.game.identity,
        "Identity A", "pgn", training::PlayerIdentityDecision::Confirmed, 0}));
}

TEST_CASE("Hosted identity creates accounts and atomically claims guest reviews") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    app::HostedIdentityStore identity(connection);
    const auto created = identity.ensure_account("test", "new-subject");
    const auto repeated = identity.ensure_account("test", "new-subject");
    CHECK_EQ(created.id, repeated.id);

    std::array<unsigned char, 32> token{};
    token.fill(0x42);
    const auto expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() +
                            60 * 60 * 1000;
    const auto session = identity.create_guest_session("guest_identity", token, expires_at);
    CHECK_EQ(session.id, "guest_identity");
    const auto owner = identity.owner_for_guest_token(token);
    CHECK(owner.has_value());
    CHECK(owner->kind() == app::OwnerKind::Guest);
    CHECK_EQ(owner->value(), "guest_identity");

    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"Guest\"]\n[Black \"Review\"]\n[Result \"1-0\"]\n\n1. d4 d5 1-0");
    analysis::GameAnalysis review;
    review.game_id = imported.game.identity;
    review.opening = "Guest Opening";
    analysis::MoveAssessment move;
    move.ply = 0;
    move.fen_before = imported.game.plies[0].fen_before;
    move.fen_after = imported.game.plies[0].fen_after;
    move.played_uci = "d2d4";
    move.best_uci = "d2d4";
    move.evaluation_before = 0;
    move.evaluation_after = 0;
    review.moves.push_back(move);
    app::PostgresRepository guest(connection, app::OwnerId::guest("guest_identity"));
    CHECK(guest.add(imported) == app::AddResult::Added);
    const auto second_imported = importer.from_pgn(
        "[White \"Guest\"]\n[Black \"Another review\"]\n[Result \"1-0\"]\n\n1. e4 c5 1-0");
    CHECK(guest.add(second_imported) == app::AddResult::Added);
    identity.reserve_guest_analysis("guest_identity", imported.game.identity);
    identity.reserve_guest_analysis("guest_identity", imported.game.identity);
    try {
        identity.reserve_guest_analysis("guest_identity", second_imported.game.identity);
        CHECK(false);
    } catch (const Error& error) {
        CHECK(error.code() == ErrorCode::QuotaExceeded);
    }
    guest.save_analysis(review);
    const auto guest_variation = guest.create_variation(imported.game.identity, 0, "before");
    const auto guest_branch = guest.extend_variation(guest_variation.id, 0, "e2e4");
    CHECK_EQ(guest_branch.current_node_id, 1ULL);
    const auto guest_attempt = guest.record_review_attempt(imported.game.identity, 0, "d2d4");
    CHECK(guest_attempt.accepted);

    const auto receipt = identity.claim_guest("guest_identity", created.id, "claim-identity-1");
    CHECK_EQ(receipt.guest_id, "guest_identity");
    CHECK_EQ(receipt.account_id, created.id);
    CHECK_EQ(receipt.transferred_games, 2ULL);
    CHECK(!receipt.already_claimed);
    CHECK(!identity.owner_for_guest_token(token).has_value());
    CHECK_THROWS(guest.size());

    app::PostgresRepository account(connection, created.owner());
    const auto stored = account.get(imported.game.identity);
    CHECK(stored.has_value());
    CHECK(stored->analysis.has_value());
    CHECK_EQ(stored->analysis->opening, "Guest Opening");
    const auto claimed_variations = account.variations(imported.game.identity);
    CHECK_EQ(claimed_variations.size(), 1ULL);
    CHECK_EQ(claimed_variations.front().root_position, "before");
    CHECK_EQ(claimed_variations.front().current_node_id, 1ULL);
    const auto claimed_attempts = account.review_attempts(imported.game.identity);
    CHECK_EQ(claimed_attempts.size(), 1ULL);
    CHECK(claimed_attempts.front().accepted);

    const auto retry = identity.claim_guest("guest_identity", created.id, "claim-identity-1");
    CHECK_EQ(retry.transferred_games, receipt.transferred_games);
    CHECK_EQ(retry.already_claimed, receipt.already_claimed);
    const auto already = identity.claim_guest("guest_identity", created.id, "claim-identity-2");
    CHECK(already.already_claimed);
    CHECK_EQ(already.transferred_games, 0ULL);
    CHECK_THROWS(identity.claim_guest("guest_identity", "account_test_b", "claim-other-1"));
}

TEST_CASE("Hosted browser observations resume durably after a store restart") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    app::HostedIdentityStore identity(connection);
    std::array<unsigned char, 32> token{};
    token.fill(0x55);
    const auto expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() +
                            60 * 60 * 1000;
    static_cast<void>(identity.create_guest_session("guest_browser_stage", token, expires_at));
    const auto owner = app::OwnerId::guest("guest_browser_stage");

    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"Browser\"]\n[Black \"Staging\"]\n[Result \"1-0\"]\n\n"
        "1. e4 e5 1-0");
    app::PostgresRepository repository(connection, owner);
    CHECK(repository.add(imported) == app::AddResult::Added);

    const analysis::BrowserObservationRunContext run{
        imported.game.identity, "browser-stage-run", "quick", imported.game.plies.size() * 2};
    const auto observation_for = [&](std::size_t ply, std::size_t sequence, bool after,
                                     std::string best_move) {
        const auto& canonical = imported.game.plies[ply];
        const std::string fen = after ? canonical.fen_after : canonical.fen_before;
        return analysis::BrowserEngineObservation{
            std::string(analysis::browser_observation_contract_version), run.analysis_run_id,
            run.game_id, ply, sequence, fen, run.profile,
            std::string(analysis::browser_engine_name),
            std::string(analysis::browser_engine_version),
            std::string(analysis::browser_engine_source),
            std::string(analysis::browser_engine_asset_hash), 10, 1'000, 100, 1,
            best_move, {analysis::BrowserObservationLine{1, 24, std::nullopt, {best_move}}}};
    };
    const std::vector<analysis::BrowserEngineObservation> observations{
        observation_for(0, 0, false, "e2e4"), observation_for(0, 1, true, "e7e5"),
        observation_for(1, 2, false, "e7e5"), observation_for(1, 3, true, "g1f3")};

    {
        app::HostedBrowserObservationStore first(connection);
        first.begin(owner, run);
        first.begin(owner, run);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto& observation = observations[index];
            const analysis::BrowserObservationContext context{
                run.game_id, run.analysis_run_id, run.profile, observation.fen};
            CHECK(first.submit(owner, context, observation).disposition ==
                  analysis::BrowserObservationDisposition::Accepted);
        }
        const auto duplicate_context = analysis::BrowserObservationContext{
            run.game_id, run.analysis_run_id, run.profile, observations[0].fen};
        CHECK(first.submit(owner, duplicate_context, observations[0]).disposition ==
              analysis::BrowserObservationDisposition::Duplicate);
    }

    {
        app::HostedBrowserObservationStore resumed(connection);
        resumed.begin(owner, run);
        for (std::size_t index = 2; index < observations.size(); ++index) {
            const auto& observation = observations[index];
            const analysis::BrowserObservationContext context{
                run.game_id, run.analysis_run_id, run.profile, observation.fen};
            CHECK(resumed.submit(owner, context, observation).disposition ==
                  analysis::BrowserObservationDisposition::Accepted);
        }
        const auto bundle = resumed.finalize(owner, run.game_id, run.analysis_run_id);
        CHECK_EQ(bundle.observations.size(), observations.size());
        CHECK(bundle.observations == observations);
        const auto retry = resumed.finalize(owner, run.game_id, run.analysis_run_id);
        CHECK(retry.observations == observations);
    }

    std::array<unsigned char, 32> other_token{};
    other_token.fill(0x56);
    static_cast<void>(identity.create_guest_session("guest_browser_other", other_token, expires_at));
    app::HostedBrowserObservationStore other(connection);
    CHECK_THROWS(other.finalize(app::OwnerId::guest("guest_browser_other"), run.game_id,
                                run.analysis_run_id));
}

TEST_CASE("PostgreSQL repository persists legal variations and review attempts") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"Variation\"]\n[Black \"Review\"]\n[Result \"1-0\"]\n\n"
        "1. d4 d5 2. c4 e6 1-0");
    app::PostgresRepository repository(connection, app::OwnerId::account("account_test_a"));
    CHECK(repository.add(imported) == app::AddResult::Added);

    analysis::GameAnalysis review;
    review.game_id = imported.game.identity;
    analysis::MoveAssessment move;
    move.ply = 0;
    move.move_number = 1;
    move.side = "white";
    move.san = "d4";
    move.played_san = "d4";
    move.played_uci = "d2d4";
    move.fen_before = imported.game.plies[0].fen_before;
    move.fen_after = imported.game.plies[0].fen_after;
    move.best_uci = "d2d4";
    move.best_san = "d4";
    move.evaluation_before = 0;
    move.evaluation_after = 0;
    move.evaluation_after_best = 0;
    move.acceptable_alternatives = {"c2c4"};
    move.classification_state = analysis::ClassificationState::Final;
    review.moves.push_back(move);
    repository.save_analysis(review);

    const auto created = repository.create_variation(imported.game.identity, 0, "before");
    CHECK_EQ(created.root_position, "before");
    CHECK_EQ(created.root_fen, imported.game.plies[0].fen_before);
    CHECK_EQ(created.current_node_id, 0ULL);
    CHECK_EQ(created.nodes.size(), 1ULL);

    const auto extended = repository.extend_variation(created.id, 0, "e2e4");
    CHECK_EQ(extended.nodes.size(), 2ULL);
    CHECK_EQ(extended.current_node_id, 1ULL);
    CHECK_EQ(extended.nodes.at(1).uci, "e2e4");
    CHECK_EQ(extended.nodes.at(0).children.size(), 1ULL);

    const auto repeated = repository.extend_variation(created.id, 0, "e2e4");
    CHECK_EQ(repeated.nodes.size(), 2ULL);
    CHECK_EQ(repeated.current_node_id, 1ULL);
    const auto reset = repository.reset_variation(created.id);
    CHECK_EQ(reset.current_node_id, 0ULL);
    const auto moved = repository.set_variation_cursor(created.id, 1);
    CHECK_EQ(moved.current_node_id, 1ULL);

    const auto accepted = repository.record_review_attempt(imported.game.identity, 0, "d2d4");
    CHECK(accepted.accepted);
    const auto rejected = repository.record_review_attempt(imported.game.identity, 0, "g1f3");
    CHECK(!rejected.accepted);
    CHECK(accepted.id != rejected.id);

    app::PostgresRepository reopened(connection, app::OwnerId::account("account_test_a"));
    const auto restored = reopened.variation(created.id);
    CHECK(restored.has_value());
    CHECK_EQ(restored->root_position, "before");
    CHECK_EQ(restored->current_node_id, 1ULL);
    CHECK_EQ(restored->nodes.size(), 2ULL);
    const auto attempts = reopened.review_attempts(imported.game.identity);
    CHECK_EQ(attempts.size(), 2ULL);
    CHECK(std::any_of(attempts.begin(), attempts.end(),
                      [](const auto& attempt) { return attempt.accepted; }));
    CHECK(std::any_of(attempts.begin(), attempts.end(),
                      [](const auto& attempt) { return !attempt.accepted; }));

    app::PostgresRepository other(connection, app::OwnerId::account("account_test_b"));
    CHECK(!other.variation(created.id).has_value());
    CHECK(other.variations(imported.game.identity).empty());
    CHECK_THROWS(other.extend_variation(created.id, 0, "e2e4"));
    CHECK(!other.delete_variation(created.id));
    CHECK(reopened.delete_variation(created.id));
    CHECK(!reopened.variation(created.id).has_value());
}

#if defined(PCT_HAS_OIDC)

class HostedRuntimeEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest&, CancellationToken) override {
        return {};
    }
};

TEST_CASE("Hosted runtime creates and reuses owner-scoped account resources") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    HostedRuntimeEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache);
    service::HostedRuntime runtime(
        service::HostedRuntimeOptions{
            connection,
            "https://project.supabase.co/auth/v1",
            "authenticated",
            "supabase",
            "https://project.supabase.co/auth/v1/.well-known/jwks.json",
        },
        analyzer,
        app::JobManagerOptions{1, 8, 0});

    const auto resolve = runtime.scope_resolver();
    CHECK(static_cast<bool>(runtime.browser_observation_begin()));
    CHECK(static_cast<bool>(runtime.browser_observation_submit()));
    CHECK(static_cast<bool>(runtime.browser_observation_finalize()));
    const auto first = resolve(app::OwnerId::account("account_test_a"));
    CHECK(first.has_value());
    CHECK(first->repository != nullptr);
    CHECK(first->jobs != nullptr);
    CHECK(first->repository->owner() == app::OwnerId::account("account_test_a"));

    const auto repeated = resolve(app::OwnerId::account("account_test_a"));
    CHECK(repeated.has_value());
    CHECK_EQ(repeated->repository, first->repository);
    CHECK_EQ(repeated->jobs, first->jobs);

    const auto create_guest = runtime.guest_session_creator();
    const auto session = create_guest();
    CHECK(session.has_value());
    CHECK_EQ(session->guest_id.starts_with("guest-"), true);
    CHECK_EQ(session->token.size(), 64ULL);

    const auto verify = runtime.token_verifier();
    const auto guest_owner = verify(session->token);
    CHECK(guest_owner.has_value());
    CHECK(guest_owner->kind() == app::OwnerKind::Guest);
    CHECK_EQ(guest_owner->value(), session->guest_id);
    const auto guest_scope = resolve(*guest_owner);
    CHECK(guest_scope.has_value());
    CHECK(guest_scope->repository != nullptr);
    CHECK(guest_scope->jobs != nullptr);
    CHECK(guest_scope->repository->owner() == *guest_owner);

    const auto claim = runtime.guest_claim_handler();
    const auto receipt = claim(session->token, app::OwnerId::account("account_test_a"),
                               "runtime-claim-1");
    CHECK_EQ(receipt.guest_id, session->guest_id);
    CHECK_EQ(receipt.account_id, "account_test_a");
    CHECK_EQ(receipt.transferred_games, 0ULL);
    CHECK(!verify(session->token).has_value());
}

TEST_CASE("Hosted runtime evicts only idle owner resources at capacity") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    HostedRuntimeEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache);
    service::HostedRuntime runtime(
        service::HostedRuntimeOptions{
            connection,
            "https://project.supabase.co/auth/v1",
            "authenticated",
            "supabase",
            "https://project.supabase.co/auth/v1/.well-known/jwks.json",
            2,
            std::chrono::minutes(10),
        },
        analyzer,
        app::JobManagerOptions{1, 8, 0});
    app::HostedIdentityStore identity(connection);
    const auto alice = identity.ensure_account("test", "resource-eviction-alice");
    const auto bob = identity.ensure_account("test", "resource-eviction-bob");
    const auto carol = identity.ensure_account("test", "resource-eviction-carol");
    const auto resolve = runtime.scope_resolver();

    const auto alice_scope = resolve(alice.owner());
    CHECK(alice_scope.has_value());
    {
        const auto bob_scope = resolve(bob.owner());
        CHECK(bob_scope.has_value());
    }

    // Alice is still pinned by an active scope, so the idle Bob resource is the only safe
    // candidate when Carol arrives at the two-owner capacity.
    const auto carol_scope = resolve(carol.owner());
    CHECK(carol_scope.has_value());

    bool rejected = false;
    try {
        static_cast<void>(resolve(bob.owner()));
    } catch (const Error& error) {
        rejected = error.code() == ErrorCode::QuotaExceeded;
    }
    CHECK(rejected);
}

TEST_CASE("Hosted WebSocket progress authenticates by subprotocol and routes by owner") {
    const char* connection = std::getenv("PCT_POSTGRES_TEST_URL");
    if (connection == nullptr || connection[0] == '\0')
        return;

    HostedRuntimeEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache);
    service::HostedRuntime runtime(
        service::HostedRuntimeOptions{
            connection,
            "https://project.supabase.co/auth/v1",
            "authenticated",
            "supabase",
            "https://project.supabase.co/auth/v1/.well-known/jwks.json",
        },
        analyzer,
        app::JobManagerOptions{1, 8, 0});
    app::HostedIdentityStore identity(connection);
    const auto alice = identity.ensure_account("test", "websocket-alice");
    const auto bob = identity.ensure_account("test", "websocket-bob");
    const auto resolve = runtime.scope_resolver();
    const auto alice_scope = resolve(alice.owner());
    const auto bob_scope = resolve(bob.owner());
    CHECK(alice_scope.has_value());
    CHECK(bob_scope.has_value());

    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"WebSocket\"]\n[Black \"Owner\"]\n[Result \"1-0\"]\n\n"
        "1. e4 e5 1-0");
    const auto added = alice_scope->repository->add(imported);
    CHECK(added == app::AddResult::Added || added == app::AddResult::Duplicate);

    service::AuthConfig auth;
    auth.required = true;
    auth.verify = [&](std::string_view token) -> std::optional<app::OwnerId> {
        if (token == "alice-token")
            return alice.owner();
        if (token == "bob-token")
            return bob.owner();
        return std::nullopt;
    };
    auth.resolve_scope = resolve;
    auth.issue_websocket_ticket = runtime.websocket_ticket_issuer();
    auth.verify_websocket_ticket = runtime.websocket_ticket_verifier();
    service::Api api(importer, *alice_scope->repository, *alice_scope->jobs, {}, {}, nullptr, {},
                     auth);
    service::HttpServer server(
        api, *alice_scope->jobs, service::ServerOptions{0, {}, "127.0.0.1", {}, {}});
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    for (int attempt = 0; attempt < 200 && server.bound_port() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::uint16_t port = server.bound_port();

    const auto alice_ticket = auth.issue_websocket_ticket(alice.owner());
    const auto bob_ticket = auth.issue_websocket_ticket(bob.owner());
    CHECK(alice_ticket.has_value());
    CHECK(bob_ticket.has_value());

    const auto connect_authenticated = [&](std::string_view ticket) {
        const int socket_fd = port == 0 ? -1 : connect_loopback(port);
        if (socket_fd < 0)
            return std::pair<int, std::string>{socket_fd, std::string{}};
        const std::string request =
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nOrigin: http://127.0.0.1:" +
            std::to_string(port) + "\r\nSec-WebSocket-Protocol: plywise-auth, " +
            std::string(ticket) +
            "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        static_cast<void>(send(socket_fd, request.data(), request.size(), 0));
        return std::pair<int, std::string>{socket_fd, receive_available(socket_fd)};
    };

    const auto alice_connection = connect_authenticated(alice_ticket->ticket);
    const auto bob_connection = connect_authenticated(bob_ticket->ticket);
    const auto replay_connection = connect_authenticated(alice_ticket->ticket);
    const int bearer_socket = port == 0 ? -1 : connect_loopback(port);
    std::string bearer_response;
    if (bearer_socket >= 0) {
        const std::string bearer_request =
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nOrigin: http://127.0.0.1:" +
            std::to_string(port) + "\r\nAuthorization: Bearer alice-token\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        static_cast<void>(send(bearer_socket, bearer_request.data(), bearer_request.size(), 0));
        bearer_response = receive_available(bearer_socket);
    }
    static_cast<void>(receive_until_quiet(alice_connection.first));
    static_cast<void>(receive_until_quiet(bob_connection.first));
    static_cast<void>(alice_scope->jobs->start(imported.game.identity));
    const std::string alice_events = receive_until_quiet(alice_connection.first);
    const std::string bob_events = receive_until_quiet(bob_connection.first);

    server.stop();
    server.stop();
    if (alice_connection.first >= 0)
        close(alice_connection.first);
    if (bob_connection.first >= 0)
        close(bob_connection.first);
    if (replay_connection.first >= 0)
        close(replay_connection.first);
    if (bearer_socket >= 0)
        close(bearer_socket);
    server_thread.join();

    CHECK(port != 0);
    CHECK(!server_error);
    CHECK(alice_connection.first >= 0);
    CHECK(bob_connection.first >= 0);
    CHECK(replay_connection.first >= 0);
    CHECK(bearer_socket >= 0);
    CHECK(alice_connection.second.starts_with("HTTP/1.1 101 Switching Protocols"));
    CHECK(alice_connection.second.find("Sec-WebSocket-Protocol: plywise-auth") !=
          std::string::npos);
    CHECK(alice_connection.second.find("alice-token") == std::string::npos);
    CHECK(bob_connection.second.starts_with("HTTP/1.1 101 Switching Protocols"));
    CHECK(replay_connection.second.starts_with("HTTP/1.1 401 Unauthorized"));
    CHECK(replay_connection.second.find("alice-token") == std::string::npos);
    CHECK(bearer_response.starts_with("HTTP/1.1 401 Unauthorized"));
    CHECK(alice_events.find("\"type\":\"job_update\"") != std::string::npos);
    CHECK(alice_events.find(imported.game.identity) != std::string::npos);
    CHECK(bob_events.find(imported.game.identity) == std::string::npos);
}

#endif

#endif

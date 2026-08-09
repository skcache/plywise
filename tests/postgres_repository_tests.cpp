#include "test.hpp"

#include "pct/app/hosted_identity.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/import/import_service.hpp"
#include "pct/service/hosted_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>

using namespace pct;

#ifdef PCT_HAS_POSTGRES

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
    guest.save_analysis(review);
    const auto guest_variation = guest.create_variation(imported.game.identity, 0, "before");
    const auto guest_branch = guest.extend_variation(guest_variation.id, 0, "e2e4");
    CHECK_EQ(guest_branch.current_node_id, 1ULL);
    const auto guest_attempt = guest.record_review_attempt(imported.game.identity, 0, "d2d4");
    CHECK(guest_attempt.accepted);

    const auto receipt = identity.claim_guest("guest_identity", created.id, "claim-identity-1");
    CHECK_EQ(receipt.guest_id, "guest_identity");
    CHECK_EQ(receipt.account_id, created.id);
    CHECK_EQ(receipt.transferred_games, 1ULL);
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

#endif

#endif

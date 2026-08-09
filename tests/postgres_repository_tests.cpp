#include "test.hpp"

#include "pct/app/hosted_identity.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/import/import_service.hpp"

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
    app::PostgresRepository guest(connection, app::OwnerId::guest("guest_identity"));
    CHECK(guest.add(imported) == app::AddResult::Added);
    guest.save_analysis(review);

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

    const auto retry = identity.claim_guest("guest_identity", created.id, "claim-identity-1");
    CHECK_EQ(retry.transferred_games, receipt.transferred_games);
    CHECK_EQ(retry.already_claimed, receipt.already_claimed);
    const auto already = identity.claim_guest("guest_identity", created.id, "claim-identity-2");
    CHECK(already.already_claimed);
    CHECK_EQ(already.transferred_games, 0ULL);
    CHECK_THROWS(identity.claim_guest("guest_identity", "account_test_b", "claim-other-1"));
}

#endif

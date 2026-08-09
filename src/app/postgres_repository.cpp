#include "pct/app/postgres_repository.hpp"

#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <libpq-fe.h>
#include <openssl/sha.h>

#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <utility>

namespace pct::app {
namespace {

struct ResultDeleter {
    void operator()(PGresult* result) const noexcept {
        if (result != nullptr)
            PQclear(result);
    }
};

using Result = std::unique_ptr<PGresult, ResultDeleter>;

Result execute(PGconn* connection, std::string_view statement,
               const std::vector<std::string>& parameters = {}) {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const auto& parameter : parameters)
        values.push_back(parameter.c_str());
    PGresult* raw = PQexecParams(connection, std::string(statement).c_str(),
                                 static_cast<int>(values.size()), nullptr, values.data(), nullptr,
                                 nullptr, 0);
    if (raw == nullptr)
        throw Error(ErrorCode::IoError, "PostgreSQL query returned no result");
    Result result(raw);
    const ExecStatusType status = PQresultStatus(result.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        const char* sql_state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
        const std::string suffix = sql_state == nullptr ? std::string{}
                                                         : " (SQLSTATE " + std::string(sql_state) + ")";
        throw Error(ErrorCode::IoError, "PostgreSQL query failed" + suffix);
    }
    return result;
}

class Transaction final {
  public:
    explicit Transaction(PGconn* connection) : connection_(connection) {
        static_cast<void>(execute(connection_, "BEGIN"));
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    ~Transaction() {
        if (!committed_) {
            try {
                static_cast<void>(execute(connection_, "ROLLBACK"));
            } catch (...) {
            }
        }
    }

    void commit() {
        static_cast<void>(execute(connection_, "COMMIT"));
        committed_ = true;
    }

  private:
    PGconn* connection_;
    bool committed_{false};
};

std::string owner_kind_name(OwnerKind kind) {
    switch (kind) {
    case OwnerKind::Guest:
        return "guest";
    case OwnerKind::Account:
        return "account";
    case OwnerKind::Local:
        throw Error(ErrorCode::InvalidArgument,
                    "PostgreSQL repositories require a guest or account owner");
    }
    throw Error(ErrorCode::InvalidArgument, "unknown PostgreSQL owner kind");
}

std::string method_name(import::ImportMethod method) {
    switch (method) {
    case import::ImportMethod::PublicApi:
        return "public_api";
    case import::ImportMethod::PublicPage:
        return "public_page";
    case import::ImportMethod::ManualPgn:
        return "manual_pgn";
    }
    return "manual_pgn";
}

import::ImportMethod parse_method(std::string_view method) {
    if (method == "public_api")
        return import::ImportMethod::PublicApi;
    if (method == "public_page")
        return import::ImportMethod::PublicPage;
    return import::ImportMethod::ManualPgn;
}

std::string sha256_hex(std::string_view value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const unsigned char byte : digest)
        encoded << std::setw(2) << static_cast<unsigned>(byte);
    return encoded.str();
}

std::string sha256_bytea(std::string_view value) {
    return "\\x" + sha256_hex(value);
}

std::string value_at(const Result& result, int row, int column) {
    if (PQgetisnull(result.get(), row, column) != 0)
        return {};
    return PQgetvalue(result.get(), row, column);
}

std::int64_t integer_at(const Result& result, int row, int column) {
    const std::string value = value_at(result, row, column);
    if (value.empty())
        return 0;
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
        throw Error(ErrorCode::Corruption, "PostgreSQL returned an invalid integer");
    return parsed;
}

void require_owner(PGconn* connection, const OwnerId& owner) {
    const Result result = execute(
        connection,
        "SELECT 1 FROM plywise.owners WHERE owner_kind = $1 AND owner_id = $2 "
        "AND (owner_kind = 'account' OR expires_at > now())",
        {owner_kind_name(owner.kind()), std::string(owner.value())});
    if (PQntuples(result.get()) != 1)
        throw Error(ErrorCode::NotFound, "repository owner does not exist");
}

void require_owned_game(PGconn* connection, const OwnerId& owner, std::string_view game_id) {
    const Result result = execute(
        connection,
        "SELECT 1 FROM plywise.game_owners WHERE game_id = $1 AND owner_kind = $2 AND owner_id = $3",
        {std::string(game_id), owner_kind_name(owner.kind()), std::string(owner.value())});
    if (PQntuples(result.get()) != 1)
        throw Error(ErrorCode::NotFound, "game does not exist");
}

std::string analysis_engine_version(const analysis::GameAnalysis& analysis) {
    for (const auto& move : analysis.moves)
        if (!move.engine_version.empty())
            return move.engine_version;
    return "stockfish-unreported";
}

StoredGame stored_game_from_row(const Result& result, int row) {
    const std::string id = value_at(result, row, 0);
    const std::string pgn = value_at(result, row, 1);
    if (id.empty() || pgn.empty())
        throw Error(ErrorCode::Corruption, "PostgreSQL returned an incomplete game");
    chess::Game game = chess::parse_pgn(pgn);
    if (game.identity != id)
        throw Error(ErrorCode::Corruption, "PostgreSQL game identity does not match its PGN");

    const json::Value metadata = json::parse(value_at(result, row, 2));
    import::ImportedGame imported{std::move(game),
                                  metadata.get("source_url", json::Value{}).is_string()
                                      ? metadata.get("source_url", json::Value{}).as_string()
                                      : std::string{},
                                  pgn,
                                  parse_method(metadata.get("method", "manual_pgn").as_string())};
    StoredGame stored{std::move(imported), std::nullopt, integer_at(result, row, 3), 0,
                      std::nullopt};
    const std::string compatibility = value_at(result, row, 4);
    const std::string review = value_at(result, row, 5);
    if (!review.empty()) {
        const analysis::GameAnalysis decoded = analysis_from_json(json::parse(review));
        if (compatibility == "shallow-v1")
            stored.shallow_analysis = decoded;
        else
            stored.analysis = decoded;
        stored.analyzed_at_ms = integer_at(result, row, 6);
    }
    return stored;
}

constexpr std::string_view game_select = R"sql(
SELECT g.id,
       g.normalized_pgn,
       g.metadata_json::text,
       (EXTRACT(EPOCH FROM go.imported_at) * 1000)::bigint,
       COALESCE(head.compatibility_key, ''),
       review.review_json::text,
       COALESCE((EXTRACT(EPOCH FROM review.created_at) * 1000)::bigint, 0)
FROM plywise.game_owners go
JOIN plywise.games g ON g.id = go.game_id
LEFT JOIN LATERAL (
    SELECT h.run_id, h.compatibility_key
    FROM plywise.analysis_heads h
    WHERE h.game_id = go.game_id
      AND h.owner_kind = go.owner_kind
      AND h.owner_id = go.owner_id
    ORDER BY CASE h.compatibility_key
                 WHEN 'review-v1' THEN 0
                 WHEN 'shallow-v1' THEN 1
                 ELSE 2
             END,
             h.updated_at DESC
    LIMIT 1
) head ON true
LEFT JOIN plywise.reviews review
    ON review.run_id = head.run_id
   AND review.game_id = go.game_id
   AND review.owner_kind = go.owner_kind
   AND review.owner_id = go.owner_id
WHERE go.owner_kind = $1 AND go.owner_id = $2
)sql";

std::vector<StoredGame> select_games(PGconn* connection, const OwnerId& owner,
                                     std::optional<std::string_view> game_id = std::nullopt) {
    require_owner(connection, owner);
    std::string statement(game_select);
    std::vector<std::string> parameters{owner_kind_name(owner.kind()), std::string(owner.value())};
    if (game_id) {
        statement += " AND g.id = $3";
        parameters.emplace_back(*game_id);
    }
    statement += " ORDER BY go.imported_at DESC, g.id";
    const Result result = execute(connection, statement, parameters);
    std::vector<StoredGame> games;
    games.reserve(static_cast<std::size_t>(PQntuples(result.get())));
    for (int row = 0; row < PQntuples(result.get()); ++row)
        games.push_back(stored_game_from_row(result, row));
    return games;
}

std::string owner_scoped_key(const OwnerId& owner, std::string_view value) {
    return owner_kind_name(owner.kind()) + ":" + std::string(owner.value()) + ":" +
           std::string(value);
}

std::string pending_run_id(const OwnerId& owner, std::string_view game_id) {
    return "job-" + sha256_hex(owner_scoped_key(owner, game_id)).substr(0, 48);
}

std::string job_status_name(std::string_view status) {
    if (status == "queued" || status == "running" || status == "complete")
        return status == "complete" ? "completed" : std::string(status);
    if (status == "cancelled" || status == "failed")
        return std::string(status);
    throw Error(ErrorCode::InvalidArgument, "unknown analysis job status");
}

std::string analysis_status_name(std::string_view job_status) {
    if (job_status == "queued")
        return "created";
    if (job_status == "running")
        return "collecting";
    return std::string(job_status);
}

[[noreturn]] void unsupported(std::string_view operation) {
    throw Error(ErrorCode::Unsupported,
                "PostgreSQL adapter does not persist " + std::string(operation) + " yet");
}

} // namespace

struct PostgresRepository::Impl {
    explicit Impl(const std::string& connection_string) {
        connection = PQconnectdb(connection_string.c_str());
        if (connection == nullptr || PQstatus(connection) != CONNECTION_OK) {
            if (connection != nullptr)
                PQfinish(connection);
            connection = nullptr;
            throw Error(ErrorCode::IoError, "failed to connect to PostgreSQL");
        }
        static_cast<void>(execute(connection, "SET search_path TO plywise, public"));
    }

    ~Impl() {
        if (connection != nullptr)
            PQfinish(connection);
    }

    PGconn* connection{nullptr};
};

PostgresRepository::PostgresRepository(std::string connection_string, OwnerId owner)
    : owner_(std::move(owner)), impl_(std::make_unique<Impl>(connection_string)) {
    static_cast<void>(owner_kind_name(owner_.kind()));
}

PostgresRepository::~PostgresRepository() = default;

AddResult PostgresRepository::add(const import::ImportedGame& imported) {
    if (imported.pgn.empty() || imported.game.identity.empty())
        throw Error(ErrorCode::InvalidArgument, "cannot persist an empty imported game");
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    const std::string owner_kind = owner_kind_name(owner_.kind());
    const std::string owner_id(owner_.value());
    const std::string source_kind = method_name(imported.method);
    const std::string source_key =
        imported.source_url.empty() ? imported.game.identity : imported.source_url;
    const Result duplicate = execute(
        impl_->connection,
        "SELECT 1 FROM plywise.game_owners WHERE owner_kind = $1 AND owner_id = $2 "
        "AND source_kind = $3 AND source_key = $4",
        {owner_kind, owner_id, source_kind, source_key});
    if (PQntuples(duplicate.get()) != 0) {
        transaction.commit();
        return AddResult::Duplicate;
    }

    const std::string metadata = json::dump(json::Value::Object{
        {"source_url", imported.source_url},
        {"method", source_kind},
    });
    // The C++ parser's identity is the canonical game contract. Hash it rather than raw PGN
    // formatting so the same game cannot create duplicate canonical rows.
    const std::string canonical_hash = sha256_bytea(imported.game.identity);
    const Result inserted = execute(
        impl_->connection,
        "INSERT INTO plywise.games (id, canonical_hash, normalized_pgn, metadata_json) "
        "VALUES ($1, $2::bytea, $3, $4::jsonb) "
        "ON CONFLICT (canonical_hash) DO NOTHING RETURNING id",
        {imported.game.identity, canonical_hash, imported.pgn, metadata});
    std::string game_id;
    if (PQntuples(inserted.get()) == 1)
        game_id = value_at(inserted, 0, 0);
    else {
        const Result existing = execute(
            impl_->connection,
            "SELECT id FROM plywise.games WHERE canonical_hash = $1::bytea",
            {canonical_hash});
        if (PQntuples(existing.get()) != 1)
            throw Error(ErrorCode::Corruption, "canonical game conflict has no stored game");
        game_id = value_at(existing, 0, 0);
    }

    const Result mapping = execute(
        impl_->connection,
        "INSERT INTO plywise.game_owners "
        "(game_id, owner_kind, owner_id, source_kind, source_key) "
        "VALUES ($1, $2, $3, $4, $5) ON CONFLICT DO NOTHING RETURNING game_id",
        {game_id, owner_kind, owner_id, source_kind, source_key});
    if (PQntuples(mapping.get()) == 0) {
        transaction.commit();
        return AddResult::Duplicate;
    }
    transaction.commit();
    return AddResult::Added;
}

BulkAddResult PostgresRepository::bulk_add(std::vector<import::ImportedGame> imported_games) {
    BulkAddResult result;
    for (const auto& imported : imported_games) {
        const AddResult added = add(imported);
        if (added == AddResult::Added) {
            ++result.added;
            result.added_game_ids.push_back(imported.game.identity);
        } else {
            ++result.duplicates;
            result.duplicate_game_ids.push_back(imported.game.identity);
        }
    }
    return result;
}

void PostgresRepository::save_analysis(const analysis::GameAnalysis& analysis) {
    save_analysis_impl(analysis, "review-v1");
}

void PostgresRepository::save_shallow_analysis(const analysis::GameAnalysis& analysis) {
    save_analysis_impl(analysis, "shallow-v1");
}

void PostgresRepository::save_analysis_impl(const analysis::GameAnalysis& analysis,
                                            std::string_view compatibility_key) {
    if (analysis.game_id.empty())
        throw Error(ErrorCode::InvalidArgument, "analysis has no game id");
    const std::string encoded = json::dump(to_json(analysis));
    const std::string digest = sha256_hex(owner_scoped_key(owner_, encoded));
    const std::string owner_kind = owner_kind_name(owner_.kind());
    const std::string owner_id(owner_.value());
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    require_owned_game(impl_->connection, owner_, analysis.game_id);

    const std::string pending_id = pending_run_id(owner_, analysis.game_id);
    const Result pending = execute(
        impl_->connection,
        "SELECT id FROM plywise.analysis_runs WHERE id = $1 AND game_id = $2 "
        "AND owner_kind = $3 AND owner_id = $4 AND status <> 'completed'",
        {pending_id, analysis.game_id, owner_kind, owner_id});
    std::string run_id;
    if (PQntuples(pending.get()) == 1) {
        run_id = value_at(pending, 0, 0);
        static_cast<void>(execute(
            impl_->connection,
            "UPDATE plywise.analysis_runs SET source = 'server', engine_build = $1, "
            "profile_version = $2, classifier_version = $3, compatibility_key = $4, "
            "status = 'completed', completed_at = now() WHERE id = $5",
            {analysis_engine_version(analysis), analysis.accuracy_version,
             analysis.accuracy_version, std::string(compatibility_key), run_id}));
    } else {
        run_id = "analysis-" + digest.substr(0, 48);
        std::string supersedes;
        const Result previous = execute(
            impl_->connection,
            "SELECT run_id FROM plywise.analysis_heads WHERE game_id = $1 AND owner_kind = $2 "
            "AND owner_id = $3 AND compatibility_key = $4",
            {analysis.game_id, owner_kind, owner_id, std::string(compatibility_key)});
        if (PQntuples(previous.get()) == 1)
            supersedes = value_at(previous, 0, 0);
        static_cast<void>(execute(
            impl_->connection,
            "INSERT INTO plywise.analysis_runs "
            "(id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build, "
            "profile_version, classifier_version, compatibility_key, status, completed_at, "
            "supersedes_id) VALUES ($1, $2, $3, $4, $5, 'server', $6, $7, $8, $9, "
            "'completed', now(), NULLIF($10, '')) "
            "ON CONFLICT (owner_kind, owner_id, idempotency_key) DO UPDATE SET "
            "status = 'completed', completed_at = now(), compatibility_key = EXCLUDED.compatibility_key "
            "RETURNING id",
            {run_id, analysis.game_id, owner_kind, owner_id, "analysis-" + digest,
             analysis_engine_version(analysis), analysis.accuracy_version,
             analysis.accuracy_version, std::string(compatibility_key), supersedes}));
    }

    static_cast<void>(execute(impl_->connection,
                              "DELETE FROM plywise.analysis_positions WHERE run_id = $1",
                              {run_id}));
    static_cast<void>(execute(impl_->connection,
                              "DELETE FROM plywise.move_assessments WHERE run_id = $1",
                              {run_id}));
    static_cast<void>(execute(impl_->connection,
                              "DELETE FROM plywise.reviews WHERE run_id = $1", {run_id}));
    for (const auto& move : analysis.moves) {
        const json::Value move_json = json::Value::Object{
            {"ply", move.ply},
            {"fen_before", move.fen_before},
            {"fen_after", move.fen_after},
            {"played_uci", move.played_uci},
            {"best_uci", move.best_uci},
            {"depth", move.depth},
            {"nodes", static_cast<double>(move.nodes)},
            {"time_ms", static_cast<double>(move.time_ms)},
        };
        const std::string move_encoded = json::dump(move_json);
        static_cast<void>(execute(
            impl_->connection,
            "INSERT INTO plywise.analysis_positions "
            "(run_id, game_id, owner_kind, owner_id, ply, sequence, canonical_fen_hash, "
            "depth, nodes, time_ms, observation_json) VALUES "
            "($1, $2, $3, $4, $5, $5, $6::bytea, $7, $8, $9, $10::jsonb)",
            {run_id, analysis.game_id, owner_kind, owner_id, std::to_string(move.ply),
             sha256_bytea(move.fen_before), std::to_string(move.depth),
             std::to_string(move.nodes), std::to_string(move.time_ms), move_encoded}));
        static_cast<void>(execute(
            impl_->connection,
            "INSERT INTO plywise.move_assessments "
            "(run_id, game_id, owner_kind, owner_id, ply, assessment_json) "
            "VALUES ($1, $2, $3, $4, $5, $6::jsonb)",
            {run_id, analysis.game_id, owner_kind, owner_id, std::to_string(move.ply),
             move_encoded}));
    }
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.reviews (run_id, game_id, owner_kind, owner_id, review_json) "
        "VALUES ($1, $2, $3, $4, $5::jsonb)",
        {run_id, analysis.game_id, owner_kind, owner_id, encoded}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_heads "
        "(game_id, owner_kind, owner_id, compatibility_key, run_id) VALUES "
        "($1, $2, $3, $4, $5) ON CONFLICT (game_id, owner_kind, owner_id, compatibility_key) "
        "DO UPDATE SET run_id = EXCLUDED.run_id, revision = plywise.analysis_heads.revision + 1, "
        "updated_at = now()",
        {analysis.game_id, owner_kind, owner_id, std::string(compatibility_key), run_id}));
    static_cast<void>(execute(
        impl_->connection,
        "UPDATE plywise.analysis_jobs SET status = 'completed', updated_at = now() "
        "WHERE run_id = $1 AND owner_kind = $2 AND owner_id = $3",
        {run_id, owner_kind, owner_id}));
    transaction.commit();
}

std::optional<StoredGame> PostgresRepository::get(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto games = select_games(impl_->connection, owner_, id);
    if (games.empty())
        return std::nullopt;
    return games.front();
}

std::vector<StoredGame> PostgresRepository::list() const {
    std::lock_guard lock(mutex_);
    return select_games(impl_->connection, owner_);
}

std::size_t PostgresRepository::size() const {
    std::lock_guard lock(mutex_);
    require_owner(impl_->connection, owner_);
    const Result result = execute(
        impl_->connection,
        "SELECT count(*) FROM plywise.game_owners WHERE owner_kind = $1 AND owner_id = $2",
        {owner_kind_name(owner_.kind()), std::string(owner_.value())});
    return static_cast<std::size_t>(integer_at(result, 0, 0));
}

void PostgresRepository::record_job_state(std::string game_id, std::string status) {
    const std::string mapped = job_status_name(status);
    const std::string run_status = analysis_status_name(mapped);
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    require_owned_game(impl_->connection, owner_, game_id);
    const std::string owner_kind = owner_kind_name(owner_.kind());
    const std::string owner_id(owner_.value());
    const std::string run_id = pending_run_id(owner_, game_id);
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_runs "
        "(id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build, "
        "profile_version, classifier_version, compatibility_key, status, completed_at) VALUES "
        "($1, $2, $3, $4, $5, 'server', 'pending', 'pending', 'pending', 'review-v1', "
        "$6, CASE WHEN $6 = 'completed' THEN now() ELSE NULL END) "
        "ON CONFLICT (id) DO UPDATE SET status = EXCLUDED.status, "
        "completed_at = CASE WHEN EXCLUDED.status = 'completed' THEN now() ELSE NULL END",
        {run_id, game_id, owner_kind, owner_id, "job-" + sha256_hex(game_id), run_status}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_jobs "
        "(id, run_id, game_id, owner_kind, owner_id, idempotency_key, status, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, now()) ON CONFLICT (run_id) DO UPDATE SET "
        "status = EXCLUDED.status, updated_at = now()",
        {run_id, run_id, game_id, owner_kind, owner_id, "job-" + sha256_hex(game_id), mapped}));
    transaction.commit();
}

std::vector<std::string> PostgresRepository::recoverable_analysis_jobs() const {
    std::lock_guard lock(mutex_);
    require_owner(impl_->connection, owner_);
    const Result result = execute(
        impl_->connection,
        "SELECT game_id FROM plywise.analysis_jobs WHERE owner_kind = $1 AND owner_id = $2 "
        "AND status IN ('queued', 'running') ORDER BY created_at",
        {owner_kind_name(owner_.kind()), std::string(owner_.value())});
    std::vector<std::string> ids;
    ids.reserve(static_cast<std::size_t>(PQntuples(result.get())));
    for (int row = 0; row < PQntuples(result.get()); ++row)
        ids.push_back(value_at(result, row, 0));
    return ids;
}

void PostgresRepository::set_background_paused(bool paused) {
    std::lock_guard lock(mutex_);
    background_paused_ = paused;
}

bool PostgresRepository::background_paused() const {
    std::lock_guard lock(mutex_);
    return background_paused_;
}

void PostgresRepository::save_chesscom_profile(ChessComProfile profile) {
    const std::string settings = json::dump(json::Value::Object{
        {"original_username", profile.original_username},
        {"selected_time_controls", [&] {
             json::Value::Array values;
             for (const auto& control : profile.selected_time_controls)
                 values.emplace_back(control);
             return json::Value(std::move(values));
         }()},
    });
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.chess_profiles "
        "(owner_kind, owner_id, provider, username, settings_json, sync_cursor, "
        "last_successful_sync_at, last_error) VALUES ($1, $2, 'chess.com', $3, $4::jsonb, $5, "
        "CASE WHEN $6 = '0' THEN NULL ELSE to_timestamp($6::double precision / 1000) END, $7) "
        "ON CONFLICT (owner_kind, owner_id, provider) DO UPDATE "
        "SET username = EXCLUDED.username, settings_json = EXCLUDED.settings_json, "
        "sync_cursor = EXCLUDED.sync_cursor, last_successful_sync_at = EXCLUDED.last_successful_sync_at, "
        "last_error = EXCLUDED.last_error",
        {owner_kind_name(owner_.kind()), std::string(owner_.value()), profile.normalized_username,
         settings, profile.archive_cursor, std::to_string(profile.last_successful_sync_ms),
         profile.last_error}));
    transaction.commit();
}

std::optional<ChessComProfile> PostgresRepository::chesscom_profile() const {
    std::lock_guard lock(mutex_);
    require_owner(impl_->connection, owner_);
    const Result result = execute(
        impl_->connection,
        "SELECT username, settings_json::text, COALESCE(sync_cursor, ''), "
        "COALESCE((EXTRACT(EPOCH FROM last_successful_sync_at) * 1000)::bigint, 0), "
        "COALESCE(last_error, '') FROM plywise.chess_profiles "
        "WHERE owner_kind = $1 AND owner_id = $2 AND provider = 'chess.com'",
        {owner_kind_name(owner_.kind()), std::string(owner_.value())});
    if (PQntuples(result.get()) == 0)
        return std::nullopt;
    const json::Value settings = json::parse(value_at(result, 0, 1));
    ChessComProfile profile;
    profile.normalized_username = value_at(result, 0, 0);
    profile.original_username = settings.get("original_username", profile.normalized_username).as_string();
    profile.archive_cursor = value_at(result, 0, 2);
    profile.last_successful_sync_ms = integer_at(result, 0, 3);
    profile.last_error = value_at(result, 0, 4);
    const json::Value empty{json::Value::Array{}};
    for (const auto& control : settings.get("selected_time_controls", empty).as_array())
        profile.selected_time_controls.push_back(control.as_string());
    return profile;
}

// The remaining projections are intentionally kept behind the same repository boundary. They
// will be added in the next hosted slices rather than silently falling back to local storage.
std::vector<training::Drill> PostgresRepository::drills(std::int64_t) const {
    unsupported("training drills");
}

std::optional<training::Drill> PostgresRepository::drill(std::string_view) const {
    unsupported("training drills");
}

bool PostgresRepository::add_validated_drill(training::Drill) {
    unsupported("training drills");
}

training::DrillAttempt PostgresRepository::record_attempt(std::string_view, std::string,
                                                          std::uint64_t, int, std::int64_t) {
    unsupported("practice attempts");
}

training::Drill PostgresRepository::advance_hint(std::string_view, std::int64_t) {
    unsupported("practice hints");
}

training::Drill PostgresRepository::begin_drill_session(std::string_view, std::int64_t) {
    unsupported("practice sessions");
}

training::Profile PostgresRepository::profile() const {
    unsupported("profiles");
}

std::vector<training::Recommendation> PostgresRepository::recommendations() {
    unsupported("recommendations");
}

void PostgresRepository::complete_resource(std::string, std::int64_t) {
    unsupported("resource completion");
}

std::filesystem::path PostgresRepository::create_snapshot() {
    unsupported("filesystem snapshots");
}

std::size_t PostgresRepository::compact_storage() {
    unsupported("filesystem compaction");
}

json::Value PostgresRepository::create_batch(std::vector<std::string>, std::size_t,
                                             std::size_t, std::size_t, std::size_t) {
    unsupported("batch projections");
}

json::Value PostgresRepository::batches() const {
    std::lock_guard lock(mutex_);
    json::Value::Array values;
    for (const auto& [_, value] : batches_)
        values.push_back(value);
    return json::Value::Object{{"batches", std::move(values)}};
}

std::size_t PostgresRepository::index_chesscom_archive_chunk(std::vector<ChessComArchiveEntry>) {
    unsupported("Chess.com archive records");
}

std::optional<ChessComArchiveEntry>
PostgresRepository::chesscom_archive_entry(std::string_view) const {
    unsupported("Chess.com archive records");
}

ChessComArchivePage
PostgresRepository::search_chesscom_archive(const ChessComArchiveSearch&) const {
    unsupported("Chess.com archive records");
}

void PostgresRepository::checkpoint_chesscom_month(ChessComMonthCheckpoint) {
    unsupported("Chess.com checkpoints");
}

std::optional<ChessComMonthCheckpoint>
PostgresRepository::chesscom_month_checkpoint(std::string_view, std::string_view) const {
    unsupported("Chess.com checkpoints");
}

std::vector<ChessComMonthCheckpoint>
PostgresRepository::chesscom_month_checkpoints(std::string_view) const {
    unsupported("Chess.com checkpoints");
}

void PostgresRepository::save_chesscom_sync_state(ChessComSyncState) {
    unsupported("Chess.com sync state");
}

ChessComSyncState PostgresRepository::chesscom_sync_state() const {
    unsupported("Chess.com sync state");
}

Variation PostgresRepository::create_variation(std::string_view, std::size_t, std::string) {
    unsupported("variations");
}

Variation PostgresRepository::extend_variation(std::string_view, std::uint64_t, std::string_view) {
    unsupported("variations");
}

Variation PostgresRepository::set_variation_cursor(std::string_view, std::uint64_t) {
    unsupported("variations");
}

Variation PostgresRepository::reset_variation(std::string_view) {
    unsupported("variations");
}

std::optional<Variation> PostgresRepository::variation(std::string_view) const {
    unsupported("variations");
}

std::vector<Variation> PostgresRepository::variations(std::string_view) const {
    unsupported("variations");
}

bool PostgresRepository::delete_variation(std::string_view) {
    unsupported("variations");
}

ReviewAttempt PostgresRepository::record_review_attempt(std::string_view, std::size_t,
                                                         std::string_view) {
    unsupported("review attempts");
}

std::vector<ReviewAttempt> PostgresRepository::review_attempts(std::string_view) const {
    unsupported("review attempts");
}

} // namespace pct::app

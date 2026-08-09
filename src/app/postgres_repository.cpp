#include "pct/app/postgres_repository.hpp"
#include "pct/app/hosted_identity.hpp"

#include "pct/chess/san.hpp"
#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <libpq-fe.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <limits>
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

std::string bytea_parameter(const std::array<unsigned char, 32>& value) {
    std::ostringstream encoded;
    encoded << "\\x" << std::hex << std::setfill('0');
    for (const unsigned char byte : value)
        encoded << std::setw(2) << static_cast<unsigned>(byte);
    return encoded.str();
}

template <std::size_t Size>
std::string byte_array_hex(const std::array<unsigned char, Size>& value) {
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const unsigned char byte : value)
        encoded << std::setw(2) << static_cast<unsigned>(byte);
    return encoded.str();
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void validate_identity_text(std::string_view label, std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum)
        throw Error(ErrorCode::InvalidArgument,
                    std::string(label) + " must be between 1 and " + std::to_string(maximum) +
                        " characters");
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte == 0 || std::iscntrl(byte) != 0)
            throw Error(ErrorCode::InvalidArgument,
                        std::string(label) + " contains a control character");
    }
}

std::string random_account_id() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Error(ErrorCode::IoError, "could not generate an account identifier");
    return "acct-" + byte_array_hex(bytes);
}

std::string random_variation_id() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Error(ErrorCode::IoError, "could not generate a variation identifier");
    return "variation-" + byte_array_hex(bytes);
}

std::string random_prefixed_id(std::string_view prefix) {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Error(ErrorCode::IoError, "could not generate a hosted data request identifier");
    return std::string(prefix) + byte_array_hex(bytes);
}

std::string random_receipt_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Error(ErrorCode::IoError, "could not generate an account deletion receipt");
    return "dr-" + byte_array_hex(bytes);
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

std::uint64_t unsigned_integer_at(const Result& result, int row, int column) {
    const std::string value = value_at(result, row, column);
    if (value.empty())
        return 0;
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
        throw Error(ErrorCode::Corruption, "PostgreSQL returned an invalid unsigned integer");
    return parsed;
}

std::optional<chess::Move> parse_uci(chess::Board& board, std::string_view uci) {
    if (uci.size() != 4 && uci.size() != 5)
        return std::nullopt;
    chess::PieceType promotion = chess::PieceType::Queen;
    if (uci.size() >= 5 && uci[4] == 'n')
        promotion = chess::PieceType::Knight;
    if (uci.size() >= 5 && uci[4] == 'b')
        promotion = chess::PieceType::Bishop;
    if (uci.size() >= 5 && uci[4] == 'r')
        promotion = chess::PieceType::Rook;
    try {
        return board.find_legal_move(chess::parse_square(uci.substr(0, 2)),
                                     chess::parse_square(uci.substr(2, 2)), promotion);
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::uint64_t random_review_attempt_id() {
    std::array<unsigned char, sizeof(std::uint64_t)> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Error(ErrorCode::IoError, "could not generate a review attempt identifier");
    std::uint64_t id = 0;
    for (const unsigned char byte : bytes)
        id = (id << 8U) | static_cast<std::uint64_t>(byte);
    id &= std::numeric_limits<std::uint64_t>::max() >> 1U;
    return id == 0 ? 1 : id;
}

void require_owner(PGconn* connection, const OwnerId& owner) {
    const Result result = execute(
        connection,
        "SELECT 1 FROM plywise.owners WHERE owner_kind = $1 AND owner_id = $2 "
        "AND (owner_kind = 'account' OR (expires_at > now() AND NOT EXISTS ("
        "SELECT 1 FROM plywise.guest_sessions WHERE owner_kind = 'guest' "
        "AND id = owner_id AND claimed_at IS NOT NULL)))",
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

std::optional<Variation> load_variation(PGconn* connection, const OwnerId& owner,
                                        std::string_view variation_id, bool lock_row = false) {
    require_owner(connection, owner);
    const std::string owner_kind = owner_kind_name(owner.kind());
    const std::string owner_id(owner.value());
    std::string header_statement =
        "SELECT game_id, root_ply, root_position, root_fen, engine_configuration, "
        "current_node_id, (EXTRACT(EPOCH FROM updated_at) * 1000)::bigint "
        "FROM plywise.variations WHERE id = $1 AND owner_kind = $2 AND owner_id = $3";
    if (lock_row)
        header_statement += " FOR UPDATE";
    const Result header = execute(connection, header_statement,
                                  {std::string(variation_id), owner_kind, owner_id});
    if (PQntuples(header.get()) == 0)
        return std::nullopt;

    Variation variation;
    variation.id = std::string(variation_id);
    variation.game_id = value_at(header, 0, 0);
    variation.root_ply = static_cast<std::size_t>(unsigned_integer_at(header, 0, 1));
    variation.root_position = value_at(header, 0, 2);
    variation.root_fen = value_at(header, 0, 3);
    variation.engine_configuration = value_at(header, 0, 4);
    variation.current_node_id = unsigned_integer_at(header, 0, 5);
    variation.updated_at_ms = integer_at(header, 0, 6);

    const Result nodes = execute(
        connection,
        "SELECT node_id, parent_node_id, COALESCE(uci, ''), COALESCE(san, ''), fen "
        "FROM plywise.variation_nodes WHERE variation_id = $1 ORDER BY node_id",
        {std::string(variation_id)});
    if (PQntuples(nodes.get()) == 0)
        throw Error(ErrorCode::Corruption, "PostgreSQL variation has no root node");

    for (int row = 0; row < PQntuples(nodes.get()); ++row) {
        VariationNode node;
        node.id = unsigned_integer_at(nodes, row, 0);
        node.parent_id = PQgetisnull(nodes.get(), row, 1) != 0
                             ? -1
                             : integer_at(nodes, row, 1);
        if (node.parent_id < 0 && node.id != 0)
            throw Error(ErrorCode::Corruption, "PostgreSQL variation has multiple roots");
        node.uci = value_at(nodes, row, 2);
        node.san = value_at(nodes, row, 3);
        node.fen = value_at(nodes, row, 4);
        static_cast<void>(chess::Board::from_fen(node.fen));
        variation.nodes.emplace(node.id, std::move(node));
    }
    if (!variation.nodes.contains(0) || !variation.nodes.contains(variation.current_node_id))
        throw Error(ErrorCode::Corruption, "PostgreSQL variation cursor is invalid");
    static_cast<void>(chess::Board::from_fen(variation.root_fen));
    for (const auto& [node_id, node] : variation.nodes) {
        if (node.parent_id < 0)
            continue;
        const auto parent = variation.nodes.find(static_cast<std::uint64_t>(node.parent_id));
        if (parent == variation.nodes.end())
            throw Error(ErrorCode::Corruption, "PostgreSQL variation parent is missing");
        parent->second.children.push_back(node_id);
    }
    if (variation.nodes.rbegin()->first == std::numeric_limits<std::uint64_t>::max())
        throw Error(ErrorCode::Corruption, "PostgreSQL variation has too many nodes");
    variation.next_node_id = variation.nodes.rbegin()->first + 1;
    return variation;
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

void PostgresRepository::save_player_identity(training::PlayerIdentity identity) {
    training::validate_player_identity(identity);
    std::lock_guard lock(mutex_);
    const auto games = select_games(impl_->connection, owner_, identity.game_id);
    if (games.empty())
        throw Error(ErrorCode::NotFound, "cannot decide identity for an unknown game");
    const std::string white = games.front().imported.game.tag("White");
    const std::string black = games.front().imported.game.tag("Black");
    if ((!identity.player_name.empty() && identity.player_name != white &&
         identity.player_name != black) ||
        (identity.decision == training::PlayerIdentityDecision::Confirmed &&
         identity.player_name != white && identity.player_name != black))
        throw Error(ErrorCode::InvalidArgument,
                    "player identity must match a name in the imported game");
    if (identity.decided_at_ms == 0)
        identity.decided_at_ms = now_ms();
    const std::string encoded = json::dump(training::to_json(identity));
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.user_settings "
        "(owner_kind, owner_id, settings_version, settings_json, updated_at) "
        "VALUES ($1, $2, 1, jsonb_build_object('player_identity', $3::jsonb), now()) "
        "ON CONFLICT (owner_kind, owner_id) DO UPDATE SET "
        "settings_version = plywise.user_settings.settings_version + 1, "
        "settings_json = jsonb_set(plywise.user_settings.settings_json, '{player_identity}', "
        "EXCLUDED.settings_json->'player_identity', true), updated_at = now()",
        {owner_kind_name(owner_.kind()), std::string(owner_.value()), encoded}));
    transaction.commit();
}

std::optional<training::PlayerIdentity> PostgresRepository::player_identity() const {
    std::lock_guard lock(mutex_);
    require_owner(impl_->connection, owner_);
    const Result result = execute(
        impl_->connection,
        "SELECT settings_json::text FROM plywise.user_settings "
        "WHERE owner_kind = $1 AND owner_id = $2",
        {owner_kind_name(owner_.kind()), std::string(owner_.value())});
    if (PQntuples(result.get()) == 0)
        return std::nullopt;
    const json::Value settings = json::parse(value_at(result, 0, 0));
    const json::Value identity = settings.get("player_identity", json::Value{});
    if (identity.is_null())
        return std::nullopt;
    return training::player_identity_from_json(identity);
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

Variation PostgresRepository::create_variation(std::string_view game_id, std::size_t root_ply,
                                               std::string root_position) {
    if (root_position != "before" && root_position != "after")
        throw Error(ErrorCode::InvalidArgument,
                    "variation root position must be before or after");
    if (root_ply > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error(ErrorCode::InvalidArgument, "variation root ply is too large");

    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    const auto games = select_games(impl_->connection, owner_, game_id);
    if (games.empty())
        throw Error(ErrorCode::NotFound, "game does not exist");
    const auto& plies = games.front().imported.game.plies;
    if (root_ply >= plies.size())
        throw Error(ErrorCode::InvalidArgument, "variation root ply is outside the game");
    const std::string root_fen = root_position == "before" ? plies[root_ply].fen_before
                                                            : plies[root_ply].fen_after;
    const std::string variation_id = random_variation_id();
    const std::string owner_kind = owner_kind_name(owner_.kind());
    const std::string owner_id(owner_.value());
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.variations "
        "(id, game_id, owner_kind, owner_id, root_ply, root_position, root_fen, "
        "engine_configuration, current_node_id) VALUES "
        "($1, $2, $3, $4, $5, $6, $7, 'current-default', 0)",
        {variation_id, std::string(game_id), owner_kind, owner_id, std::to_string(root_ply),
         root_position, root_fen}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.variation_nodes "
        "(variation_id, node_id, parent_node_id, uci, san, fen) "
        "VALUES ($1, 0, NULL, NULL, NULL, $2)",
        {variation_id, root_fen}));
    transaction.commit();

    const auto created = load_variation(impl_->connection, owner_, variation_id);
    if (!created)
        throw Error(ErrorCode::Corruption, "created variation could not be loaded");
    return *created;
}

Variation PostgresRepository::extend_variation(std::string_view variation_id,
                                               std::uint64_t node_id,
                                               std::string_view move_uci) {
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    const auto current = load_variation(impl_->connection, owner_, variation_id, true);
    if (!current)
        throw Error(ErrorCode::NotFound, "variation does not exist");
    const auto parent = current->nodes.find(node_id);
    if (parent == current->nodes.end())
        throw Error(ErrorCode::NotFound, "variation node does not exist");

    chess::Board board = chess::Board::from_fen(parent->second.fen);
    const auto move = parse_uci(board, move_uci);
    if (!move)
        throw Error(ErrorCode::IllegalMove, "move is not legal in the variation position");
    const std::string canonical_uci = chess::uci(*move);
    std::uint64_t child_id = 0;
    for (const std::uint64_t candidate_id : parent->second.children) {
        const auto candidate = current->nodes.find(candidate_id);
        if (candidate != current->nodes.end() && candidate->second.uci == canonical_uci) {
            child_id = candidate_id;
            break;
        }
    }
    if (child_id == 0) {
        if (current->next_node_id == std::numeric_limits<std::uint64_t>::max())
            throw Error(ErrorCode::InvalidArgument, "variation has reached its node limit");
        child_id = current->next_node_id;
        const std::string san = chess::to_san(board, *move);
        board.make_move(*move);
        static_cast<void>(execute(
            impl_->connection,
            "INSERT INTO plywise.variation_nodes "
            "(variation_id, node_id, parent_node_id, uci, san, fen) "
            "VALUES ($1, $2, $3, $4, $5, $6)",
            {std::string(variation_id), std::to_string(child_id), std::to_string(node_id),
             canonical_uci, san, board.to_fen()}));
    }
    static_cast<void>(execute(
        impl_->connection,
        "UPDATE plywise.variations SET current_node_id = $1, updated_at = now() "
        "WHERE id = $2 AND owner_kind = $3 AND owner_id = $4",
        {std::to_string(child_id), std::string(variation_id), owner_kind_name(owner_.kind()),
         std::string(owner_.value())}));
    transaction.commit();

    const auto updated = load_variation(impl_->connection, owner_, variation_id);
    if (!updated)
        throw Error(ErrorCode::Corruption, "updated variation could not be loaded");
    return *updated;
}

Variation PostgresRepository::set_variation_cursor(std::string_view variation_id,
                                                   std::uint64_t node_id) {
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    const auto current = load_variation(impl_->connection, owner_, variation_id, true);
    if (!current)
        throw Error(ErrorCode::NotFound, "variation does not exist");
    if (!current->nodes.contains(node_id))
        throw Error(ErrorCode::NotFound, "variation node does not exist");
    static_cast<void>(execute(
        impl_->connection,
        "UPDATE plywise.variations SET current_node_id = $1, updated_at = now() "
        "WHERE id = $2 AND owner_kind = $3 AND owner_id = $4",
        {std::to_string(node_id), std::string(variation_id), owner_kind_name(owner_.kind()),
         std::string(owner_.value())}));
    transaction.commit();

    const auto updated = load_variation(impl_->connection, owner_, variation_id);
    if (!updated)
        throw Error(ErrorCode::Corruption, "updated variation could not be loaded");
    return *updated;
}

Variation PostgresRepository::reset_variation(std::string_view variation_id) {
    return set_variation_cursor(variation_id, 0);
}

std::optional<Variation> PostgresRepository::variation(std::string_view variation_id) const {
    std::lock_guard lock(mutex_);
    return load_variation(impl_->connection, owner_, variation_id);
}

std::vector<Variation> PostgresRepository::variations(std::string_view game_id) const {
    std::lock_guard lock(mutex_);
    require_owner(impl_->connection, owner_);
    const Result ids = execute(
        impl_->connection,
        "SELECT id FROM plywise.variations WHERE game_id = $1 AND owner_kind = $2 "
        "AND owner_id = $3 ORDER BY updated_at DESC, id",
        {std::string(game_id), owner_kind_name(owner_.kind()), std::string(owner_.value())});
    std::vector<std::string> variation_ids;
    variation_ids.reserve(static_cast<std::size_t>(PQntuples(ids.get())));
    for (int row = 0; row < PQntuples(ids.get()); ++row)
        variation_ids.push_back(value_at(ids, row, 0));

    std::vector<Variation> result;
    result.reserve(variation_ids.size());
    for (const auto& id : variation_ids) {
        const auto loaded = load_variation(impl_->connection, owner_, id);
        if (loaded)
            result.push_back(*loaded);
    }
    return result;
}

bool PostgresRepository::delete_variation(std::string_view variation_id) {
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    const Result deleted = execute(
        impl_->connection,
        "DELETE FROM plywise.variations WHERE id = $1 AND owner_kind = $2 AND owner_id = $3 "
        "RETURNING id",
        {std::string(variation_id), owner_kind_name(owner_.kind()), std::string(owner_.value())});
    transaction.commit();
    return PQntuples(deleted.get()) == 1;
}

ReviewAttempt PostgresRepository::record_review_attempt(std::string_view game_id,
                                                        std::size_t ply,
                                                        std::string_view uci) {
    if (ply > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error(ErrorCode::InvalidArgument, "review ply is too large");
    std::lock_guard lock(mutex_);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner_);
    const auto games = select_games(impl_->connection, owner_, game_id);
    if (games.empty())
        throw Error(ErrorCode::NotFound, "game does not exist");
    const auto& stored = games.front();
    if (!stored.analysis || ply >= stored.analysis->moves.size() ||
        ply >= stored.imported.game.plies.size())
        throw Error(ErrorCode::InvalidArgument, "review attempt requires an analyzed move");
    chess::Board board = chess::Board::from_fen(stored.imported.game.plies[ply].fen_before);
    const auto move = parse_uci(board, uci);
    if (!move)
        throw Error(ErrorCode::IllegalMove, "move is not legal in the retry position");
    const std::string canonical = chess::uci(*move);
    const auto& assessment = stored.analysis->moves[ply];
    const bool accepted = canonical == assessment.best_uci ||
                          std::find(assessment.acceptable_alternatives.begin(),
                                    assessment.acceptable_alternatives.end(), canonical) !=
                              assessment.acceptable_alternatives.end();

    const Result head = execute(
        impl_->connection,
        "SELECT run_id FROM plywise.analysis_heads WHERE game_id = $1 AND owner_kind = $2 "
        "AND owner_id = $3 AND compatibility_key = 'review-v1'",
        {std::string(game_id), owner_kind_name(owner_.kind()), std::string(owner_.value())});
    if (PQntuples(head.get()) != 1)
        throw Error(ErrorCode::Corruption, "completed review has no analysis head");
    const std::string run_id = value_at(head, 0, 0);
    ReviewAttempt attempt;
    attempt.game_id = std::string(game_id);
    attempt.ply = ply;
    attempt.uci = canonical;
    attempt.accepted = accepted;
    attempt.attempted_at_ms = now_ms();
    for (int retry = 0; retry < 4; ++retry) {
        attempt.id = random_review_attempt_id();
        const Result inserted = execute(
            impl_->connection,
            "INSERT INTO plywise.review_attempts "
            "(id, game_id, run_id, owner_kind, owner_id, ply, uci, accepted, attempted_at) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, "
            "to_timestamp($9::double precision / 1000)) "
            "ON CONFLICT (id) DO NOTHING RETURNING id",
            {std::to_string(attempt.id), std::string(game_id), run_id,
             owner_kind_name(owner_.kind()), std::string(owner_.value()),
             std::to_string(ply), canonical, accepted ? "true" : "false",
             std::to_string(attempt.attempted_at_ms)});
        if (PQntuples(inserted.get()) == 1) {
            transaction.commit();
            return attempt;
        }
    }
    throw Error(ErrorCode::IoError, "could not allocate a review attempt identifier");
}

std::vector<ReviewAttempt> PostgresRepository::review_attempts(std::string_view game_id) const {
    std::lock_guard lock(mutex_);
    require_owner(impl_->connection, owner_);
    const Result result = execute(
        impl_->connection,
        "SELECT CASE WHEN id ~ '^[0-9]+$' AND id::numeric <= 9223372036854775807 "
        "THEN id::bigint ELSE ((('x' || substr(md5(id), 1, 15))::bit(60))::bigint + 1) END, "
        "ply, uci, accepted, (EXTRACT(EPOCH FROM attempted_at) * 1000)::bigint "
        "FROM plywise.review_attempts WHERE game_id = $1 AND owner_kind = $2 AND owner_id = $3 "
        "ORDER BY attempted_at, id",
        {std::string(game_id), owner_kind_name(owner_.kind()), std::string(owner_.value())});
    std::vector<ReviewAttempt> attempts;
    attempts.reserve(static_cast<std::size_t>(PQntuples(result.get())));
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        ReviewAttempt attempt;
        attempt.id = unsigned_integer_at(result, row, 0);
        attempt.game_id = std::string(game_id);
        attempt.ply = static_cast<std::size_t>(unsigned_integer_at(result, row, 1));
        attempt.uci = value_at(result, row, 2);
        attempt.accepted = value_at(result, row, 3) == "t";
        attempt.attempted_at_ms = integer_at(result, row, 4);
        attempts.push_back(std::move(attempt));
    }
    return attempts;
}

struct HostedIdentityStore::Impl {
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
    std::mutex mutex;
};

HostedIdentityStore::HostedIdentityStore(std::string connection_string)
    : impl_(std::make_unique<Impl>(connection_string)) {}

HostedIdentityStore::~HostedIdentityStore() = default;

HostedAccount HostedIdentityStore::ensure_account(std::string auth_provider,
                                                  std::string auth_subject) {
    validate_identity_text("authentication provider", auth_provider, 128);
    validate_identity_text("authentication subject", auth_subject, 512);
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    const Result existing = execute(
        impl_->connection,
        "SELECT id FROM plywise.accounts WHERE auth_provider = $1 AND auth_subject = $2 FOR UPDATE",
        {auth_provider, auth_subject});
    if (PQntuples(existing.get()) == 1) {
        const std::string id = value_at(existing, 0, 0);
        transaction.commit();
        return HostedAccount{id};
    }

    // Expired receipts no longer need to be trackable and must not block a fresh account shell.
    static_cast<void>(execute(impl_->connection,
                              "DELETE FROM plywise.account_deletion_receipts "
                              "WHERE expires_at <= now()"));
    // A deleted account may sign in again. Reuse its opaque owner id while a deletion receipt is
    // retained so replaying the same delete key can return the original receipt instead of
    // creating a second identity. Only a hash of the provider subject is kept in the receipt table.
    const std::string subject_hash = sha256_bytea(auth_subject);
    const Result tombstone = execute(
        impl_->connection,
        "SELECT account_id FROM plywise.account_deletion_receipts "
        "WHERE auth_provider = $1 AND auth_subject_hash = $2::bytea "
        "AND expires_at > now() ORDER BY completed_at DESC LIMIT 1",
        {auth_provider, subject_hash});
    if (PQntuples(tombstone.get()) == 1) {
        const std::string id = value_at(tombstone, 0, 0);
        if (id.empty())
            throw Error(ErrorCode::Corruption, "account deletion receipt has no account id");
        const Result owner = execute(
            impl_->connection,
            "INSERT INTO plywise.owners (owner_kind, owner_id) VALUES ('account', $1) "
            "ON CONFLICT DO NOTHING RETURNING owner_id",
            {id});
        if (PQntuples(owner.get()) == 1) {
            const Result inserted = execute(
                impl_->connection,
                "INSERT INTO plywise.accounts (id, auth_provider, auth_subject) "
                "VALUES ($1, $2, $3) ON CONFLICT (auth_provider, auth_subject) DO NOTHING "
                "RETURNING id",
                {id, auth_provider, auth_subject});
            if (PQntuples(inserted.get()) == 1) {
                transaction.commit();
                return HostedAccount{id};
            }
        }
        const Result raced = execute(
            impl_->connection,
            "SELECT id FROM plywise.accounts WHERE auth_provider = $1 AND auth_subject = $2",
            {auth_provider, auth_subject});
        if (PQntuples(raced.get()) == 1) {
            const std::string raced_id = value_at(raced, 0, 0);
            transaction.commit();
            return HostedAccount{raced_id};
        }
        throw Error(ErrorCode::Corruption, "account deletion tombstone could not be restored");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::string id = random_account_id();
        const Result owner = execute(
            impl_->connection,
            "INSERT INTO plywise.owners (owner_kind, owner_id) VALUES ('account', $1) "
            "ON CONFLICT DO NOTHING RETURNING owner_id",
            {id});
        if (PQntuples(owner.get()) == 0)
            continue;
        const Result inserted = execute(
            impl_->connection,
            "INSERT INTO plywise.accounts (id, auth_provider, auth_subject) VALUES ($1, $2, $3) "
            "ON CONFLICT (auth_provider, auth_subject) DO NOTHING RETURNING id",
            {id, auth_provider, auth_subject});
        if (PQntuples(inserted.get()) == 1) {
            transaction.commit();
            return HostedAccount{id};
        }

        const Result raced = execute(
            impl_->connection,
            "SELECT id FROM plywise.accounts WHERE auth_provider = $1 AND auth_subject = $2",
            {auth_provider, auth_subject});
        if (PQntuples(raced.get()) == 1) {
            const std::string raced_id = value_at(raced, 0, 0);
            transaction.commit();
            return HostedAccount{raced_id};
        }
    }
    throw Error(ErrorCode::IoError, "could not create a hosted account");
}

GuestSession HostedIdentityStore::create_guest_session(
    std::string guest_id, const std::array<unsigned char, 32>& token_hash,
    std::int64_t expires_at_ms) {
    validate_identity_text("guest id", guest_id, 256);
    const std::int64_t current = now_ms();
    constexpr std::int64_t max_guest_lifetime_ms = 30LL * 24 * 60 * 60 * 1000;
    if (expires_at_ms <= current || expires_at_ms > current + max_guest_lifetime_ms)
        throw Error(ErrorCode::InvalidArgument, "guest session expiry is outside the allowed window");

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    const std::string token = bytea_parameter(token_hash);
    const Result existing = execute(
        impl_->connection,
        "SELECT encode(token_hash, 'hex'), "
        "(EXTRACT(EPOCH FROM expires_at) * 1000)::bigint, "
        "COALESCE(claimed_by_account_id, '') "
        "FROM plywise.guest_sessions WHERE id = $1 FOR UPDATE",
        {guest_id});
    if (PQntuples(existing.get()) == 1) {
        if (value_at(existing, 0, 0) != token.substr(2) ||
            !value_at(existing, 0, 2).empty() || integer_at(existing, 0, 1) <= current)
            throw Error(ErrorCode::InvalidArgument, "guest session id is already in use");
        const GuestSession session{guest_id, integer_at(existing, 0, 1)};
        transaction.commit();
        return session;
    }

    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.owners (owner_kind, owner_id, expires_at) "
        "VALUES ('guest', $1, to_timestamp($2::double precision / 1000)) "
        "ON CONFLICT DO NOTHING",
        {guest_id, std::to_string(expires_at_ms)}));
    const Result inserted = execute(
        impl_->connection,
        "INSERT INTO plywise.guest_sessions (id, token_hash, expires_at) "
        "VALUES ($1, $2::bytea, to_timestamp($3::double precision / 1000)) "
        "ON CONFLICT (id) DO NOTHING RETURNING id",
        {guest_id, token, std::to_string(expires_at_ms)});
    if (PQntuples(inserted.get()) == 0) {
        const Result token_conflict = execute(
            impl_->connection,
            "SELECT id FROM plywise.guest_sessions WHERE token_hash = $1::bytea",
            {token});
        if (PQntuples(token_conflict.get()) != 1)
            throw Error(ErrorCode::InvalidArgument, "guest session could not be created");
        throw Error(ErrorCode::InvalidArgument, "guest token is already in use");
    }
    transaction.commit();
    return GuestSession{guest_id, expires_at_ms};
}

std::optional<OwnerId> HostedIdentityStore::owner_for_guest_token(
    const std::array<unsigned char, 32>& token_hash) const {
    std::lock_guard lock(impl_->mutex);
    const Result result = execute(
        impl_->connection,
        "SELECT id FROM plywise.guest_sessions WHERE token_hash = $1::bytea "
        "AND expires_at > now() AND claimed_at IS NULL",
        {bytea_parameter(token_hash)});
    if (PQntuples(result.get()) == 0)
        return std::nullopt;
    return OwnerId::guest(value_at(result, 0, 0));
}

GuestClaimReceipt claim_receipt_from_json(const json::Value& value) {
    return GuestClaimReceipt{value.at("guest_id").as_string(),
                             value.at("account_id").as_string(),
                             static_cast<std::size_t>(value.at("transferred_games").as_number()),
                             value.at("already_claimed").as_bool()};
}

GuestClaimReceipt HostedIdentityStore::claim_guest(std::string guest_id,
                                                   std::string account_id,
                                                   std::string idempotency_key) {
    validate_identity_text("guest id", guest_id, 256);
    validate_identity_text("account id", account_id, 256);
    validate_identity_text("guest claim idempotency key", idempotency_key, 256);
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    const Result account = execute(
        impl_->connection,
        "SELECT 1 FROM plywise.accounts WHERE id = $1 FOR UPDATE",
        {account_id});
    if (PQntuples(account.get()) != 1)
        throw Error(ErrorCode::NotFound, "account does not exist");

    const std::string request_hash = sha256_bytea(guest_id + ":" + account_id);
    const Result prior = execute(
        impl_->connection,
        "SELECT encode(request_hash, 'hex'), response_json::text FROM plywise.idempotency_records "
        "WHERE owner_kind = 'account' AND owner_id = $1 AND operation = 'guest_claim' "
        "AND idempotency_key = $2 AND expires_at > now() FOR UPDATE",
        {account_id, idempotency_key});
    if (PQntuples(prior.get()) == 1) {
        if (value_at(prior, 0, 0) != request_hash.substr(2))
            throw Error(ErrorCode::InvalidArgument, "guest claim idempotency key was reused");
        const GuestClaimReceipt receipt = claim_receipt_from_json(json::parse(value_at(prior, 0, 1)));
        transaction.commit();
        return receipt;
    }

    const auto persist_receipt = [&](const GuestClaimReceipt& receipt) {
        const std::string response = json::dump(json::Value::Object{
            {"guest_id", receipt.guest_id},
            {"account_id", receipt.account_id},
            {"transferred_games", receipt.transferred_games},
            {"already_claimed", receipt.already_claimed},
        });
        static_cast<void>(execute(
            impl_->connection,
            "INSERT INTO plywise.idempotency_records "
            "(owner_kind, owner_id, operation, idempotency_key, request_hash, resource_kind, "
            "resource_id, response_json, expires_at) VALUES "
            "('account', $1, 'guest_claim', $2, $3::bytea, 'guest_claim', $4, $5::jsonb, "
            "now() + interval '30 days') ON CONFLICT DO NOTHING",
            {account_id, idempotency_key, request_hash, guest_id, response}));
    };

    const Result session = execute(
        impl_->connection,
        "SELECT COALESCE(claimed_by_account_id, ''), (expires_at > now()) "
        "FROM plywise.guest_sessions WHERE id = $1 AND owner_kind = 'guest' FOR UPDATE",
        {guest_id});
    if (PQntuples(session.get()) != 1)
        throw Error(ErrorCode::NotFound, "guest session does not exist");
    const std::string claimed_by = value_at(session, 0, 0);
    if (!claimed_by.empty()) {
        if (claimed_by != account_id)
            throw Error(ErrorCode::NotFound, "guest session does not exist");
        const GuestClaimReceipt receipt{guest_id, account_id, 0, true};
        persist_receipt(receipt);
        transaction.commit();
        return receipt;
    }
    if (value_at(session, 0, 1) != "t")
        throw Error(ErrorCode::NotFound, "guest session does not exist");

    const Result games = execute(
        impl_->connection,
        "INSERT INTO plywise.game_owners "
        "(game_id, owner_kind, owner_id, source_kind, source_key, provenance_json) "
        "SELECT game_id, 'account', $2, source_kind, source_key, provenance_json "
        "FROM plywise.game_owners WHERE owner_kind = 'guest' AND owner_id = $1 "
        "ON CONFLICT DO NOTHING RETURNING game_id",
        {guest_id, account_id});
    const std::size_t transferred_games = static_cast<std::size_t>(PQntuples(games.get()));

    const GuestClaimReceipt receipt{guest_id, account_id, transferred_games, false};
    persist_receipt(receipt);
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_runs "
        "(id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build, engine_hash, "
        "profile_version, classifier_version, compatibility_key, status, created_at, completed_at, "
        "supersedes_id) "
        "SELECT 'claim-' || md5($2 || ':' || id), game_id, 'account', $2, idempotency_key, source, engine_build, engine_hash, "
        "profile_version, classifier_version, compatibility_key, status, created_at, completed_at, "
        "CASE WHEN supersedes_id IS NULL THEN NULL ELSE 'claim-' || md5($2 || ':' || supersedes_id) END "
        "FROM plywise.analysis_runs "
        "WHERE owner_kind = 'guest' AND owner_id = $1 ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_positions "
        "(run_id, game_id, owner_kind, owner_id, ply, sequence, canonical_fen_hash, depth, nodes, "
        "time_ms, observation_json, validated_at) "
        "SELECT 'claim-' || md5($2 || ':' || p.run_id), p.game_id, 'account', $2, p.ply, p.sequence, p.canonical_fen_hash, "
        "p.depth, p.nodes, p.time_ms, p.observation_json, p.validated_at "
        "FROM plywise.analysis_positions p WHERE p.owner_kind = 'guest' AND p.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || p.run_id) "
        "AND r.game_id = p.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.move_assessments "
        "(run_id, game_id, owner_kind, owner_id, ply, assessment_json) "
        "SELECT 'claim-' || md5($2 || ':' || p.run_id), p.game_id, 'account', $2, p.ply, p.assessment_json "
        "FROM plywise.move_assessments p WHERE p.owner_kind = 'guest' AND p.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || p.run_id) "
        "AND r.game_id = p.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.reviews (run_id, game_id, owner_kind, owner_id, review_json, created_at) "
        "SELECT 'claim-' || md5($2 || ':' || p.run_id), p.game_id, 'account', $2, p.review_json, p.created_at "
        "FROM plywise.reviews p WHERE p.owner_kind = 'guest' AND p.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || p.run_id) "
        "AND r.game_id = p.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_heads "
        "(game_id, owner_kind, owner_id, compatibility_key, run_id, revision, updated_at) "
        "SELECT h.game_id, 'account', $2, h.compatibility_key, 'claim-' || md5($2 || ':' || h.run_id), h.revision, h.updated_at "
        "FROM plywise.analysis_heads h WHERE h.owner_kind = 'guest' AND h.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || h.run_id) "
        "AND r.game_id = h.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));

    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.analysis_jobs "
        "(id, run_id, game_id, owner_kind, owner_id, idempotency_key, priority, status, attempt, "
        "lease_owner, lease_expires_at, cancel_requested_at, created_at, updated_at) "
        "SELECT 'claim-job-' || md5($2 || ':' || j.id), 'claim-' || md5($2 || ':' || j.run_id), j.game_id, 'account', $2, j.idempotency_key, j.priority, j.status, "
        "j.attempt, j.lease_owner, j.lease_expires_at, j.cancel_requested_at, j.created_at, j.updated_at "
        "FROM plywise.analysis_jobs j WHERE j.owner_kind = 'guest' AND j.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || j.run_id) "
        "AND r.game_id = j.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.job_events "
        "(job_id, run_id, owner_kind, owner_id, sequence, stage, completed_units, total_units, created_at) "
        "SELECT 'claim-job-' || md5($2 || ':' || e.job_id), 'claim-' || md5($2 || ':' || e.run_id), 'account', $2, e.sequence, e.stage, e.completed_units, "
        "e.total_units, e.created_at FROM plywise.job_events e "
        "WHERE e.owner_kind = 'guest' AND e.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_jobs j WHERE j.id = 'claim-job-' || md5($2 || ':' || e.job_id) "
        "AND j.run_id = 'claim-' || md5($2 || ':' || e.run_id) AND j.owner_kind = 'account' AND j.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));

    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.chess_profiles "
        "(owner_kind, owner_id, provider, username, settings_json, sync_cursor, "
        "last_successful_sync_at, last_error) "
        "SELECT 'account', $2, provider, username, settings_json, sync_cursor, "
        "last_successful_sync_at, last_error FROM plywise.chess_profiles "
        "WHERE owner_kind = 'guest' AND owner_id = $1 ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.variations "
        "(id, game_id, owner_kind, owner_id, root_ply, root_position, root_fen, "
        "engine_configuration, current_node_id, created_at, updated_at) "
        "SELECT 'claim-var-' || md5($2 || ':' || id), game_id, 'account', $2, root_ply, "
        "root_position, root_fen, engine_configuration, current_node_id, created_at, updated_at "
        "FROM plywise.variations WHERE owner_kind = 'guest' AND owner_id = $1 "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.variation_nodes "
        "(variation_id, node_id, parent_node_id, uci, san, fen) "
        "SELECT 'claim-var-' || md5($2 || ':' || n.variation_id), n.node_id, n.parent_node_id, n.uci, n.san, n.fen "
        "FROM plywise.variation_nodes n JOIN plywise.variations guest_variation "
        "ON guest_variation.id = n.variation_id AND guest_variation.owner_kind = 'guest' "
        "AND guest_variation.owner_id = $1 "
        "WHERE EXISTS (SELECT 1 FROM plywise.variations account_variation "
        "WHERE account_variation.id = 'claim-var-' || md5($2 || ':' || n.variation_id) AND account_variation.owner_kind = 'account' "
        "AND account_variation.owner_id = $2) ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.review_attempts "
        "(id, game_id, run_id, owner_kind, owner_id, ply, uci, accepted, attempted_at) "
        "SELECT (((('x' || substr(md5($2 || ':' || a.id), 1, 15))::bit(60))::bigint + 1)::text), "
        "a.game_id, 'claim-' || md5($2 || ':' || a.run_id), 'account', $2, a.ply, a.uci, a.accepted, a.attempted_at "
        "FROM plywise.review_attempts a WHERE a.owner_kind = 'guest' AND a.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || a.run_id) "
        "AND r.game_id = a.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));

    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.intelligence_evidence "
        "(id, owner_kind, owner_id, run_id, game_id, ply, evidence_kind, model_version, evidence_json, created_at) "
        "SELECT 'claim-evidence-' || md5($2 || ':' || e.id), 'account', $2, 'claim-' || md5($2 || ':' || e.run_id), e.game_id, e.ply, e.evidence_kind, e.model_version, "
        "e.evidence_json, e.created_at FROM plywise.intelligence_evidence e "
        "WHERE e.owner_kind = 'guest' AND e.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.analysis_runs r WHERE r.id = 'claim-' || md5($2 || ':' || e.run_id) "
        "AND r.game_id = e.game_id AND r.owner_kind = 'account' AND r.owner_id = $2) "
        "ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.practice_items "
        "(id, owner_kind, owner_id, evidence_id, state, due_at, schedule_version, created_at, updated_at) "
        "SELECT 'claim-practice-' || md5($2 || ':' || p.id), 'account', $2, 'claim-evidence-' || md5($2 || ':' || p.evidence_id), p.state, p.due_at, p.schedule_version, "
        "p.created_at, p.updated_at FROM plywise.practice_items p "
        "WHERE p.owner_kind = 'guest' AND p.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.intelligence_evidence e WHERE e.id = 'claim-evidence-' || md5($2 || ':' || p.evidence_id) "
        "AND e.owner_kind = 'account' AND e.owner_id = $2) ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.practice_outcomes "
        "(id, practice_item_id, owner_kind, owner_id, result, response_time_ms, hint_level, attempted_at) "
        "SELECT 'claim-outcome-' || md5($2 || ':' || o.id), 'claim-practice-' || md5($2 || ':' || o.practice_item_id), 'account', $2, o.result, o.response_time_ms, o.hint_level, o.attempted_at "
        "FROM plywise.practice_outcomes o WHERE o.owner_kind = 'guest' AND o.owner_id = $1 "
        "AND EXISTS (SELECT 1 FROM plywise.practice_items p WHERE p.id = 'claim-practice-' || md5($2 || ':' || o.practice_item_id) "
        "AND p.owner_kind = 'account' AND p.owner_id = $2) ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.user_settings "
        "(owner_kind, owner_id, settings_version, settings_json, updated_at) "
        "SELECT 'account', $2, settings_version, settings_json, updated_at "
        "FROM plywise.user_settings WHERE owner_kind = 'guest' AND owner_id = $1 "
        "ON CONFLICT (owner_kind, owner_id) DO UPDATE SET "
        "settings_version = GREATEST(plywise.user_settings.settings_version, EXCLUDED.settings_version) + 1, "
        "settings_json = CASE WHEN plywise.user_settings.settings_json ? 'player_identity' "
        "THEN plywise.user_settings.settings_json "
        "ELSE plywise.user_settings.settings_json || EXCLUDED.settings_json END, "
        "updated_at = now()",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.idempotency_records "
        "(owner_kind, owner_id, operation, idempotency_key, request_hash, resource_kind, resource_id, "
        "response_json, created_at, expires_at) "
        "SELECT 'account', $2, operation, idempotency_key, request_hash, resource_kind, resource_id, "
        "response_json, created_at, expires_at FROM plywise.idempotency_records "
        "WHERE owner_kind = 'guest' AND owner_id = $1 ON CONFLICT DO NOTHING",
        {guest_id, account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.outbox_events "
        "(owner_kind, owner_id, aggregate_kind, aggregate_id, event_kind, payload_json, created_at, published_at) "
        "SELECT 'account', $2, aggregate_kind, "
        "CASE WHEN aggregate_kind = 'analysis' THEN 'claim-' || md5($2 || ':' || aggregate_id) ELSE aggregate_id END, "
        "event_kind, payload_json, created_at, published_at "
        "FROM plywise.outbox_events WHERE owner_kind = 'guest' AND owner_id = $1",
        {guest_id, account_id}));

    static_cast<void>(execute(
        impl_->connection,
        "UPDATE plywise.guest_sessions SET claimed_by_owner_kind = 'account', "
        "claimed_by_account_id = $2, claimed_at = now() WHERE id = $1",
        {guest_id, account_id}));
    transaction.commit();
    return receipt;
}

AccountExport HostedIdentityStore::export_account(std::string account_id,
                                                  std::string idempotency_key) {
    validate_identity_text("account id", account_id, 256);
    validate_identity_text("account export idempotency key", idempotency_key, 256);
    constexpr std::size_t max_export_bytes = 64U * 1024U * 1024U;

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    const Result account = execute(
        impl_->connection,
        "SELECT 1 FROM plywise.accounts WHERE id = $1 FOR UPDATE",
        {account_id});
    if (PQntuples(account.get()) != 1)
        throw Error(ErrorCode::NotFound, "account does not exist");

    std::string request_id;
    const Result prior = execute(
        impl_->connection,
        "SELECT id, status, COALESCE(receipt_json::text, '') "
        "FROM plywise.account_data_requests "
        "WHERE owner_kind = 'account' AND owner_id = $1 AND request_kind = 'export' "
        "AND idempotency_key = $2 FOR UPDATE",
        {account_id, idempotency_key});
    if (PQntuples(prior.get()) == 1) {
        request_id = value_at(prior, 0, 0);
        if (value_at(prior, 0, 1) == "completed" && !value_at(prior, 0, 2).empty()) {
            const json::Value receipt = json::parse(value_at(prior, 0, 2));
            return AccountExport{receipt.at("request_id").as_string(), receipt.at("data"),
                                 static_cast<std::int64_t>(receipt.at("completed_at_ms")
                                                                .as_number())};
        }
        static_cast<void>(execute(
            impl_->connection,
            "UPDATE plywise.account_data_requests SET status = 'running', receipt_json = NULL, "
            "completed_at = NULL WHERE id = $1",
            {request_id}));
    } else {
        request_id = random_prefixed_id("export-");
        static_cast<void>(execute(
            impl_->connection,
            "INSERT INTO plywise.account_data_requests "
            "(id, owner_kind, owner_id, request_kind, idempotency_key, status) "
            "VALUES ($1, 'account', $2, 'export', $3, 'running')",
            {request_id, account_id, idempotency_key}));
    }

    const Result encoded = execute(
        impl_->connection,
        R"sql(
SELECT jsonb_build_object(
    'export_version', 1,
    'account', (SELECT jsonb_build_object(
        'id', id,
        'auth_provider', auth_provider,
        'auth_subject', auth_subject,
        'email', email,
        'created_at', created_at,
        'deletion_requested_at', deletion_requested_at
    ) FROM plywise.accounts WHERE id = $1),
    'games', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', g.id,
        'normalized_pgn', g.normalized_pgn,
        'metadata', g.metadata_json,
        'imported_at', go.imported_at,
        'source_kind', go.source_kind,
        'source_key', go.source_key,
        'provenance', go.provenance_json
    ) ORDER BY go.imported_at DESC, g.id)
        FROM plywise.game_owners go JOIN plywise.games g ON g.id = go.game_id
        WHERE go.owner_kind = 'account' AND go.owner_id = $1), '[]'::jsonb),
    'analysis_runs', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'game_id', game_id, 'idempotency_key', idempotency_key,
        'source', source, 'engine_build', engine_build,
        'engine_hash', CASE WHEN engine_hash IS NULL THEN NULL ELSE encode(engine_hash, 'hex') END,
        'profile_version', profile_version, 'classifier_version', classifier_version,
        'compatibility_key', compatibility_key, 'status', status,
        'created_at', created_at, 'completed_at', completed_at, 'supersedes_id', supersedes_id
    ) ORDER BY created_at, id)
        FROM plywise.analysis_runs WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'analysis_positions', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'run_id', run_id, 'game_id', game_id, 'ply', ply, 'sequence', sequence,
        'canonical_fen_hash', encode(canonical_fen_hash, 'hex'), 'depth', depth,
        'nodes', nodes, 'time_ms', time_ms, 'observation', observation_json,
        'validated_at', validated_at
    ) ORDER BY run_id, ply, sequence)
        FROM plywise.analysis_positions WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'move_assessments', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'run_id', run_id, 'game_id', game_id, 'ply', ply, 'assessment', assessment_json
    ) ORDER BY run_id, ply)
        FROM plywise.move_assessments WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'reviews', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'run_id', run_id, 'game_id', game_id, 'review', review_json, 'created_at', created_at
    ) ORDER BY created_at, run_id)
        FROM plywise.reviews WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'analysis_jobs', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'run_id', run_id, 'game_id', game_id, 'idempotency_key', idempotency_key,
        'priority', priority, 'status', status, 'attempt', attempt,
        'cancel_requested_at', cancel_requested_at, 'created_at', created_at, 'updated_at', updated_at
    ) ORDER BY created_at, id)
        FROM plywise.analysis_jobs WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'job_events', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'job_id', job_id, 'run_id', run_id, 'sequence', sequence, 'stage', stage,
        'completed_units', completed_units, 'total_units', total_units, 'created_at', created_at
    ) ORDER BY job_id, sequence)
        FROM plywise.job_events WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'chess_profiles', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'provider', provider, 'username', username, 'settings', settings_json,
        'sync_cursor', sync_cursor, 'last_successful_sync_at', last_successful_sync_at,
        'last_error', last_error
    ) ORDER BY provider)
        FROM plywise.chess_profiles WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'variations', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'game_id', game_id, 'root_ply', root_ply, 'root_position', root_position,
        'root_fen', root_fen, 'engine_configuration', engine_configuration,
        'current_node_id', current_node_id, 'created_at', created_at, 'updated_at', updated_at
    ) ORDER BY created_at, id)
        FROM plywise.variations WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'variation_nodes', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'variation_id', n.variation_id, 'node_id', n.node_id, 'parent_node_id', n.parent_node_id,
        'uci', n.uci, 'san', n.san, 'fen', n.fen
    ) ORDER BY n.variation_id, n.node_id)
        FROM plywise.variation_nodes n JOIN plywise.variations v ON v.id = n.variation_id
        WHERE v.owner_kind = 'account' AND v.owner_id = $1), '[]'::jsonb),
    'review_attempts', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'game_id', game_id, 'run_id', run_id, 'ply', ply, 'uci', uci,
        'accepted', accepted, 'attempted_at', attempted_at
    ) ORDER BY attempted_at, id)
        FROM plywise.review_attempts WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'intelligence_evidence', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'run_id', run_id, 'game_id', game_id, 'ply', ply,
        'evidence_kind', evidence_kind, 'model_version', model_version,
        'evidence', evidence_json, 'created_at', created_at
    ) ORDER BY created_at, id)
        FROM plywise.intelligence_evidence WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'practice_items', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'evidence_id', evidence_id, 'state', state, 'due_at', due_at,
        'schedule_version', schedule_version, 'created_at', created_at, 'updated_at', updated_at
    ) ORDER BY created_at, id)
        FROM plywise.practice_items WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'practice_outcomes', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'id', id, 'practice_item_id', practice_item_id, 'result', result,
        'response_time_ms', response_time_ms, 'hint_level', hint_level, 'attempted_at', attempted_at
    ) ORDER BY attempted_at, id)
        FROM plywise.practice_outcomes WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb),
    'settings', COALESCE((SELECT jsonb_agg(jsonb_build_object(
        'settings_version', settings_version, 'settings', settings_json, 'updated_at', updated_at
    ) ORDER BY updated_at DESC)
        FROM plywise.user_settings WHERE owner_kind = 'account' AND owner_id = $1), '[]'::jsonb)
)::text
)sql",
        {account_id});
    const std::string encoded_export = value_at(encoded, 0, 0);
    if (encoded_export.empty() || encoded_export.size() > max_export_bytes)
        throw Error(ErrorCode::IoError, "account export exceeds the storage safety limit");
    const json::Value data = json::parse(encoded_export);
    const std::int64_t completed_at_ms = now_ms();
    const json::Value receipt = json::Value::Object{
        {"request_id", request_id},
        {"completed_at_ms", static_cast<double>(completed_at_ms)},
        {"data", data},
    };
    static_cast<void>(execute(
        impl_->connection,
        "UPDATE plywise.account_data_requests SET status = 'completed', receipt_json = $2::jsonb, "
        "completed_at = to_timestamp($3::double precision / 1000) WHERE id = $1",
        {request_id, json::dump(receipt), std::to_string(completed_at_ms)}));
    transaction.commit();
    return AccountExport{std::move(request_id), data, completed_at_ms};
}

AccountDeletionReceipt HostedIdentityStore::delete_account(std::string account_id,
                                                           std::string idempotency_key) {
    validate_identity_text("account id", account_id, 256);
    validate_identity_text("account deletion idempotency key", idempotency_key, 256);
    constexpr std::int64_t backup_retention_ms = 30LL * 24 * 60 * 60 * 1000;

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    const Result account = execute(
        impl_->connection,
        "SELECT auth_provider, auth_subject FROM plywise.accounts WHERE id = $1 FOR UPDATE",
        {account_id});
    if (PQntuples(account.get()) != 1)
        throw Error(ErrorCode::NotFound, "account does not exist");

    const std::string auth_provider = value_at(account, 0, 0);
    const std::string auth_subject = value_at(account, 0, 1);
    const Result prior_receipt = execute(
        impl_->connection,
        "SELECT request_id, (EXTRACT(EPOCH FROM completed_at) * 1000)::bigint, "
        "(EXTRACT(EPOCH FROM expires_at) * 1000)::bigint "
        "FROM plywise.account_deletion_receipts WHERE account_id = $1 "
        "AND idempotency_key = $2 FOR UPDATE",
        {account_id, idempotency_key});
    if (PQntuples(prior_receipt.get()) == 1) {
        transaction.commit();
        // The raw receipt token is deliberately never stored, so a replay receives an empty
        // token while retaining the original trackable request and retention deadline.
        return AccountDeletionReceipt{value_at(prior_receipt, 0, 0), {},
                                      integer_at(prior_receipt, 0, 1),
                                      integer_at(prior_receipt, 0, 2)};
    }

    const Result prior = execute(
        impl_->connection,
        "SELECT status FROM plywise.account_data_requests "
        "WHERE owner_kind = 'account' AND owner_id = $1 AND request_kind = 'delete' "
        "AND idempotency_key = $2 FOR UPDATE",
        {account_id, idempotency_key});
    if (PQntuples(prior.get()) == 1 && value_at(prior, 0, 0) == "completed")
        throw Error(ErrorCode::NotFound, "account has already been deleted");

    const std::string request_id = random_prefixed_id("delete-");
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.account_data_requests "
        "(id, owner_kind, owner_id, request_kind, idempotency_key, status) "
        "VALUES ($1, 'account', $2, 'delete', $3, 'running') "
        "ON CONFLICT (owner_kind, owner_id, request_kind, idempotency_key) DO UPDATE "
        "SET status = 'running', receipt_json = NULL, completed_at = NULL",
        {request_id, account_id, idempotency_key}));

    // Claimed guest proofs point back to accounts without ON DELETE CASCADE. Remove the proof
    // before the account cascade so the account cannot retain a usable guest session.
    static_cast<void>(execute(
        impl_->connection,
        "DELETE FROM plywise.guest_sessions "
        "WHERE claimed_by_owner_kind = 'account' AND claimed_by_account_id = $1",
        {account_id}));
    static_cast<void>(execute(impl_->connection,
                              "DELETE FROM plywise.accounts WHERE id = $1", {account_id}));
    // The account row references its owner (the owner does not reference the account), so the
    // owner cascade is explicit. This removes every account-scoped projection and revokes the
    // owner before the transaction becomes visible.
    static_cast<void>(execute(
        impl_->connection,
        "DELETE FROM plywise.owners WHERE owner_kind = 'account' AND owner_id = $1",
        {account_id}));
    static_cast<void>(execute(
        impl_->connection,
        "DELETE FROM plywise.games g "
        "WHERE NOT EXISTS (SELECT 1 FROM plywise.game_owners go WHERE go.game_id = g.id)"));

    const std::int64_t completed_at_ms = now_ms();
    const std::int64_t retention_until_ms = completed_at_ms + backup_retention_ms;
    const std::string receipt_token = random_receipt_token();
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.account_deletion_receipts "
        "(request_id, receipt_token_hash, status, completed_at, expires_at, account_id, "
        "auth_provider, auth_subject_hash, idempotency_key) "
        "VALUES ($1, $2::bytea, 'completed', "
        "to_timestamp($3::double precision / 1000), "
        "to_timestamp($4::double precision / 1000), $5, $6, $7::bytea, $8)",
        {request_id, sha256_bytea(receipt_token), std::to_string(completed_at_ms),
         std::to_string(retention_until_ms), account_id, auth_provider,
         sha256_bytea(auth_subject), idempotency_key}));
    transaction.commit();
    return AccountDeletionReceipt{request_id, receipt_token, completed_at_ms,
                                  retention_until_ms};
}

} // namespace pct::app

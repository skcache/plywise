#include "pct/app/hosted_browser_observations.hpp"

#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <libpq-fe.h>

#include <charconv>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
        throw Error(ErrorCode::IoError, "PostgreSQL browser staging query failed");
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
    case OwnerKind::Guest: return "guest";
    case OwnerKind::Account: return "account";
    case OwnerKind::Local:
        throw Error(ErrorCode::InvalidArgument,
                    "hosted browser staging requires a guest or account owner");
    }
    throw Error(ErrorCode::InvalidArgument, "unknown hosted browser staging owner kind");
}

std::string value_at(const Result& result, int row, int column) {
    if (PQgetisnull(result.get(), row, column) != 0)
        return {};
    return PQgetvalue(result.get(), row, column);
}

std::size_t size_at(const Result& result, int row, int column) {
    const std::string value = value_at(result, row, column);
    std::size_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
        throw Error(ErrorCode::Corruption, "PostgreSQL browser staging count is invalid");
    return parsed;
}

bool boolean_at(const Result& result, int row, int column) {
    const std::string value = value_at(result, row, column);
    if (value == "t") return true;
    if (value == "f") return false;
    throw Error(ErrorCode::Corruption, "PostgreSQL browser staging flag is invalid");
}

void require_owner(PGconn* connection, const OwnerId& owner) {
    const std::string kind = owner_kind_name(owner.kind());
    const std::string id(owner.value());
    const Result result = execute(
        connection,
        "SELECT 1 FROM plywise.owners WHERE owner_kind = $1 AND owner_id = $2 "
        "AND (owner_kind = 'account' OR (expires_at > now() AND NOT EXISTS ("
        "SELECT 1 FROM plywise.guest_sessions WHERE owner_kind = 'guest' "
        "AND id = owner_id AND claimed_at IS NOT NULL)))",
        {kind, id});
    if (PQntuples(result.get()) != 1)
        throw Error(ErrorCode::NotFound, "browser staging owner does not exist");
}

void require_owned_game(PGconn* connection, const OwnerId& owner, std::string_view game_id) {
    const Result result = execute(
        connection,
        "SELECT 1 FROM plywise.game_owners WHERE game_id = $1 AND owner_kind = $2 "
        "AND owner_id = $3",
        {std::string(game_id), owner_kind_name(owner.kind()), std::string(owner.value())});
    if (PQntuples(result.get()) != 1)
        throw Error(ErrorCode::NotFound, "browser staging game does not exist");
}

std::string canonical_json(std::string_view encoded) {
    try {
        return json::dump(json::parse(encoded));
    } catch (...) {
        throw Error(ErrorCode::Corruption, "PostgreSQL browser observation JSON is invalid");
    }
}

struct RunRow {
    std::string game_id;
    std::string profile;
    std::size_t expected_observations{0};
    std::size_t next_sequence{0};
    bool finalized{false};
};

RunRow run_from_row(const Result& result) {
    return RunRow{value_at(result, 0, 0), value_at(result, 0, 1), size_at(result, 0, 2),
                  size_at(result, 0, 3), boolean_at(result, 0, 4)};
}

} // namespace

struct HostedBrowserObservationStore::Impl {
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

HostedBrowserObservationStore::HostedBrowserObservationStore(std::string connection_string)
    : impl_(std::make_unique<Impl>(connection_string)) {}

HostedBrowserObservationStore::~HostedBrowserObservationStore() = default;

void HostedBrowserObservationStore::begin(
    const OwnerId& owner, const analysis::BrowserObservationRunContext& context) {
    if (context.game_id.empty() || context.analysis_run_id.empty() || context.profile.empty())
        throw Error(ErrorCode::InvalidArgument, "browser staging run context is invalid");
    analysis::validate_browser_observation_profile(context.profile);
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner);
    require_owned_game(impl_->connection, owner, context.game_id);
    const std::string kind = owner_kind_name(owner.kind());
    const std::string id(owner.value());
    const Result existing = execute(
        impl_->connection,
        "SELECT game_id, profile, expected_observations, next_sequence, finalized "
        "FROM plywise.browser_observation_runs "
        "WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3 FOR UPDATE",
        {kind, id, context.analysis_run_id});
    if (PQntuples(existing.get()) == 1) {
        const RunRow run = run_from_row(existing);
        if (run.game_id != context.game_id || run.profile != context.profile ||
            (run.expected_observations != 0 && context.expected_observations != 0 &&
             run.expected_observations != context.expected_observations))
            throw Error(ErrorCode::InvalidArgument, "browser staging run context changed");
        if (run.expected_observations == 0 && context.expected_observations != 0)
            static_cast<void>(execute(
                impl_->connection,
                "UPDATE plywise.browser_observation_runs SET expected_observations = $1, "
                "updated_at = now() WHERE owner_kind = $2 AND owner_id = $3 "
                "AND analysis_run_id = $4",
                {std::to_string(context.expected_observations), kind, id,
                 context.analysis_run_id}));
        transaction.commit();
        return;
    }
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.browser_observation_runs "
        "(owner_kind, owner_id, game_id, analysis_run_id, profile, expected_observations) "
        "VALUES ($1, $2, $3, $4, $5, $6)",
        {kind, id, context.game_id, context.analysis_run_id, context.profile,
         std::to_string(context.expected_observations)}));
    transaction.commit();
}

analysis::BrowserObservationReceipt HostedBrowserObservationStore::submit(
    const OwnerId& owner, const analysis::BrowserObservationContext& context,
    const analysis::BrowserEngineObservation& observation) {
    static_cast<void>(analysis::validate_browser_observation(context, observation));
    const std::string encoded = json::dump(analysis::browser_observation_to_json(observation));
    const std::string kind = owner_kind_name(owner.kind());
    const std::string id(owner.value());
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner);
    const Result run_result = execute(
        impl_->connection,
        "SELECT game_id, profile, expected_observations, next_sequence, finalized "
        "FROM plywise.browser_observation_runs "
        "WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3 FOR UPDATE",
        {kind, id, observation.analysis_run_id});
    if (PQntuples(run_result.get()) != 1)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is not registered");
    const RunRow run = run_from_row(run_result);
    if (run.game_id != context.game_id || run.profile != context.profile ||
        observation.analysis_run_id != context.analysis_run_id)
        throw Error(ErrorCode::InvalidArgument, "browser staging run context changed");
    const Result existing = execute(
        impl_->connection,
        "SELECT observation_json::text FROM plywise.browser_observations "
        "WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3 AND sequence = $4",
        {kind, id, observation.analysis_run_id, std::to_string(observation.sequence)});
    if (PQntuples(existing.get()) == 1) {
        if (canonical_json(value_at(existing, 0, 0)) != encoded)
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation replay conflicts with prior data");
        transaction.commit();
        return analysis::BrowserObservationReceipt{
            analysis::BrowserObservationDisposition::Duplicate, observation.sequence};
    }
    if (run.finalized)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is already finalized");
    if (run.expected_observations != 0 && run.next_sequence >= run.expected_observations)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is already complete");
    if (observation.sequence != run.next_sequence)
        throw Error(ErrorCode::InvalidArgument, "browser observation sequence is stale or incomplete");
    static_cast<void>(execute(
        impl_->connection,
        "INSERT INTO plywise.browser_observations "
        "(owner_kind, owner_id, analysis_run_id, sequence, ply, observation_json) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb)",
        {kind, id, observation.analysis_run_id, std::to_string(observation.sequence),
         std::to_string(observation.ply), encoded}));
    static_cast<void>(execute(
        impl_->connection,
        "UPDATE plywise.browser_observation_runs SET next_sequence = next_sequence + 1, "
        "updated_at = now() WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3",
        {kind, id, observation.analysis_run_id}));
    transaction.commit();
    return analysis::BrowserObservationReceipt{analysis::BrowserObservationDisposition::Accepted,
                                               observation.sequence};
}

analysis::BrowserObservationBundle HostedBrowserObservationStore::finalize(
    const OwnerId& owner, std::string_view game_id, std::string_view analysis_run_id) {
    const std::string kind = owner_kind_name(owner.kind());
    const std::string id(owner.value());
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    require_owner(impl_->connection, owner);
    const Result run_result = execute(
        impl_->connection,
        "SELECT game_id, profile, expected_observations, next_sequence, finalized "
        "FROM plywise.browser_observation_runs "
        "WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3 FOR UPDATE",
        {kind, id, std::string(analysis_run_id)});
    if (PQntuples(run_result.get()) != 1)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is not registered");
    const RunRow run = run_from_row(run_result);
    if (run.game_id != game_id)
        throw Error(ErrorCode::InvalidArgument, "browser observation game does not match the run");
    if (run.expected_observations == 0 || run.next_sequence != run.expected_observations)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is incomplete");

    const Result observations = execute(
        impl_->connection,
        "SELECT sequence, observation_json::text FROM plywise.browser_observations "
        "WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3 "
        "ORDER BY sequence",
        {kind, id, std::string(analysis_run_id)});
    if (static_cast<std::size_t>(PQntuples(observations.get())) != run.expected_observations)
        throw Error(ErrorCode::Corruption, "browser staging observation count is inconsistent");

    analysis::BrowserObservationBundle bundle;
    bundle.context = analysis::BrowserObservationRunContext{
        run.game_id, std::string(analysis_run_id), run.profile, run.expected_observations};
    bundle.observations.reserve(run.expected_observations);
    for (int row = 0; row < PQntuples(observations.get()); ++row) {
        analysis::BrowserEngineObservation observation;
        try {
            observation = analysis::browser_observation_from_json(
                json::parse(value_at(observations, row, 1)));
        } catch (const Error&) {
            throw;
        } catch (...) {
            throw Error(ErrorCode::Corruption, "PostgreSQL browser observation JSON is invalid");
        }
        if (observation.sequence != size_at(observations, row, 0))
            throw Error(ErrorCode::Corruption, "browser staging sequence is inconsistent");
        bundle.observations.push_back(observation);
    }
    if (!run.finalized)
        static_cast<void>(execute(
            impl_->connection,
            "UPDATE plywise.browser_observation_runs SET finalized = TRUE, updated_at = now() "
            "WHERE owner_kind = $1 AND owner_id = $2 AND analysis_run_id = $3",
            {kind, id, std::string(analysis_run_id)}));
    transaction.commit();
    return bundle;
}

} // namespace pct::app

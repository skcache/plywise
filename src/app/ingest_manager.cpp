#include "pct/app/ingest_manager.hpp"

#include "pct/common/error.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>

namespace pct::app {
namespace {

constexpr std::int64_t day_ms = 24LL * 60LL * 60LL * 1000LL;
constexpr std::size_t chunk_entry_overhead = 1024;

std::int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string month_from_archive(std::string_view url) {
    if (url.size() < 7)
        throw Error(ErrorCode::ParseError, "Chess.com archive URL has no month");
    const std::string_view tail = url.substr(url.size() - 7);
    const std::string month = std::string(tail.substr(0, 4)) + "-" +
                              std::string(tail.substr(5, 2));
    if (!valid_chesscom_month(month))
        throw Error(ErrorCode::ParseError, "Chess.com archive URL has an invalid month");
    return month;
}

std::string utc_month(std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &seconds);
#else
    gmtime_r(&seconds, &value);
#endif
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m");
    return output.str();
}

std::optional<std::int64_t> pgn_utc_time_ms(const chess::Game& game) {
    const std::string date = game.tag("UTCDate");
    const std::string time = game.tag("UTCTime");
    if (date.size() != 10 || time.size() != 8 || date.find('?') != std::string::npos ||
        time.find('?') != std::string::npos)
        return std::nullopt;
    std::tm parsed{};
    std::istringstream input(date + " " + time);
    input >> std::get_time(&parsed, "%Y.%m.%d %H:%M:%S");
    if (input.fail())
        return std::nullopt;
    const int year = parsed.tm_year;
    const int month = parsed.tm_mon;
    const int day = parsed.tm_mday;
    const int hour = parsed.tm_hour;
    const int minute = parsed.tm_min;
    const int second = parsed.tm_sec;
#if defined(_WIN32)
    const std::time_t encoded = _mkgmtime(&parsed);
#else
    const std::time_t encoded = timegm(&parsed);
#endif
    if (encoded < 0)
        return std::nullopt;
    std::tm roundtrip{};
#if defined(_WIN32)
    gmtime_s(&roundtrip, &encoded);
#else
    gmtime_r(&encoded, &roundtrip);
#endif
    if (roundtrip.tm_year != year || roundtrip.tm_mon != month ||
        roundtrip.tm_mday != day || roundtrip.tm_hour != hour ||
        roundtrip.tm_min != minute || roundtrip.tm_sec != second)
        return std::nullopt;
    return static_cast<std::int64_t>(encoded) * 1000;
}

bool exact_binding(const import::ImportedGame& imported, std::string_view game_id) {
    try {
        if (import::ImportService::parse_chesscom_url(imported.source_url).game_id != game_id)
            return false;
        for (const std::string_view tag : {std::string_view{"Site"}, std::string_view{"Link"}}) {
            const std::string value = imported.game.tag(tag);
            if (value.empty()) continue;
            try {
                if (import::ImportService::parse_chesscom_url(value).game_id != game_id)
                    return false;
            } catch (const Error&) {
                // Chess.com's current archive PGNs use `[Site "Chess.com"]` and
                // often omit Link. The exact archive-entry URL remains the
                // authoritative binding in that format.
            }
        }
        return true;
    } catch (const Error&) {
        return false;
    }
}

std::optional<ChessComArchiveEntry>
archive_entry(const import::ChessComArchiveGame& game, std::string_view username,
              std::string_view month, std::string_view source_url,
              std::int64_t fetched_at_ms, bool require_utc_time) {
    const auto parsed_url = import::ImportService::parse_chesscom_url(game.url);
    import::ImportedGame imported{chess::parse_pgn(game.pgn), parsed_url.canonical, game.pgn,
                                  import::ImportMethod::PublicApi};
    if (!exact_binding(imported, parsed_url.game_id))
        throw Error(ErrorCode::ParseError,
                    "Chess.com archive PGN did not match its exact game identifier");
    const auto ended_at = pgn_utc_time_ms(imported.game);
    if (require_utc_time && !ended_at)
        return std::nullopt;
    return ChessComArchiveEntry{parsed_url.game_id,
                                parsed_url.canonical,
                                game.pgn,
                                normalize_chesscom_username(username),
                                std::string(month),
                                game.time_class,
                                ended_at.value_or(0),
                                fetched_at_ms,
                                std::string(source_url)};
}

std::size_t persist_archive_chunks(IGameRepository& repository,
                                   const std::vector<ChessComArchiveEntry>& entries) {
    std::size_t indexed = 0;
    std::vector<ChessComArchiveEntry> chunk;
    std::size_t bytes = 0;
    auto flush = [&]() {
        if (!chunk.empty()) {
            indexed += repository.index_chesscom_archive_chunk(std::move(chunk));
            chunk.clear();
            bytes = 0;
        }
    };
    for (const auto& entry : entries) {
        const std::size_t estimate = entry.pgn.size() + entry.canonical_url.size() +
                                     entry.username.size() + entry.source_url.size() +
                                     chunk_entry_overhead;
        if (estimate > chesscom_archive_chunk_encoded_byte_limit)
            throw Error(ErrorCode::InvalidArgument,
                        "Chess.com archive entry exceeds the durable chunk byte limit");
        if (chunk.size() == ingest_archive_chunk_limit ||
            bytes + estimate > chesscom_archive_chunk_encoded_byte_limit) {
            flush();
        }
        chunk.push_back(entry);
        bytes += estimate;
    }
    flush();
    return indexed;
}

std::vector<std::string> recovery_actions(bool has_username) {
    std::vector<std::string> actions;
    if (!has_username)
        actions.push_back("configure_profile");
    actions.push_back("search_date_window");
    actions.push_back("retry");
    actions.push_back("paste_pgn");
    return actions;
}

std::string sync_cursor(const IngestSync& sync) {
    return "v1;days=" + std::to_string(sync.days) +
           ";cutoff_ms=" + std::to_string(sync.cutoff_ms) +
           ";upper_ms=" + std::to_string(sync.upper_bound_ms);
}

std::optional<std::int64_t> cursor_number(std::string_view cursor, std::string_view key) {
    const std::string prefix = std::string(key) + "=";
    std::size_t offset = 0;
    while (offset <= cursor.size()) {
        const std::size_t end = cursor.find(';', offset);
        const std::string_view item = cursor.substr(offset, end - offset);
        if (item.starts_with(prefix)) {
            std::int64_t result = 0;
            const std::string_view value = item.substr(prefix.size());
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
                return result;
            return std::nullopt;
        }
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return std::nullopt;
}

bool active_status(std::string_view status) {
    return status == "queued" || status == "running";
}

} // namespace

json::Value to_json(const ChessComProfile& profile) {
    json::Value::Array controls;
    for (const auto& control : profile.selected_time_controls)
        controls.emplace_back(control);
    return json::Value::Object{{"username", profile.original_username},
                               {"normalized_username", profile.normalized_username},
                               {"selected_time_controls", std::move(controls)},
                               {"archive_cursor", profile.archive_cursor},
                               {"last_successful_sync_ms", static_cast<double>(profile.last_successful_sync_ms)},
                               {"last_error", profile.last_error}};
}

json::Value to_json(const ChessComArchiveEntry& entry) {
    return json::Value::Object{{"game_id", entry.game_id}, {"url", entry.canonical_url},
                               {"username", entry.username}, {"month", entry.month},
                               {"time_class", entry.time_class},
                               {"end_time_ms", static_cast<double>(entry.end_time_ms)},
                               {"fetched_at_ms", static_cast<double>(entry.fetched_at_ms)},
                               {"source_url", entry.source_url}};
}

json::Value to_json(const ImportResolution& value) {
    json::Value::Array actions;
    for (const auto& action : value.actions) actions.emplace_back(action);
    return json::Value::Object{{"id", value.id}, {"status", value.status},
        {"url", value.supplied_url}, {"canonical_url", value.canonical_url},
        {"game_id", value.game_id}, {"username", value.username}, {"source", value.source},
        {"imported_game_id", value.imported_game_id}, {"code", value.code},
        {"error", value.error}, {"actions", std::move(actions)},
        {"created_at_ms", static_cast<double>(value.created_at_ms)},
        {"updated_at_ms", static_cast<double>(value.updated_at_ms)}};
}

json::Value to_json(const IngestSync& value) {
    return json::Value::Object{{"id", value.id}, {"status", value.status},
        {"username", value.username}, {"days", value.days}, {"max_months", value.max_months},
        {"cutoff_ms", static_cast<double>(value.cutoff_ms)},
        {"upper_bound_ms", static_cast<double>(value.upper_bound_ms)},
        {"current_month", value.current_month}, {"months_completed", value.months_completed},
        {"games_indexed", value.games_indexed}, {"code", value.code}, {"error", value.error},
        {"created_at_ms", static_cast<double>(value.created_at_ms)},
        {"updated_at_ms", static_cast<double>(value.updated_at_ms)}};
}

IngestManager::IngestManager(import::ImportService& importer, IRepository& repository,
                             JobManager&, import::HttpTransport transport,
                             import::RetrySleeper sleeper, IngestOptions options,
                             std::string startup_username)
    : importer_(importer), repository_(repository),
      client_(std::move(transport), std::move(sleeper)), options_(std::move(options)) {
    if (options_.max_pending == 0 || options_.max_history == 0)
        throw Error(ErrorCode::InvalidArgument, "ingest bounds must be positive");
    if (!options_.clock)
        options_.clock = system_now_ms;
    if (!startup_username.empty())
        configure_profile(std::move(startup_username));

    const ChessComSyncState prior = repository_.chesscom_sync_state();
    const auto configured = repository_.chesscom_profile();
    if (prior.status == "running" && !prior.username.empty() && configured &&
        configured->normalized_username == prior.username) {
        const auto days = cursor_number(prior.cursor, "days");
        const auto cutoff = cursor_number(prior.cursor, "cutoff_ms");
        const auto upper = cursor_number(prior.cursor, "upper_ms");
        if (days && cutoff && upper) {
            IngestSync resumed;
            resumed.id = "sync-" + std::to_string(next_sync_id_++);
            resumed.username = prior.username;
            resumed.days = static_cast<int>(*days);
            resumed.max_months = max_months_for_days(resumed.days);
            resumed.cutoff_ms = *cutoff;
            resumed.upper_bound_ms = *upper;
            resumed.current_month = prior.current_month;
            resumed.games_indexed = prior.games_indexed;
            resumed.created_at_ms = prior.started_at_ms;
            resumed.updated_at_ms = now_ms();
            syncs_.emplace(resumed.id, resumed);
            sync_runtime_.emplace(resumed.id, SyncRuntime{});
            sync_order_.push_back(resumed.id);
            current_sync_ = resumed.id;
            historical_queue_.push_back(resumed.id);
        }
    } else if (prior.status == "running") {
        repository_.save_chesscom_sync_state(
            {"paused", prior.username, prior.cursor, prior.current_month,
             prior.months_completed, prior.games_indexed, prior.started_at_ms, now_ms(),
             "configured profile changed; resume was not started"});
    }
    interactive_worker_ = std::thread([this] { interactive_work(); });
    historical_worker_ = std::thread([this] { historical_work(); });
}

IngestManager::~IngestManager() {
    set_observer({});
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [_, source] : resolution_cancellations_) source.request_stop();
        for (auto& [_, runtime] : sync_runtime_) runtime.cancellation.request_stop();
    }
    interactive_condition_.notify_all();
    historical_condition_.notify_all();
    if (interactive_worker_.joinable()) interactive_worker_.join();
    if (historical_worker_.joinable()) historical_worker_.join();
}

std::int64_t IngestManager::now_ms() const { return options_.clock(); }

std::size_t IngestManager::max_months_for_days(int days) {
    if (days == 7 || days == 30) return 2;
    if (days == 90) return 4;
    throw Error(ErrorCode::InvalidArgument, "sync days must be 7, 30, or 90");
}

void IngestManager::configure_profile(std::string username,
                                      std::vector<std::string> controls) {
    ChessComProfile value;
    value.original_username = std::move(username);
    value.selected_time_controls = std::move(controls);
    if (const auto existing = repository_.chesscom_profile(); existing &&
        normalize_chesscom_username(value.original_username) == existing->normalized_username) {
        value.archive_cursor = existing->archive_cursor;
        value.last_successful_sync_ms = existing->last_successful_sync_ms;
        value.last_error = existing->last_error;
        if (value.selected_time_controls.empty())
            value.selected_time_controls = existing->selected_time_controls;
    }
    repository_.save_chesscom_profile(std::move(value));
    notify("profile", to_json(*repository_.chesscom_profile()));
}

std::optional<ChessComProfile> IngestManager::profile() const {
    return repository_.chesscom_profile();
}

ImportResolution IngestManager::resolve(std::string url, std::string username) {
    const import::ChessComUrl parsed = import::ImportService::parse_chesscom_url(url);
    if (username.empty()) {
        if (const auto configured = profile()) username = configured->original_username;
    } else {
        static_cast<void>(import::ChessComArchiveClient::archive_index_url(username));
        configure_profile(username);
    }
    ImportResolution result;
    {
        std::lock_guard lock(mutex_);
        if (const auto active = active_canonical_resolutions_.find(parsed.canonical);
            active != active_canonical_resolutions_.end())
            return resolutions_.at(active->second);
        if (interactive_queue_.size() >= options_.max_pending)
            throw Error(ErrorCode::InvalidArgument, "interactive ingest queue is full");
        result.id = "resolution-" + std::to_string(next_resolution_id_++);
        result.supplied_url = std::move(url);
        result.canonical_url = parsed.canonical;
        result.game_id = parsed.game_id;
        result.username = std::move(username);
        result.created_at_ms = result.updated_at_ms = now_ms();
        resolutions_.emplace(result.id, result);
        resolution_cancellations_.emplace(result.id, CancellationSource{});
        active_canonical_resolutions_.emplace(result.canonical_url, result.id);
        resolution_order_.push_back(result.id);
        interactive_queue_.push_back(result.id);
        trim_history_unlocked();
    }
    interactive_condition_.notify_one();
    notify("resolution", to_json(result));
    return result;
}

std::optional<ImportResolution> IngestManager::resolution(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto found = resolutions_.find(std::string(id));
    return found == resolutions_.end() ? std::nullopt :
           std::optional<ImportResolution>(found->second);
}

bool IngestManager::cancel_resolution(std::string_view id) {
    ImportResolution state;
    {
        std::lock_guard lock(mutex_);
        const auto found = resolutions_.find(std::string(id));
        if (found == resolutions_.end() || !active_status(found->second.status)) return false;
        resolution_cancellations_.at(found->first).request_stop();
        found->second.status = "cancelled";
        found->second.code = "cancelled";
        found->second.error = "resolution was cancelled";
        found->second.updated_at_ms = now_ms();
        active_canonical_resolutions_.erase(found->second.canonical_url);
        std::erase(interactive_queue_, found->first);
        state = found->second;
    }
    notify("resolution", to_json(state));
    return true;
}

IngestSync IngestManager::start_sync(int days, std::string username) {
    const std::size_t max_months = max_months_for_days(days);
    const bool supplied_username = !username.empty();
    if (username.empty()) {
        const auto configured = profile();
        if (!configured)
            throw Error(ErrorCode::InvalidArgument, "a public Chess.com username is required for sync");
        username = configured->original_username;
    } else
        static_cast<void>(import::ChessComArchiveClient::archive_index_url(username));
    username = normalize_chesscom_username(username);
    IngestSync result;
    std::optional<ChessComProfile> configured_profile;
    {
        std::lock_guard lock(mutex_);
        if (current_sync_) {
            const auto& current = syncs_.at(*current_sync_);
            if (active_status(current.status)) {
                if (current.username == username && current.days == days) return current;
                throw IngestConflict("another Chess.com sync is already active for a different profile or window");
            }
        }
        if (historical_queue_.size() >= options_.max_pending)
            throw Error(ErrorCode::InvalidArgument, "historical ingest queue is full");
        result.id = "sync-" + std::to_string(next_sync_id_++);
        result.username = username;
        result.days = days;
        result.max_months = max_months;
        result.upper_bound_ms = now_ms();
        result.cutoff_ms = result.upper_bound_ms - static_cast<std::int64_t>(days) * day_ms;
        result.created_at_ms = result.updated_at_ms = result.upper_bound_ms;
        if (supplied_username) {
            ChessComProfile value;
            value.original_username = username;
            if (const auto existing = repository_.chesscom_profile(); existing &&
                existing->normalized_username == username) {
                value.selected_time_controls = existing->selected_time_controls;
                value.archive_cursor = existing->archive_cursor;
                value.last_successful_sync_ms = existing->last_successful_sync_ms;
                value.last_error = existing->last_error;
            }
            repository_.save_chesscom_profile(std::move(value));
            configured_profile = repository_.chesscom_profile();
        }
        repository_.save_chesscom_sync_state(
            {"running", username, sync_cursor(result), "", 0, 0, result.created_at_ms,
             result.updated_at_ms, ""});
        syncs_.emplace(result.id, result);
        sync_runtime_.emplace(result.id, SyncRuntime{});
        sync_order_.push_back(result.id);
        current_sync_ = result.id;
        historical_queue_.push_back(result.id);
        trim_history_unlocked();
    }
    historical_condition_.notify_one();
    if (configured_profile)
        notify("profile", to_json(*configured_profile));
    notify("sync", to_json(result));
    return result;
}

std::optional<IngestSync> IngestManager::sync(std::string_view id) const {
    std::lock_guard lock(mutex_);
    std::string key(id);
    if (key == "current") {
        if (!current_sync_) return std::nullopt;
        key = *current_sync_;
    }
    const auto found = syncs_.find(key);
    return found == syncs_.end() ? std::nullopt : std::optional<IngestSync>(found->second);
}

bool IngestManager::cancel_sync(std::string_view id) {
    IngestSync state;
    {
        std::lock_guard lock(mutex_);
        std::string key(id);
        if (key == "current") {
            if (!current_sync_) return false;
            key = *current_sync_;
        }
        const auto found = syncs_.find(key);
        if (found == syncs_.end() || !active_status(found->second.status)) return false;
        sync_runtime_.at(key).cancellation.request_stop();
        found->second.status = "cancelled";
        found->second.code = "cancelled";
        found->second.error = "sync was cancelled";
        found->second.updated_at_ms = now_ms();
        std::erase(historical_queue_, key);
        state = found->second;
    }
    repository_.save_chesscom_sync_state(
        {"paused", state.username, sync_cursor(state), state.current_month,
         state.months_completed, state.games_indexed, state.created_at_ms,
         state.updated_at_ms, state.error});
    notify("sync", to_json(state));
    return true;
}

json::Value IngestManager::snapshot() const {
    std::lock_guard lock(mutex_);
    json::Value::Array resolutions;
    for (const auto& id : resolution_order_)
        if (const auto found = resolutions_.find(id); found != resolutions_.end())
            resolutions.push_back(to_json(found->second));
    json::Value::Array syncs;
    for (const auto& id : sync_order_)
        if (const auto found = syncs_.find(id); found != syncs_.end())
            syncs.push_back(to_json(found->second));
    return json::Value::Object{{"resolutions", std::move(resolutions)}, {"syncs", std::move(syncs)},
        {"queued_interactive", interactive_queue_.size()},
        {"queued_historical", historical_queue_.size()}, {"queue_capacity", options_.max_pending}};
}

void IngestManager::set_observer(IngestObserver observer) {
    std::unique_lock lock(mutex_);
    observer_ = std::move(observer);
    if (!observer_)
        observer_condition_.wait(lock, [&] { return observer_calls_ == 0; });
}

void IngestManager::notify(std::string_view event, const json::Value& payload) {
    IngestObserver observer;
    {
        std::lock_guard lock(mutex_);
        observer = observer_;
        if (observer) ++observer_calls_;
    }
    if (!observer) return;
    try {
        observer(json::Value::Object{{"type", "ingest_update"},
                                     {"event", std::string(event)}, {"payload", payload}});
    } catch (...) {
        std::lock_guard lock(mutex_);
        --observer_calls_;
        observer_condition_.notify_all();
        return;
    }
    std::lock_guard lock(mutex_);
    --observer_calls_;
    observer_condition_.notify_all();
}

void IngestManager::trim_history_unlocked() {
    while (resolution_order_.size() > options_.max_history) {
        const auto id = resolution_order_.front();
        if (active_status(resolutions_.at(id).status)) break;
        resolutions_.erase(id);
        resolution_cancellations_.erase(id);
        resolution_order_.pop_front();
    }
    while (sync_order_.size() > options_.max_history) {
        const auto id = sync_order_.front();
        if (active_status(syncs_.at(id).status)) break;
        syncs_.erase(id);
        sync_runtime_.erase(id);
        sync_order_.pop_front();
    }
}

void IngestManager::interactive_work() {
    for (;;) {
        std::string id;
        {
            std::unique_lock lock(mutex_);
            interactive_condition_.wait(lock, [&] { return stopping_ || !interactive_queue_.empty(); });
            if (stopping_) return;
            id = std::move(interactive_queue_.front());
            interactive_queue_.pop_front();
        }
        run_resolution(std::move(id));
    }
}

void IngestManager::historical_work() {
    for (;;) {
        std::string id;
        {
            std::unique_lock lock(mutex_);
            historical_condition_.wait(lock, [&] { return stopping_ || !historical_queue_.empty(); });
            if (stopping_) return;
            id = std::move(historical_queue_.front());
            historical_queue_.pop_front();
        }
        run_sync_step(std::move(id));
    }
}

void IngestManager::run_resolution(std::string id) {
    ImportResolution state;
    CancellationToken cancellation;
    {
        std::lock_guard lock(mutex_);
        auto found = resolutions_.find(id);
        if (found == resolutions_.end() || found->second.status == "cancelled") return;
        found->second.status = "running";
        found->second.updated_at_ms = now_ms();
        state = found->second;
        cancellation = resolution_cancellations_.at(id).get_token();
    }
    notify("resolution", to_json(state));
    try {
        std::optional<import::ImportedGame> imported;
        if (const auto local = repository_.chesscom_archive_entry(state.game_id)) {
            auto candidate = importer_.from_pgn(local->pgn, local->canonical_url);
            if (!exact_binding(candidate, state.game_id))
                throw Error(ErrorCode::Corruption, "local archive PGN did not match its exact game identifier");
            imported = std::move(candidate);
            state.source = "local_archive";
        }
        const auto search_archive = [&](const std::string& username) -> bool {
            auto index = client_.discover(username, {}, cancellation);
            std::sort(index.archives.begin(), index.archives.end(), std::greater<>());
            if (index.archives.size() > 2) index.archives.resize(2);
            for (const auto& archive_url : index.archives) {
                const std::string month = month_from_archive(archive_url);
                const auto page = client_.fetch_month(username, month.substr(0, 4),
                                                      month.substr(5, 2), {}, cancellation);
                std::vector<ChessComArchiveEntry> entries;
                for (const auto& game : page.games) {
                    try {
                        if (auto entry = archive_entry(game, username, month, archive_url,
                                                       now_ms(), false))
                            entries.push_back(std::move(*entry));
                    } catch (const Error&) {
                        // A single incomplete remote record should not prevent the exact
                        // game from reaching the callback/page fallback below.
                    }
                }
                static_cast<void>(persist_archive_chunks(repository_, entries));
                if (const auto local = repository_.chesscom_archive_entry(state.game_id)) {
                    imported = importer_.from_pgn(local->pgn, local->canonical_url);
                    if (!exact_binding(*imported, state.game_id))
                        throw Error(ErrorCode::Corruption, "archive PGN did not match its exact game identifier");
                    state.source = "profile_archive";
                    state.username = username;
                    return true;
                }
            }
            return false;
        };

        std::vector<std::string> candidate_users;
        bool page_confirmed = false;
        if (!imported) {
            try {
                const auto players = importer_.discover_players(state.canonical_url, cancellation);
                page_confirmed = true;
                candidate_users = {players.white, players.black};
            } catch (const import::ChessComClientError& error) {
                if (error.failure() == import::ChessComFailure::Cancelled) throw;
            } catch (const Error&) {
                // Older or restricted game pages may omit public player metadata;
                // the existing PGN extraction and recovery path remains available.
            }
        }
        if (!state.username.empty()) candidate_users.push_back(state.username);
        std::set<std::string> attempted_users;
        for (const std::string& username : candidate_users) {
            if (imported ||
                !attempted_users.insert(normalize_chesscom_username(username)).second)
                continue;
            try {
                static_cast<void>(search_archive(username));
            } catch (const import::ChessComClientError& error) {
                if (error.failure() != import::ChessComFailure::NotFound &&
                    error.failure() != import::ChessComFailure::Gone)
                    throw;
            }
        }
        if (!imported) {
            try {
                imported = importer_.from_url(state.canonical_url, cancellation);
                if (!exact_binding(*imported, state.game_id))
                    throw Error(ErrorCode::ParseError, "public page PGN did not match its exact game identifier");
                state.source = "public_page";
            } catch (const import::ChessComClientError& error) {
                if (error.failure() == import::ChessComFailure::Cancelled) throw;
                state.status = "needs_recovery";
                state.code = page_confirmed ? "archive_unavailable"
                                            : (state.username.empty() ? "profile_required"
                                                                      : "game_not_found");
                state.error = page_confirmed
                    ? "Chess.com confirms this game exists, but its PGN was not available through either player's public archive. Retry or paste the PGN."
                    : (state.username.empty()
                    ? "Connect the public Chess.com username for this game, choose a date window, or paste its PGN."
                    : std::string("The exact game was not found in the bounded recent archive scan: ") + error.what());
                state.actions = recovery_actions(!state.username.empty());
            } catch (const Error& error) {
                state.status = "needs_recovery";
                state.code = page_confirmed ? "archive_unavailable"
                                            : (state.username.empty() ? "profile_required"
                                                                      : "game_not_found");
                state.error = page_confirmed
                    ? "Chess.com confirms this game exists, but its PGN was not available through either player's public archive. Retry or paste the PGN."
                    : error.what();
                state.actions = recovery_actions(!state.username.empty());
            }
        }
        if (imported) {
            static_cast<void>(repository_.add(*imported));
            state.status = "resolved";
            state.imported_game_id = imported->game.identity;
        }
    } catch (const import::ChessComClientError& error) {
        if (error.failure() == import::ChessComFailure::Cancelled) {
            state.status = "cancelled";
            state.code = "cancelled";
            state.error = "resolution was cancelled";
        } else {
            state.status = "needs_recovery";
            state.code = "resolution_failed";
            state.error = error.what();
            state.actions = recovery_actions(!state.username.empty());
        }
    } catch (const Error& error) {
        state.status = "needs_recovery";
        state.code = state.username.empty() ? "profile_required" : "resolution_failed";
        state.error = error.what();
        state.actions = recovery_actions(!state.username.empty());
    }
    state.updated_at_ms = now_ms();
    {
        std::lock_guard lock(mutex_);
        if (resolutions_.at(id).status == "cancelled") state = resolutions_.at(id);
        else resolutions_.insert_or_assign(id, state);
        active_canonical_resolutions_.erase(state.canonical_url);
        trim_history_unlocked();
    }
    notify("resolution", to_json(state));
}

void IngestManager::run_sync_step(std::string id) {
    IngestSync state;
    CancellationToken cancellation;
    bool discover = false;
    {
        std::lock_guard lock(mutex_);
        auto found = syncs_.find(id);
        if (found == syncs_.end() || found->second.status == "cancelled") return;
        found->second.status = "running";
        found->second.updated_at_ms = now_ms();
        state = found->second;
        auto& runtime = sync_runtime_.at(id);
        cancellation = runtime.cancellation.get_token();
        discover = !runtime.discovered;
    }
    try {
        if (discover) {
            auto index = client_.discover(state.username, {}, cancellation);
            std::sort(index.archives.begin(), index.archives.end(), std::greater<>());
            index.archives.erase(std::unique(index.archives.begin(), index.archives.end()), index.archives.end());
            if (index.archives.size() > state.max_months) index.archives.resize(state.max_months);
            std::lock_guard lock(mutex_);
            auto& runtime = sync_runtime_.at(id);
            runtime.archives = std::move(index.archives);
            runtime.discovered = true;
        }
        std::string archive_url;
        {
            std::lock_guard lock(mutex_);
            auto& runtime = sync_runtime_.at(id);
            if (runtime.next_archive < runtime.archives.size())
                archive_url = runtime.archives[runtime.next_archive++];
        }
        if (archive_url.empty()) {
            finish_sync(id, "succeeded");
            return;
        }
        const std::string month = month_from_archive(archive_url);
        state.current_month = month;
        const bool mutable_month = month == utc_month(state.upper_bound_ms);
        const bool checkpointed = repository_.chesscom_month_checkpoint(state.username, month).has_value();
        if (!checkpointed || mutable_month) {
            const auto page = client_.fetch_month(state.username, month.substr(0, 4),
                                                  month.substr(5, 2), {}, cancellation);
            std::vector<ChessComArchiveEntry> entries;
            std::vector<import::ImportedGame> imported_games;
            entries.reserve(page.games.size());
            imported_games.reserve(page.games.size());
            for (const auto& game : page.games) {
                std::optional<ChessComArchiveEntry> entry;
                try {
                    entry = archive_entry(game, state.username, month, archive_url, now_ms(), true);
                } catch (const Error&) {
                    // Keep a malformed archive record from failing the whole bounded sync.
                    continue;
                }
                if (!entry || entry->end_time_ms < state.cutoff_ms ||
                    entry->end_time_ms > state.upper_bound_ms)
                    continue;
                imported_games.push_back(
                    importer_.from_pgn(entry->pgn, entry->canonical_url));
                entries.push_back(std::move(*entry));
            }
            static_cast<void>(repository_.bulk_add(std::move(imported_games)));
            state.games_indexed += persist_archive_chunks(repository_, entries);
            repository_.checkpoint_chesscom_month(
                {state.username, month, archive_url, entries.size(), now_ms()});
        }
        ++state.months_completed;
        state.updated_at_ms = now_ms();
        {
            std::lock_guard lock(mutex_);
            if (syncs_.at(id).status == "cancelled") return;
            syncs_.insert_or_assign(id, state);
            historical_queue_.push_back(id);
        }
        repository_.save_chesscom_sync_state(
            {"running", state.username, sync_cursor(state), month, state.months_completed,
             state.games_indexed, state.created_at_ms, state.updated_at_ms, ""});
        notify("sync", to_json(state));
        historical_condition_.notify_one();
    } catch (const import::ChessComClientError& error) {
        finish_sync(id, error.failure() == import::ChessComFailure::Cancelled ? "cancelled" : "failed",
                    error.failure() == import::ChessComFailure::Cancelled ? "cancelled" : "chesscom_request_failed",
                    error.what());
    } catch (const Error& error) {
        finish_sync(id, "failed", "sync_failed", error.what());
    }
}

void IngestManager::finish_sync(std::string id, std::string status, std::string code,
                                std::string error) {
    IngestSync state;
    {
        std::lock_guard lock(mutex_);
        const auto found = syncs_.find(id);
        if (found == syncs_.end()) return;
        if (found->second.status == "cancelled") status = "cancelled";
        found->second.status = std::move(status);
        found->second.code = std::move(code);
        found->second.error = std::move(error);
        found->second.updated_at_ms = now_ms();
        state = found->second;
        trim_history_unlocked();
    }
    const bool succeeded = state.status == "succeeded";
    repository_.save_chesscom_sync_state(
        {succeeded ? "succeeded" : (state.status == "cancelled" ? "paused" : "failed"),
         state.username, sync_cursor(state), state.current_month, state.months_completed,
         state.games_indexed, state.created_at_ms, state.updated_at_ms, state.error});
    if (auto configured = repository_.chesscom_profile(); configured && succeeded &&
        configured->normalized_username == state.username) {
        configured->archive_cursor = sync_cursor(state);
        configured->last_successful_sync_ms = state.updated_at_ms;
        configured->last_error.clear();
        repository_.save_chesscom_profile(*configured);
    }
    notify("sync", to_json(state));
}

} // namespace pct::app

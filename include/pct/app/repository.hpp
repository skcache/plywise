#pragma once

#include "pct/analysis/analyzer.hpp"
#include "pct/common/error.hpp"
#include "pct/common/json.hpp"
#include "pct/import/import_service.hpp"
#include "pct/storage/event_log.hpp"
#include "pct/training/training.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pct::app {

struct StoredGame {
    import::ImportedGame imported;
    std::optional<analysis::GameAnalysis> analysis;
    std::int64_t imported_at_ms{0};
    std::int64_t analyzed_at_ms{0};
    std::optional<analysis::GameAnalysis> shallow_analysis;
};

struct VariationNode {
    std::uint64_t id{0};
    std::int64_t parent_id{-1};
    std::string uci;
    std::string san;
    std::string fen;
    std::vector<std::uint64_t> children;
    bool operator==(const VariationNode&) const = default;
};

struct Variation {
    std::string id;
    std::string game_id;
    std::size_t root_ply{0};
    std::string root_position{"after"};
    std::string root_fen;
    std::uint64_t current_node_id{0};
    std::uint64_t next_node_id{1};
    std::string engine_configuration{"current-default"};
    std::int64_t updated_at_ms{0};
    std::map<std::uint64_t, VariationNode> nodes;
    bool operator==(const Variation&) const = default;
};

struct ReviewAttempt {
    std::uint64_t id{0};
    std::string game_id;
    std::size_t ply{0};
    std::string uci;
    bool accepted{false};
    std::int64_t attempted_at_ms{0};
};

enum class AddResult { Added, Duplicate };

struct BulkAddResult {
    std::size_t added{0};
    std::size_t duplicates{0};
    std::vector<std::string> added_game_ids;
    std::vector<std::string> duplicate_game_ids;
};

inline constexpr std::size_t bulk_game_import_limit = 1000;
inline constexpr std::size_t bulk_game_import_pgn_byte_limit = 64U * 1024U * 1024U;
inline constexpr std::size_t bulk_game_import_single_pgn_byte_limit = 10U * 1024U * 1024U;
inline constexpr std::size_t bulk_game_import_source_url_limit = 2048;

struct ChessComProfile {
    std::string original_username;
    std::string normalized_username;
    std::vector<std::string> selected_time_controls;
    std::string archive_cursor;
    std::int64_t last_successful_sync_ms{0};
    std::string last_error;
    bool operator==(const ChessComProfile&) const = default;
};

struct ChessComArchiveEntry {
    std::string game_id;
    std::string canonical_url;
    std::string pgn;
    std::string username;
    std::string month;
    std::string time_class;
    std::int64_t end_time_ms{0};
    std::int64_t fetched_at_ms{0};
    std::string source_url;
    bool operator==(const ChessComArchiveEntry&) const = default;
};

struct ChessComMonthCheckpoint {
    std::string username;
    std::string month;
    std::string source_url;
    std::size_t indexed_games{0};
    std::int64_t completed_at_ms{0};
    bool operator==(const ChessComMonthCheckpoint&) const = default;
};

struct ChessComSyncState {
    std::string status{"idle"};
    std::string username;
    std::string cursor;
    std::string current_month;
    std::size_t months_completed{0};
    std::size_t games_indexed{0};
    std::int64_t started_at_ms{0};
    std::int64_t updated_at_ms{0};
    std::string last_error;
    bool operator==(const ChessComSyncState&) const = default;
};

struct ChessComArchiveSearch {
    std::string username;
    std::string month;
    std::string time_class;
    std::int64_t ended_after_ms{0};
    std::int64_t ended_before_ms{0};
    std::size_t offset{0};
    std::size_t limit{50};
    bool include_pgn{false};
};

struct ChessComArchivePage {
    std::vector<ChessComArchiveEntry> entries;
    std::size_t next_offset{0};
    bool has_more{false};
};

inline constexpr std::size_t chesscom_archive_search_limit = 200;
inline constexpr std::size_t chesscom_archive_chunk_limit = 256;
inline constexpr std::size_t chesscom_archive_chunk_encoded_byte_limit = 8U * 1024U * 1024U;
inline constexpr std::size_t chesscom_profile_cursor_limit = 512;
inline constexpr std::size_t chesscom_profile_error_limit = 512;

[[nodiscard]] std::string normalize_chesscom_username(std::string_view username);
[[nodiscard]] bool valid_chesscom_month(std::string_view month);
[[nodiscard]] bool valid_chesscom_month_checkpoint(const ChessComMonthCheckpoint& checkpoint);

enum class OwnerKind { Local, Guest, Account };

class OwnerId final {
  public:
    [[nodiscard]] static OwnerId local();
    [[nodiscard]] static OwnerId guest(std::string id);
    [[nodiscard]] static OwnerId account(std::string id);

    [[nodiscard]] OwnerKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    bool operator==(const OwnerId&) const = default;

  private:
    OwnerId(OwnerKind kind, std::string id);

    OwnerKind kind_;
    std::string value_;
};

class IOwnedRepository {
  public:
    virtual ~IOwnedRepository() = default;
    [[nodiscard]] virtual const OwnerId& owner() const noexcept = 0;
};

class IGameRepository : public virtual IOwnedRepository {
  public:
    ~IGameRepository() override = default;

    // These operations are owner-scoped transaction boundaries. An adapter must commit the
    // immutable canonical game and its owner mapping together.
    [[nodiscard]] virtual AddResult add(const import::ImportedGame& imported) = 0;
    [[nodiscard]] virtual BulkAddResult
    bulk_add(std::vector<import::ImportedGame> imported_games) = 0;
    [[nodiscard]] virtual std::optional<StoredGame> get(std::string_view id) const = 0;
    [[nodiscard]] virtual std::vector<StoredGame> list() const = 0;
    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual std::size_t
    index_chesscom_archive_chunk(std::vector<ChessComArchiveEntry> entries) = 0;
    [[nodiscard]] virtual std::optional<ChessComArchiveEntry>
    chesscom_archive_entry(std::string_view game_id) const = 0;
    [[nodiscard]] virtual ChessComArchivePage
    search_chesscom_archive(const ChessComArchiveSearch& search = {}) const = 0;
    virtual void checkpoint_chesscom_month(ChessComMonthCheckpoint checkpoint) = 0;
    [[nodiscard]] virtual std::optional<ChessComMonthCheckpoint>
    chesscom_month_checkpoint(std::string_view username, std::string_view month) const = 0;
    [[nodiscard]] virtual std::vector<ChessComMonthCheckpoint>
    chesscom_month_checkpoints(std::string_view username = {}) const = 0;
};

class IAnalysisRepository : public virtual IOwnedRepository {
  public:
    ~IAnalysisRepository() override = default;

    // A completed analysis is persisted as one canonical review transaction. Browser engine
    // observations must be validated by C++ before reaching this boundary.
    virtual void save_analysis(const analysis::GameAnalysis& analysis) = 0;
    virtual void save_shallow_analysis(const analysis::GameAnalysis& analysis) = 0;
    [[nodiscard]] virtual Variation create_variation(std::string_view game_id,
                                                     std::size_t root_ply,
                                                     std::string root_position = "after") = 0;
    [[nodiscard]] virtual Variation extend_variation(std::string_view variation_id,
                                                     std::uint64_t node_id,
                                                     std::string_view uci) = 0;
    [[nodiscard]] virtual Variation set_variation_cursor(std::string_view variation_id,
                                                         std::uint64_t node_id) = 0;
    [[nodiscard]] virtual Variation reset_variation(std::string_view variation_id) = 0;
    [[nodiscard]] virtual std::optional<Variation>
    variation(std::string_view variation_id) const = 0;
    [[nodiscard]] virtual std::vector<Variation>
    variations(std::string_view game_id) const = 0;
    [[nodiscard]] virtual bool delete_variation(std::string_view variation_id) = 0;
    [[nodiscard]] virtual ReviewAttempt record_review_attempt(std::string_view game_id,
                                                              std::size_t ply,
                                                              std::string_view uci) = 0;
    [[nodiscard]] virtual std::vector<ReviewAttempt>
    review_attempts(std::string_view game_id) const = 0;
};

class IJobRepository : public virtual IOwnedRepository {
  public:
    ~IJobRepository() override = default;

    virtual void record_job_state(std::string game_id, std::string status) = 0;
    [[nodiscard]] virtual std::vector<std::string> recoverable_analysis_jobs() const = 0;
    virtual void set_background_paused(bool paused) = 0;
    [[nodiscard]] virtual bool background_paused() const = 0;
    [[nodiscard]] virtual json::Value create_batch(std::vector<std::string> game_ids,
                                                   std::size_t discovered,
                                                   std::size_t imported,
                                                   std::size_t duplicates,
                                                   std::size_t failed) = 0;
    [[nodiscard]] virtual json::Value batches() const = 0;
};

class IIntelligenceRepository : public virtual IOwnedRepository {
  public:
    ~IIntelligenceRepository() override = default;

    [[nodiscard]] virtual std::vector<training::Drill> drills(std::int64_t now_ms) const = 0;
    [[nodiscard]] virtual std::optional<training::Drill> drill(std::string_view id) const = 0;
    [[nodiscard]] virtual bool add_validated_drill(training::Drill drill) = 0;
    [[nodiscard]] virtual training::DrillAttempt
    record_attempt(std::string_view drill_id, std::string move,
                   std::uint64_t response_time_ms, int hint_level,
                   std::int64_t attempted_at_ms) = 0;
    [[nodiscard]] virtual training::Drill advance_hint(std::string_view drill_id,
                                                       std::int64_t now_ms) = 0;
    [[nodiscard]] virtual training::Drill begin_drill_session(std::string_view drill_id,
                                                              std::int64_t now_ms) = 0;
    [[nodiscard]] virtual training::Profile profile() const = 0;
    [[nodiscard]] virtual std::vector<training::Recommendation> recommendations() = 0;
    virtual void complete_resource(std::string resource_id,
                                   std::int64_t completed_at_ms) = 0;
};

class ISettingsRepository : public virtual IOwnedRepository {
  public:
    ~ISettingsRepository() override = default;

    virtual void save_chesscom_profile(ChessComProfile profile) = 0;
    [[nodiscard]] virtual std::optional<ChessComProfile> chesscom_profile() const = 0;
    virtual void save_player_identity(training::PlayerIdentity identity) = 0;
    [[nodiscard]] virtual std::optional<training::PlayerIdentity> player_identity() const = 0;
    virtual void save_chesscom_sync_state(ChessComSyncState state) = 0;
    [[nodiscard]] virtual ChessComSyncState chesscom_sync_state() const = 0;
};

class IRepositoryMaintenance : public virtual IOwnedRepository {
  public:
    ~IRepositoryMaintenance() override = default;

    [[nodiscard]] virtual std::filesystem::path create_snapshot() = 0;
    [[nodiscard]] virtual std::size_t compact_storage() = 0;
};

class IRepository : public IGameRepository,
                    public IAnalysisRepository,
                    public IJobRepository,
                    public IIntelligenceRepository,
                    public ISettingsRepository,
                    public IRepositoryMaintenance {
  public:
    ~IRepository() override = default;
};

// Filesystem-backed local adapter. It intentionally binds one synthetic local owner while hosted
// account and guest adapters are implemented separately.
class EventLogRepository final : public IRepository {
  public:
    explicit EventLogRepository(storage::EventLog& log);

    [[nodiscard]] const OwnerId& owner() const noexcept override { return owner_; }
    [[nodiscard]] AddResult add(const import::ImportedGame& imported) override;
    [[nodiscard]] BulkAddResult
    bulk_add(std::vector<import::ImportedGame> imported_games) override;
    void save_analysis(const analysis::GameAnalysis& analysis) override;
    void save_shallow_analysis(const analysis::GameAnalysis& analysis) override;
    [[nodiscard]] std::optional<StoredGame> get(std::string_view id) const override;
    [[nodiscard]] std::vector<StoredGame> list() const override;
    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] std::vector<training::Drill>
    drills(std::int64_t now_ms) const override;
    [[nodiscard]] std::optional<training::Drill>
    drill(std::string_view id) const override;
    [[nodiscard]] bool add_validated_drill(training::Drill drill) override;
    [[nodiscard]] training::DrillAttempt record_attempt(std::string_view drill_id,
                                                        std::string move,
                                                        std::uint64_t response_time_ms,
                                                        int hint_level,
                                                        std::int64_t attempted_at_ms) override;
    [[nodiscard]] training::Drill advance_hint(std::string_view drill_id,
                                               std::int64_t now_ms) override;
    [[nodiscard]] training::Drill begin_drill_session(std::string_view drill_id,
                                                      std::int64_t now_ms) override;
    [[nodiscard]] training::Profile profile() const override;
    [[nodiscard]] std::vector<training::Recommendation> recommendations() override;
    void complete_resource(std::string resource_id,
                           std::int64_t completed_at_ms) override;
    [[nodiscard]] std::filesystem::path create_snapshot() override;
    [[nodiscard]] std::size_t compact_storage() override;
    void record_job_state(std::string game_id, std::string status) override;
    [[nodiscard]] std::vector<std::string> recoverable_analysis_jobs() const override;
    void set_background_paused(bool paused) override;
    [[nodiscard]] bool background_paused() const override;
    [[nodiscard]] json::Value create_batch(std::vector<std::string> game_ids,
                                           std::size_t discovered, std::size_t imported,
                                           std::size_t duplicates,
                                           std::size_t failed) override;
    [[nodiscard]] json::Value batches() const override;
    void save_chesscom_profile(ChessComProfile profile) override;
    [[nodiscard]] std::optional<ChessComProfile> chesscom_profile() const override;
    void save_player_identity(training::PlayerIdentity identity) override;
    [[nodiscard]] std::optional<training::PlayerIdentity> player_identity() const override;
    [[nodiscard]] std::size_t
    index_chesscom_archive_chunk(std::vector<ChessComArchiveEntry> entries) override;
    [[nodiscard]] std::optional<ChessComArchiveEntry>
    chesscom_archive_entry(std::string_view game_id) const override;
    [[nodiscard]] ChessComArchivePage
    search_chesscom_archive(const ChessComArchiveSearch& search = {}) const override;
    void checkpoint_chesscom_month(ChessComMonthCheckpoint checkpoint) override;
    [[nodiscard]] std::optional<ChessComMonthCheckpoint>
    chesscom_month_checkpoint(std::string_view username,
                              std::string_view month) const override;
    [[nodiscard]] std::vector<ChessComMonthCheckpoint>
    chesscom_month_checkpoints(std::string_view username = {}) const override;
    void save_chesscom_sync_state(ChessComSyncState state) override;
    [[nodiscard]] ChessComSyncState chesscom_sync_state() const override;
    [[nodiscard]] Variation create_variation(std::string_view game_id, std::size_t root_ply,
                                             std::string root_position = "after") override;
    [[nodiscard]] Variation extend_variation(std::string_view variation_id,
                                             std::uint64_t node_id,
                                             std::string_view uci) override;
    [[nodiscard]] Variation set_variation_cursor(std::string_view variation_id,
                                                 std::uint64_t node_id) override;
    [[nodiscard]] Variation reset_variation(std::string_view variation_id) override;
    [[nodiscard]] std::optional<Variation>
    variation(std::string_view variation_id) const override;
    [[nodiscard]] std::vector<Variation>
    variations(std::string_view game_id) const override;
    [[nodiscard]] bool delete_variation(std::string_view variation_id) override;
    [[nodiscard]] ReviewAttempt record_review_attempt(std::string_view game_id,
                                                      std::size_t ply,
                                                      std::string_view uci) override;
    [[nodiscard]] std::vector<ReviewAttempt>
    review_attempts(std::string_view game_id) const override;

  private:
    OwnerId owner_{OwnerId::local()};
    storage::EventLog& log_;
    mutable std::mutex mutex_;
    std::map<std::string, StoredGame> games_;
    std::map<std::string, training::Drill> drills_;
    std::map<std::string, std::int64_t> resource_completions_;
    std::set<std::string> recommended_resources_;
    std::uint64_t next_attempt_id_{1};
    std::map<std::string, std::string> analysis_job_states_;
    bool background_paused_{false};
    std::map<std::string, json::Value> batches_;
    std::uint64_t next_batch_id_{1};
    std::optional<ChessComProfile> chesscom_profile_;
    std::optional<training::PlayerIdentity> player_identity_;
    std::map<std::string, ChessComArchiveEntry> chesscom_archive_;
    std::map<std::string, ChessComMonthCheckpoint> chesscom_checkpoints_;
    ChessComSyncState chesscom_sync_state_;
    std::map<std::string, Variation> variations_;
    std::uint64_t next_variation_id_{1};
    std::vector<ReviewAttempt> review_attempts_;
    std::uint64_t next_review_attempt_id_{1};
    std::uint64_t projection_event_id_{0};
    bool projection_contiguous_{true};

    void replay();
    void rebuild_indexes() const;
    void note_applied_event(const storage::Event& event);
    [[nodiscard]] AddResult add_unlocked(const import::ImportedGame& imported,
                                         bool rebuild_indexes_after_add);
    [[nodiscard]] training::Profile profile_unlocked() const;
};

// Transitional source-compatible name for the verified local adapter.
using Repository = EventLogRepository;

[[nodiscard]] json::Value to_json(const chess::Game& game);
[[nodiscard]] json::Value to_json(const analysis::GameAnalysis& analysis);
[[nodiscard]] json::Value to_json(const StoredGame& game, bool include_pgn = false);
[[nodiscard]] analysis::GameAnalysis analysis_from_json(const json::Value& value);
[[nodiscard]] json::Value to_json(const Variation& variation);
[[nodiscard]] json::Value to_json(const ReviewAttempt& attempt);

} // namespace pct::app

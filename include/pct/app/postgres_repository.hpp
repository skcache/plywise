#pragma once

#include "pct/app/repository.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pct::app {

// PostgreSQL adapter for the owner-scoped hosted review path. The local event-log adapter remains
// the default; this adapter is compiled when libpq is available and never accepts a synthetic
// local owner.
class PostgresRepository final : public IRepository {
  public:
    PostgresRepository(std::string connection_string, OwnerId owner);
    ~PostgresRepository() override;

    PostgresRepository(const PostgresRepository&) = delete;
    PostgresRepository& operator=(const PostgresRepository&) = delete;

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
    struct Impl;

    OwnerId owner_;
    std::unique_ptr<Impl> impl_;
    mutable std::mutex mutex_;
    bool background_paused_{false};
    std::map<std::string, json::Value> batches_;

    void save_analysis_impl(const analysis::GameAnalysis& analysis,
                            std::string_view compatibility_key);
};

} // namespace pct::app

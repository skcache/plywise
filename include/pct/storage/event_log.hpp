#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pct::storage {

inline constexpr std::uint16_t current_schema_version = 2;
enum class CompactionStage { TemporaryWritten, Validated, BeforeReplace, Replaced };
enum class AppendStage { BeforeWrite, AfterPartialWrite, BeforeSync, AfterSync };

enum class EventType : std::uint16_t {
    GameImported = 1,
    GameParsed = 2,
    PositionAnalyzed = 3,
    MistakeDetected = 4,
    MistakeClassified = 5,
    ExplanationCreated = 6,
    AnalysisCompleted = 7,
    DrillCreated = 8,
    DrillAttempted = 9,
    ResourceRecommended = 10,
    ResourceCompleted = 11,
    RatingObserved = 12,
    ProfileSnapshotCreated = 13,
    SchemaMigrated = 14,
    LogCompacted = 15,
    BatchCreated = 16,
    BatchStateChanged = 17,
    AnalysisJobStateChanged = 18,
    DrillSessionUpdated = 19,
    ShallowAnalysisCompleted = 20,
    ChessComProfileUpdated = 21,
    ChessComArchiveChunkIndexed = 22,
    ChessComMonthCheckpointed = 23,
    ChessComSyncStateChanged = 24,
    VariationSaved = 25,
    VariationDeleted = 26,
    ReviewAttempted = 27,
    PlayerIdentityUpdated = 28,
};

struct Event {
    std::uint16_t schema_version{1};
    EventType type{EventType::GameImported};
    std::uint64_t id{0};
    std::int64_t timestamp_ms{0};
    std::string payload;
};

struct Corruption {
    std::uint64_t offset{0};
    std::string reason;
};

struct ReplayResult {
    std::vector<Event> events;
    std::vector<Corruption> corruptions;
    std::uint64_t valid_prefix_bytes{0};
    bool truncated_tail{false};
};

class EventLog {
  public:
    explicit EventLog(std::filesystem::path path);

    [[nodiscard]] Event
    append(EventType type, std::string payload,
           std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());
    [[nodiscard]] ReplayResult replay() const;
    [[nodiscard]] bool recover_trailing_record();
    [[nodiscard]] std::size_t compact(
        const std::function<void(CompactionStage)>& stage_hook = {});
    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }
    void set_append_fault_hook(std::function<void(AppendStage)> hook);

    [[nodiscard]] static std::vector<std::byte> serialize(const Event& event);
    [[nodiscard]] static std::uint32_t checksum(const std::byte* data, std::size_t size);

  private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::uint64_t next_id_{1};
    std::function<void(AppendStage)> append_fault_hook_;

    [[nodiscard]] ReplayResult replay_unlocked() const;
};

[[nodiscard]] std::string_view name(EventType type);

} // namespace pct::storage

#include "pct/storage/event_log.hpp"

#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>

namespace pct::storage {
namespace {

constexpr std::uint32_t magic = 0x45544350U; // PCTE in little-endian byte order.
constexpr std::size_t header_size = 32;
constexpr std::size_t checksum_size = 4;
constexpr std::size_t minimum_record_size = header_size + checksum_size;
constexpr std::size_t maximum_record_size = 16 * 1024 * 1024;

void create_private_directories(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> missing;
    for (auto current = path; !current.empty() && !std::filesystem::exists(current);
         current = current.parent_path()) {
        missing.push_back(current);
        if (current == current.parent_path())
            break;
    }
    for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator) {
        if (::mkdir(iterator->c_str(), 0700) != 0 && errno != EEXIST)
            throw Error(ErrorCode::IoError,
                        std::string("failed to create event-log directory: ") +
                            std::strerror(errno));
    }
}

void fsync_parent_directory(const std::filesystem::path& path) {
    const std::filesystem::path parent =
        path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
    const int descriptor = open(parent.c_str(), O_RDONLY);
    if (descriptor < 0)
        throw Error(ErrorCode::IoError,
                    std::string("failed to open event-log directory: ") + std::strerror(errno));
    if (fsync(descriptor) != 0) {
        const int saved_errno = errno;
        close(descriptor);
        throw Error(ErrorCode::IoError,
                    std::string("failed to sync event-log directory: ") +
                        std::strerror(saved_errno));
    }
    close(descriptor);
}

template <typename T> void append_little_endian(std::vector<std::byte>& output, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned encoded = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>((encoded >> (index * 8U)) & 0xffU));
    }
}

template <typename T> T read_little_endian(const std::byte* data) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        const Unsigned byte =
            static_cast<Unsigned>(std::to_integer<unsigned>(data[index]));
        value = static_cast<Unsigned>(
            value | static_cast<Unsigned>(byte << (index * 8U)));
    }
    return static_cast<T>(value);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto length = input.tellg();
    if (length < 0)
        throw Error(ErrorCode::IoError, "failed to determine event-log size");
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!data.empty())
        input.read(reinterpret_cast<char*>(data.data()), length);
    if (!input && !data.empty())
        throw Error(ErrorCode::IoError, "failed to read event log");
    return data;
}

std::size_t find_magic(const std::vector<std::byte>& data, std::size_t offset) {
    const std::array<std::byte, 4> bytes{
        static_cast<std::byte>(magic & 0xffU),
        static_cast<std::byte>((magic >> 8U) & 0xffU),
        static_cast<std::byte>((magic >> 16U) & 0xffU),
        static_cast<std::byte>((magic >> 24U) & 0xffU),
    };
    for (std::size_t index = offset; index + bytes.size() <= data.size(); ++index) {
        if (std::equal(bytes.begin(), bytes.end(),
                       data.begin() + static_cast<std::ptrdiff_t>(index))) {
            return index;
        }
    }
    return data.size();
}

} // namespace

EventLog::EventLog(std::filesystem::path path) : path_(std::move(path)) {
    if (path_.has_parent_path())
        create_private_directories(path_.parent_path());
    const int descriptor = open(path_.c_str(), O_WRONLY | O_CREAT, 0600);
    if (descriptor < 0)
        throw Error(ErrorCode::IoError,
                    std::string("failed to create event log: ") + std::strerror(errno));
    if (fchmod(descriptor, 0600) != 0) {
        const int saved_errno = errno;
        close(descriptor);
        throw Error(ErrorCode::IoError,
                    std::string("failed to secure event log: ") + std::strerror(saved_errno));
    }
    close(descriptor);
    const ReplayResult state = replay_unlocked();
    for (const Event& event : state.events)
        next_id_ = std::max(next_id_, event.id + 1);
}

std::uint32_t EventLog::checksum(const std::byte* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= std::to_integer<std::uint8_t>(data[index]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::vector<std::byte> EventLog::serialize(const Event& event) {
    if (event.payload.size() > maximum_record_size - minimum_record_size) {
        throw Error(ErrorCode::InvalidArgument, "event payload exceeds the record limit");
    }
    const auto record_size = static_cast<std::uint32_t>(minimum_record_size + event.payload.size());
    std::vector<std::byte> output;
    output.reserve(record_size);
    append_little_endian(output, magic);
    append_little_endian(output, event.schema_version);
    append_little_endian(output, static_cast<std::uint16_t>(event.type));
    append_little_endian(output, record_size);
    append_little_endian(output, event.id);
    append_little_endian(output, event.timestamp_ms);
    append_little_endian(output, static_cast<std::uint32_t>(event.payload.size()));
    for (const char character : event.payload)
        output.push_back(static_cast<std::byte>(character));
    append_little_endian(output, checksum(output.data(), output.size()));
    return output;
}

Event EventLog::append(EventType type, std::string payload,
                       std::chrono::system_clock::time_point timestamp) {
    std::lock_guard lock(mutex_);
    Event event{
        current_schema_version, type, next_id_,
        std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count(),
        std::move(payload)};
    const std::vector<std::byte> record = serialize(event);
    const int descriptor = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (descriptor < 0) {
        throw Error(ErrorCode::IoError,
                    std::string("failed to open event log: ") + std::strerror(errno));
    }
    try {
        if (append_fault_hook_)
            append_fault_hook_(AppendStage::BeforeWrite);
        std::size_t offset = 0;
        const std::size_t first_target = append_fault_hook_ ? record.size() / 2 : record.size();
        while (offset < first_target) {
            const ssize_t written =
                write(descriptor, record.data() + offset, first_target - offset);
            if (written < 0)
                throw Error(ErrorCode::IoError,
                            std::string("failed to append event: ") + std::strerror(errno));
            offset += static_cast<std::size_t>(written);
        }
        if (append_fault_hook_)
            append_fault_hook_(AppendStage::AfterPartialWrite);
        while (offset < record.size()) {
            const ssize_t written = write(descriptor, record.data() + offset, record.size() - offset);
            if (written < 0)
                throw Error(ErrorCode::IoError,
                            std::string("failed to append event: ") + std::strerror(errno));
            offset += static_cast<std::size_t>(written);
        }
        if (append_fault_hook_)
            append_fault_hook_(AppendStage::BeforeSync);
        if (fsync(descriptor) != 0)
            throw Error(ErrorCode::IoError,
                        std::string("failed to sync event log: ") + std::strerror(errno));
        ++next_id_;
        if (append_fault_hook_)
            append_fault_hook_(AppendStage::AfterSync);
        close(descriptor);
    } catch (...) {
        close(descriptor);
        throw;
    }
    return event;
}

void EventLog::set_append_fault_hook(std::function<void(AppendStage)> hook) {
    std::lock_guard lock(mutex_);
    append_fault_hook_ = std::move(hook);
}

ReplayResult EventLog::replay() const {
    std::lock_guard lock(mutex_);
    return replay_unlocked();
}

ReplayResult EventLog::replay_unlocked() const {
    const std::vector<std::byte> data = read_file(path_);
    ReplayResult result;
    std::size_t offset = 0;
    bool prefix_intact = true;
    while (offset < data.size()) {
        if (data.size() - offset < minimum_record_size) {
            result.truncated_tail = true;
            break;
        }
        if (read_little_endian<std::uint32_t>(data.data() + offset) != magic) {
            result.corruptions.push_back(Corruption{offset, "record magic does not match"});
            prefix_intact = false;
            offset = find_magic(data, offset + 1);
            continue;
        }
        const std::uint16_t schema = read_little_endian<std::uint16_t>(data.data() + offset + 4);
        const auto type = read_little_endian<std::uint16_t>(data.data() + offset + 6);
        const std::uint32_t record_size =
            read_little_endian<std::uint32_t>(data.data() + offset + 8);
        const std::uint32_t payload_size =
            read_little_endian<std::uint32_t>(data.data() + offset + 28);
        if (record_size < minimum_record_size || record_size > maximum_record_size ||
            payload_size != record_size - minimum_record_size) {
            result.corruptions.push_back(Corruption{offset, "record length is invalid"});
            prefix_intact = false;
            offset = find_magic(data, offset + 1);
            continue;
        }
        if (data.size() - offset < record_size) {
            result.truncated_tail = true;
            break;
        }
        const std::uint32_t expected =
            read_little_endian<std::uint32_t>(data.data() + offset + record_size - checksum_size);
        const std::uint32_t actual = checksum(data.data() + offset, record_size - checksum_size);
        if (actual != expected) {
            result.corruptions.push_back(Corruption{offset, "record checksum does not match"});
            prefix_intact = false;
            offset += record_size;
            continue;
        }
        if (schema == 0 || schema > current_schema_version) {
            result.corruptions.push_back(Corruption{offset, "record schema is unsupported"});
            prefix_intact = false;
            offset += record_size;
            continue;
        }
        Event event;
        event.schema_version = schema;
        event.type = static_cast<EventType>(type);
        event.id = read_little_endian<std::uint64_t>(data.data() + offset + 12);
        event.timestamp_ms = read_little_endian<std::int64_t>(data.data() + offset + 20);
        event.payload.assign(reinterpret_cast<const char*>(data.data() + offset + header_size),
                             payload_size);
        result.events.push_back(std::move(event));
        offset += record_size;
        if (prefix_intact)
            result.valid_prefix_bytes = offset;
    }
    return result;
}

bool EventLog::recover_trailing_record() {
    std::lock_guard lock(mutex_);
    const ReplayResult result = replay_unlocked();
    if (!result.truncated_tail || !result.corruptions.empty())
        return false;
    std::filesystem::resize_file(path_, result.valid_prefix_bytes);
    return true;
}

std::size_t EventLog::compact(const std::function<void(CompactionStage)>& stage_hook) {
    std::lock_guard lock(mutex_);
    ReplayResult state = replay_unlocked();
    if (!state.corruptions.empty() || state.truncated_tail)
        throw Error(ErrorCode::IoError, "cannot compact an invalid event log");
    const std::size_t source_event_count = state.events.size();
    const bool had_legacy_schema = std::any_of(
        state.events.begin(), state.events.end(),
        [](const Event& event) { return event.schema_version < current_schema_version; });
    std::vector<Event> retained;
    std::set<std::string> job_games;
    std::set<std::string> completed_resources;
    std::set<std::string> drill_sessions;
    std::set<std::string> completed_analyses;
    std::set<std::string> shallow_analyses;
    std::set<std::string> chesscom_months;
    std::set<std::string> variations;
    bool kept_snapshot = false;
    bool kept_queue_state = false;
    bool kept_migration = false;
    bool kept_chesscom_profile = false;
    bool kept_player_identity = false;
    bool kept_chesscom_sync_state = false;
    for (auto iterator = state.events.rbegin(); iterator != state.events.rend(); ++iterator) {
        bool keep = true;
        try {
            if (iterator->type == EventType::AnalysisCompleted) {
                const std::string game_id =
                    json::parse(iterator->payload).at("game_id").as_string();
                completed_analyses.insert(game_id);
            } else if (iterator->type == EventType::ShallowAnalysisCompleted) {
                const std::string game_id =
                    json::parse(iterator->payload).at("game_id").as_string();
                keep = !completed_analyses.contains(game_id) &&
                       shallow_analyses.insert(game_id).second;
            } else if (iterator->type == EventType::AnalysisJobStateChanged) {
                const std::string game_id = json::parse(iterator->payload).at("game_id").as_string();
                keep = job_games.insert(game_id).second;
            } else if (iterator->type == EventType::ResourceCompleted) {
                const std::string resource_id =
                    json::parse(iterator->payload).at("resource_id").as_string();
                keep = completed_resources.insert(resource_id).second;
            } else if (iterator->type == EventType::DrillSessionUpdated) {
                const std::string drill_id =
                    json::parse(iterator->payload).at("drill_id").as_string();
                keep = drill_sessions.insert(drill_id).second;
            } else if (iterator->type == EventType::ProfileSnapshotCreated) {
                keep = !kept_snapshot;
                kept_snapshot = true;
            } else if (iterator->type == EventType::BatchStateChanged) {
                keep = !kept_queue_state;
                kept_queue_state = true;
            } else if (iterator->type == EventType::SchemaMigrated) {
                keep = !kept_migration;
                kept_migration = true;
            } else if (iterator->type == EventType::ChessComProfileUpdated) {
                keep = !kept_chesscom_profile;
                kept_chesscom_profile = true;
            } else if (iterator->type == EventType::PlayerIdentityUpdated) {
                keep = !kept_player_identity;
                kept_player_identity = true;
            } else if (iterator->type == EventType::ChessComSyncStateChanged) {
                keep = !kept_chesscom_sync_state;
                kept_chesscom_sync_state = true;
            } else if (iterator->type == EventType::ChessComMonthCheckpointed) {
                const auto payload = json::parse(iterator->payload);
                const std::string key = payload.at("username").as_string() + "\n" +
                                        payload.at("month").as_string();
                keep = chesscom_months.insert(key).second;
            } else if (iterator->type == EventType::VariationSaved ||
                       iterator->type == EventType::VariationDeleted) {
                const std::string variation_id =
                    json::parse(iterator->payload).at("id").as_string();
                keep = variations.insert(variation_id).second;
            } else if (iterator->type == EventType::LogCompacted) {
                keep = false;
            }
        } catch (const std::exception&) {
            keep = true;
        }
        if (keep)
            retained.push_back(*iterator);
    }
    std::reverse(retained.begin(), retained.end());
    state.events = std::move(retained);
    std::uint64_t next_new_id = next_id_;
    if (had_legacy_schema) {
        state.events.push_back(Event{current_schema_version, EventType::SchemaMigrated,
                                     next_new_id++,
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count(),
                                     "{\"from_schema\":1,\"to_schema\":" +
                                         std::to_string(current_schema_version) + "}"});
    }
    Event compacted{current_schema_version, EventType::LogCompacted, next_new_id,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count(),
                    "{\"source_events\":" + std::to_string(source_event_count) +
                        ",\"retained_events\":" + std::to_string(state.events.size()) + "}"};
    state.events.push_back(compacted);
    const std::filesystem::path temporary = path_.string() + ".compact.tmp";
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0)
        throw Error(ErrorCode::IoError, "failed to create compacted event log");
    if (fchmod(descriptor, 0600) != 0) {
        close(descriptor);
        std::filesystem::remove(temporary);
        throw Error(ErrorCode::IoError, "failed to secure compacted event log");
    }
    try {
        for (Event event : state.events) {
            event.schema_version = current_schema_version;
            const auto record = serialize(event);
            std::size_t offset = 0;
            while (offset < record.size()) {
                const ssize_t written = write(descriptor, record.data() + offset, record.size() - offset);
                if (written < 0)
                    throw Error(ErrorCode::IoError, "failed to write compacted event log");
                offset += static_cast<std::size_t>(written);
            }
        }
        if (fsync(descriptor) != 0)
            throw Error(ErrorCode::IoError, "failed to sync compacted event log");
        close(descriptor);
    } catch (...) {
        close(descriptor);
        std::filesystem::remove(temporary);
        throw;
    }
    if (stage_hook)
        stage_hook(CompactionStage::TemporaryWritten);
    EventLog validation(temporary);
    const ReplayResult verified = validation.replay();
    if (!verified.corruptions.empty() || verified.truncated_tail ||
        verified.events.size() != state.events.size()) {
        std::filesystem::remove(temporary);
        throw Error(ErrorCode::IoError, "compacted event log failed validation");
    }
    if (stage_hook)
        stage_hook(CompactionStage::Validated);
    if (stage_hook)
        stage_hook(CompactionStage::BeforeReplace);
    std::filesystem::rename(temporary, path_);
    fsync_parent_directory(path_);
    next_id_ = next_new_id + 1;
    if (stage_hook)
        stage_hook(CompactionStage::Replaced);
    return state.events.size();
}

std::string_view name(EventType type) {
    switch (type) {
    case EventType::GameImported:
        return "GameImported";
    case EventType::GameParsed:
        return "GameParsed";
    case EventType::PositionAnalyzed:
        return "PositionAnalyzed";
    case EventType::MistakeDetected:
        return "MistakeDetected";
    case EventType::MistakeClassified:
        return "MistakeClassified";
    case EventType::ExplanationCreated:
        return "ExplanationCreated";
    case EventType::AnalysisCompleted:
        return "AnalysisCompleted";
    case EventType::DrillCreated: return "DrillCreated";
    case EventType::DrillAttempted: return "DrillAttempted";
    case EventType::ResourceRecommended: return "ResourceRecommended";
    case EventType::ResourceCompleted: return "ResourceCompleted";
    case EventType::RatingObserved: return "RatingObserved";
    case EventType::ProfileSnapshotCreated: return "ProfileSnapshotCreated";
    case EventType::SchemaMigrated: return "SchemaMigrated";
    case EventType::LogCompacted: return "LogCompacted";
    case EventType::BatchCreated: return "BatchCreated";
    case EventType::BatchStateChanged: return "BatchStateChanged";
    case EventType::AnalysisJobStateChanged: return "AnalysisJobStateChanged";
    case EventType::DrillSessionUpdated: return "DrillSessionUpdated";
    case EventType::ShallowAnalysisCompleted: return "ShallowAnalysisCompleted";
    case EventType::ChessComProfileUpdated: return "ChessComProfileUpdated";
    case EventType::ChessComArchiveChunkIndexed: return "ChessComArchiveChunkIndexed";
    case EventType::ChessComMonthCheckpointed: return "ChessComMonthCheckpointed";
    case EventType::ChessComSyncStateChanged: return "ChessComSyncStateChanged";
    case EventType::VariationSaved: return "VariationSaved";
    case EventType::VariationDeleted: return "VariationDeleted";
    case EventType::ReviewAttempted: return "ReviewAttempted";
    case EventType::PlayerIdentityUpdated: return "PlayerIdentityUpdated";
    }
    return "Unknown";
}

} // namespace pct::storage

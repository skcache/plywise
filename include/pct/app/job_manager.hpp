#pragma once

#include "pct/analysis/analyzer.hpp"
#include "pct/app/repository.hpp"

#include <condition_variable>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pct::app {

enum class JobStatus { Queued, Running, Complete, Failed, Cancelled };

struct JobManagerOptions {
    std::size_t workers{1};
    std::size_t max_queued{256};
    std::size_t retry_limit{1};
};

struct AnalysisJob {
    std::uint64_t id{0};
    std::string game_id;
    JobStatus status{JobStatus::Queued};
    analysis::Progress progress;
    std::string error;
    CancellationSource cancellation;
};

using JobObserver = std::function<void(const AnalysisJob&)>;

class JobManager {
  public:
    using ObserverId = std::uint64_t;

    JobManager(IRepository& repository, analysis::Analyzer& analyzer,
               JobManagerOptions options = {});
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    [[nodiscard]] AnalysisJob start(
        std::string game_id,
        engine::AnalysisPriority priority = engine::AnalysisPriority::Interactive);
    [[nodiscard]] std::vector<AnalysisJob> start_batch(
        const std::vector<std::string>& game_ids);
    [[nodiscard]] bool cancel(std::uint64_t job_id);
    [[nodiscard]] std::optional<AnalysisJob> get(std::uint64_t job_id) const;
    [[nodiscard]] std::vector<AnalysisJob> list() const;
    void pause();
    void resume();
    [[nodiscard]] bool paused() const;
    [[nodiscard]] std::size_t cache_hits() const { return analyzer_.cache_hits(); }
    [[nodiscard]] std::size_t cache_misses() const { return analyzer_.cache_misses(); }
    [[nodiscard]] std::size_t cache_evictions() const { return analyzer_.cache_evictions(); }
    [[nodiscard]] std::size_t cache_size() const { return analyzer_.cache_size(); }
    [[nodiscard]] std::size_t cache_capacity() const { return analyzer_.cache_capacity(); }
    [[nodiscard]] std::size_t worker_count() const noexcept { return options_.workers; }
    [[nodiscard]] std::size_t queued_count() const;
    [[nodiscard]] engine::AnalysisResult analyze_variation_position(std::string_view fen);
    [[nodiscard]] std::size_t queue_capacity() const noexcept { return options_.max_queued; }
    void set_observer(JobObserver observer);
    [[nodiscard]] ObserverId add_observer(JobObserver observer);
    void remove_observer(ObserverId observer_id);

  private:
    IRepository& repository_;
    analysis::Analyzer& analyzer_;
    JobManagerOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::condition_variable observer_condition_;
    std::map<std::uint64_t, AnalysisJob> jobs_;
    struct Task {
        std::uint64_t job_id{0};
        bool deep{false};
        engine::AnalysisPriority priority{engine::AnalysisPriority::CurrentGame};
        std::size_t attempts{0};
        CancellationSource cancellation;
    };
    struct ActiveTask {
        engine::AnalysisPriority priority{engine::AnalysisPriority::CurrentGame};
        CancellationSource cancellation;
    };
    std::array<std::deque<Task>, 3> queues_;
    std::array<std::size_t, 3> active_by_priority_{};
    std::map<std::uint64_t, ActiveTask> active_tasks_;
    std::map<std::uint64_t, engine::AnalysisPriority> requested_priorities_;
    std::uint64_t next_id_{1};
    JobObserver observer_;
    std::map<ObserverId, JobObserver> additional_observers_;
    ObserverId next_observer_id_{1};
    std::size_t observer_calls_{0};
    std::vector<std::thread> workers_;
    CancellationSource worker_cancellation_;
    bool paused_{false};

    [[nodiscard]] std::vector<AnalysisJob>
    start_with_priority(const std::vector<std::string>& game_ids,
                        engine::AnalysisPriority priority);
    [[nodiscard]] std::size_t queued_count_unlocked() const;
    [[nodiscard]] bool runnable_unlocked() const;
    void enqueue_unlocked(Task task);
    void promote_unlocked(std::uint64_t job_id, engine::AnalysisPriority priority);
    void preempt_historical_unlocked();
    [[nodiscard]] Task take_unlocked();
    void finish_task(std::uint64_t job_id, engine::AnalysisPriority priority);
    void work(CancellationToken stop_token);
    void notify(const AnalysisJob& job);
};

[[nodiscard]] std::string_view name(JobStatus status);
[[nodiscard]] json::Value to_json(const AnalysisJob& job);

} // namespace pct::app

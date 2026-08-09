#include "pct/engine/pool.hpp"

#include "pct/common/error.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pct::engine {
namespace {

std::size_t priority_index(AnalysisPriority priority) {
    return static_cast<std::size_t>(priority);
}

} // namespace

class EnginePool::State {
  public:
    struct Work {
        AnalysisRequest request;
        CancellationSource cancellation;
        std::promise<AnalysisResult> promise;
        std::chrono::steady_clock::time_point queued_at;
        std::size_t attempts{0};
    };

    State(EngineFactory engine_factory, EnginePoolOptions pool_options)
        : factory(std::move(engine_factory)), options(pool_options) {
        if (!factory)
            throw Error(ErrorCode::InvalidArgument, "engine pool requires a worker factory");
        if (options.workers == 0 || options.max_pending == 0)
            throw Error(ErrorCode::InvalidArgument, "engine pool bounds must be positive");
        threads.reserve(options.workers);
        for (std::size_t index = 0; index < options.workers; ++index)
            threads.emplace_back([this, index] { worker(index); });
    }

    ~State() {
        std::array<std::deque<std::shared_ptr<Work>>, 3> abandoned;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            abandoned.swap(queues);
        }
        for (auto& queue : abandoned) {
            for (const auto& work : queue) {
                work->cancellation.request_stop();
                try {
                    throw Error(ErrorCode::EngineError, "engine pool is shutting down");
                } catch (...) {
                    work->promise.set_exception(std::current_exception());
                }
            }
        }
        condition.notify_all();
        for (auto& thread : threads)
            if (thread.joinable())
                thread.join();
    }

    [[nodiscard]] std::size_t pending_unlocked() const {
        std::size_t count = active;
        for (const auto& queue : queues)
            count += queue.size();
        return count;
    }

    [[nodiscard]] std::shared_ptr<Work> take() {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] {
            const std::size_t historical_limit = options.workers == 1 ? 1 : options.workers - 1;
            return stopping || !queues[0].empty() || !queues[1].empty() ||
                   (!queues[2].empty() && active_by_priority[2] < historical_limit);
        });
        if (stopping)
            return {};
        for (std::size_t priority = 0; priority < queues.size(); ++priority) {
            auto& queue = queues[priority];
            if (queue.empty())
                continue;
            const std::size_t historical_limit = options.workers == 1 ? 1 : options.workers - 1;
            if (priority == 2 && active_by_priority[2] >= historical_limit)
                continue;
            auto work = queue.front();
            queue.pop_front();
            ++active;
            ++active_by_priority[priority];
            const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - work->queued_at)
                                     .count();
            maximum_queue_latency_ms =
                std::max(maximum_queue_latency_ms, static_cast<std::uint64_t>(latency));
            return work;
        }
        return {};
    }

    void finish_active(AnalysisPriority priority) {
        std::lock_guard lock(mutex);
        --active;
        --active_by_priority[priority_index(priority)];
        condition.notify_all();
    }

    void worker(std::size_t index) {
        std::unique_ptr<AnalysisEngine> engine;
        while (true) {
            const auto work = take();
            if (!work)
                return;
            try {
                if (!engine)
                    engine = factory(index);
                AnalysisResult result =
                    engine->analyze(work->request, work->cancellation.get_token());
                {
                    std::lock_guard lock(mutex);
                    ++completed;
                }
                work->promise.set_value(std::move(result));
                finish_active(work->request.priority);
            } catch (...) {
                engine.reset();
                bool retry = false;
                {
                    std::lock_guard lock(mutex);
                    --active;
                    --active_by_priority[priority_index(work->request.priority)];
                    retry = !stopping && !work->cancellation.stop_requested() &&
                            work->attempts < options.retry_limit;
                    if (retry) {
                        ++work->attempts;
                        ++retried;
                        work->queued_at = std::chrono::steady_clock::now();
                        queues[priority_index(work->request.priority)].push_front(work);
                    } else {
                        ++failed;
                    }
                }
                if (retry) {
                    condition.notify_one();
                } else {
                    work->promise.set_exception(std::current_exception());
                    condition.notify_all();
                }
            }
        }
    }

    EngineFactory factory;
    EnginePoolOptions options;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::array<std::deque<std::shared_ptr<Work>>, 3> queues;
    std::vector<std::thread> threads;
    bool stopping{false};
    std::uint64_t submitted{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::uint64_t retried{0};
    std::uint64_t rejected{0};
    std::uint64_t active{0};
    std::array<std::size_t, 3> active_by_priority{};
    std::uint64_t maximum_queue_latency_ms{0};
};

EnginePool::EnginePool(EngineFactory factory, EnginePoolOptions options)
    : state_(std::make_unique<State>(std::move(factory), options)) {}

EnginePool::~EnginePool() = default;

AnalysisResult EnginePool::analyze(const AnalysisRequest& request, CancellationToken stop_token) {
    auto work = std::make_shared<State::Work>();
    work->request = request;
    work->queued_at = std::chrono::steady_clock::now();
    std::future<AnalysisResult> result = work->promise.get_future();
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping)
            throw Error(ErrorCode::EngineError, "engine pool is shutting down");
        if (state_->pending_unlocked() >= state_->options.max_pending) {
            ++state_->rejected;
            throw Error(ErrorCode::EngineError, "engine pool backpressure limit reached");
        }
        state_->queues[priority_index(request.priority)].push_back(work);
        ++state_->submitted;
    }
    state_->condition.notify_one();
    while (result.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) {
        if (stop_token.stop_requested())
            work->cancellation.request_stop();
    }
    return result.get();
}

EnginePoolStats EnginePool::stats() const {
    std::lock_guard lock(state_->mutex);
    return EnginePoolStats{
        state_->submitted,
        state_->completed,
        state_->failed,
        state_->retried,
        state_->rejected,
        state_->active,
        state_->queues[0].size(),
        state_->queues[1].size(),
        state_->queues[2].size(),
        state_->maximum_queue_latency_ms,
    };
}

std::size_t EnginePool::worker_count() const noexcept {
    return state_->options.workers;
}

} // namespace pct::engine

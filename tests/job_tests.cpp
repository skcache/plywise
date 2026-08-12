#include "test.hpp"

#include "pct/app/job_manager.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <unistd.h>

using namespace pct;

namespace {

class QuietEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken) override {
        chess::Board board = chess::Board::from_fen(request.fen);
        const auto moves = board.legal_moves();
        const std::string best = moves.empty() ? "(none)" : chess::uci(moves.front());
        return engine::AnalysisResult{
            {engine::PrincipalVariation{1, request.depth, 0, std::nullopt, 1, 1, {best}}},
            best,
            {}};
    }
};

class StagedEngine final : public engine::AnalysisEngine {
  public:
    std::atomic<bool> allow_deep{false};

    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken stop_token) override {
        {
            std::lock_guard lock(mutex_);
            depths_.push_back(request.depth);
            if (request.depth >= 3)
                deep_started_ = true;
        }
        condition_.notify_all();
        while (request.depth >= 3 && !allow_deep.load() && !stop_token.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        chess::Board board = chess::Board::from_fen(request.fen);
        const auto moves = board.legal_moves();
        const std::string best = moves.empty() ? "(none)" : chess::uci(moves.front());
        return {{{1, request.depth, 160, std::nullopt, 1, 1, {best}}}, best, {}};
    }

    [[nodiscard]] std::vector<int> depths() const {
        std::lock_guard lock(mutex_);
        return depths_;
    }

    [[nodiscard]] bool wait_for_deep() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return deep_started_; });
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<int> depths_;
    bool deep_started_{false};
};

class PreemptibleEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken stop_token) override {
        if (request.priority == engine::AnalysisPriority::Historical) {
            {
                std::lock_guard lock(mutex_);
                historical_started_ = true;
            }
            condition_.notify_all();
            while (true) {
                {
                    std::lock_guard lock(mutex_);
                    if (allow_historical_)
                        break;
                }
                if (stop_token.stop_requested()) {
                    {
                        std::lock_guard lock(mutex_);
                        historical_preempted_ = true;
                    }
                    condition_.notify_all();
                    throw Error(ErrorCode::Timeout, "historical analysis preempted");
                }
                std::this_thread::yield();
            }
        }
        chess::Board board = chess::Board::from_fen(request.fen);
        const auto moves = board.legal_moves();
        const std::string best = moves.empty() ? "(none)" : chess::uci(moves.front());
        return {{{1, request.depth, 0, std::nullopt, 1, 1, {best}}}, best, {}};
    }

    [[nodiscard]] bool wait_for_historical_start() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return historical_started_; });
    }

    [[nodiscard]] bool wait_for_preemption() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return historical_preempted_; });
    }

    void release_historical() {
        {
            std::lock_guard lock(mutex_);
            allow_historical_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool historical_started_{false};
    bool historical_preempted_{false};
    bool allow_historical_{false};
};

class PathCleanup final {
  public:
    explicit PathCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~PathCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    PathCleanup(const PathCleanup&) = delete;
    PathCleanup& operator=(const PathCleanup&) = delete;

  private:
    std::filesystem::path path_;
};

void run_priority_cleanup_iteration(std::size_t iteration) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("pct-jobs-priority-" + std::to_string(::getpid()) + "-" +
         std::to_string(iteration));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        import::ImportService importer;
        const auto older = importer.from_pgn(
            "[Date \"2025.01.01\"]\n[White \"A\"]\n[Black \"B\"]\n"
            "[Result \"1-0\"]\n\n1. e4 e5 1-0");
        const auto recent = importer.from_pgn(
            "[Date \"2026.06.20\"]\n[White \"A\"]\n[Black \"C\"]\n"
            "[Result \"0-1\"]\n\n1. d4 d5 0-1");
        static_cast<void>(repository.add(older));
        static_cast<void>(repository.add(recent));
        StagedEngine engine;
        analysis::AnalysisCache cache;
        analysis::Analyzer analyzer(engine, cache,
                                    analysis::AnalyzerOptions{2, 3, 80, 2, 1});
        repository.save_shallow_analysis(analyzer.analyze_shallow(older.game));
        repository.set_background_paused(true);
        app::JobManager jobs(repository, analyzer);
        const auto started =
            jobs.start_batch({older.game.identity, recent.game.identity});
        CHECK_EQ(started.size(), 2ULL);
        CHECK_EQ(started.front().game_id, recent.game.identity);
        jobs.resume();
        for (int attempt = 0; attempt < 200 &&
                              !repository.get(recent.game.identity)->shallow_analysis;
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(repository.get(recent.game.identity)->shallow_analysis.has_value());
        CHECK(!repository.get(older.game.identity)->analysis.has_value());
        engine.allow_deep = true;
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (repository.get(older.game.identity)->analysis &&
                repository.get(recent.game.identity)->analysis) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(repository.get(older.game.identity)->analysis.has_value());
        CHECK(repository.get(recent.game.identity)->analysis.has_value());
    }
    std::filesystem::remove_all(directory);
    CHECK(!std::filesystem::exists(directory));
}

} // namespace

TEST_CASE("job manager runs analysis in background and deduplicates active work") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("pct-jobs-" + std::to_string(::getpid()) + ".log");
    std::filesystem::remove(path);
    const PathCleanup cleanup(path);
    storage::EventLog log(path);
    app::Repository repository(log);
    import::ImportService importer;
    const auto imported =
        importer.from_pgn("[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    static_cast<void>(repository.add(imported));
    QuietEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    app::JobManager jobs(repository, analyzer);
    const auto first = jobs.start(imported.game.identity);
    const auto duplicate = jobs.start(imported.game.identity);
    CHECK_EQ(first.id, duplicate.id);
    app::JobStatus status = app::JobStatus::Queued;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        status = jobs.get(first.id)->status;
        if (status == app::JobStatus::Complete || status == app::JobStatus::Failed)
            break;
    }
    CHECK(status == app::JobStatus::Complete);
    CHECK(repository.get(imported.game.identity)->analysis.has_value());
}

TEST_CASE("paused analysis queue and incomplete work resume after restart") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-resume-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    std::string game_id;
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        import::ImportService importer;
        const auto imported = importer.from_pgn(
            "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
        game_id = imported.game.identity;
        static_cast<void>(repository.add(imported));
        QuietEngine engine;
        analysis::AnalysisCache cache;
        analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
        repository.save_shallow_analysis(analyzer.analyze_shallow(imported.game));
        repository.record_job_state(game_id, "running");
        repository.set_background_paused(true);
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        QuietEngine engine;
        analysis::AnalysisCache cache;
        analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
        app::JobManager jobs(repository, analyzer);
        CHECK(jobs.paused());
        CHECK(repository.get(game_id)->shallow_analysis.has_value());
        CHECK_EQ(jobs.list().size(), 1ULL);
        CHECK(jobs.list().front().status == app::JobStatus::Queued);
        CHECK(jobs.list().front().progress.stage == analysis::AnalysisStage::DeepAnalysis);
        jobs.resume();
        for (int attempt = 0; attempt < 100 && !repository.get(game_id)->analysis; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(repository.get(game_id)->analysis.has_value());
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("queued batch work can be cancelled and retried idempotently") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-retry-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(path);
    app::Repository repository(log);
    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. d4 d5 1-0");
    static_cast<void>(repository.add(imported));
    repository.set_background_paused(true);
    QuietEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    app::JobManager jobs(repository, analyzer);
    const auto cancelled = jobs.start(imported.game.identity);
    CHECK(jobs.cancel(cancelled.id));
    CHECK(jobs.get(cancelled.id)->status == app::JobStatus::Cancelled);
    const auto retried = jobs.start(imported.game.identity);
    CHECK(retried.id != cancelled.id);
    jobs.resume();
    for (int attempt = 0; attempt < 100 && !repository.get(imported.game.identity)->analysis;
         ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(repository.get(imported.game.identity)->analysis.has_value());
}

TEST_CASE("batch scheduling persists every shallow projection before deep work") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-staged-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(path);
    app::Repository repository(log);
    import::ImportService importer;
    const auto first = importer.from_pgn(
        "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    const auto second = importer.from_pgn(
        "[White \"A\"]\n[Black \"C\"]\n[Result \"0-1\"]\n\n1. d4 d5 0-1");
    static_cast<void>(repository.add(first));
    static_cast<void>(repository.add(second));
    StagedEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    app::JobManager jobs(repository, analyzer);
    const auto started =
        jobs.start_batch({first.game.identity, second.game.identity});
    CHECK_EQ(started.size(), 2ULL);
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (repository.get(first.game.identity)->shallow_analysis &&
            repository.get(second.game.identity)->shallow_analysis)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(repository.get(first.game.identity)->shallow_analysis.has_value());
    CHECK(repository.get(second.game.identity)->shallow_analysis.has_value());
    CHECK(!repository.get(first.game.identity)->analysis.has_value());
    CHECK(!repository.get(second.game.identity)->analysis.has_value());
    CHECK_EQ(repository.profile().games_shallow_analyzed, 2ULL);
    CHECK(cache.hit_count() > 0);
    CHECK(engine.wait_for_deep());
    const auto depths_before_release = engine.depths();
    const auto first_deep = std::find_if(depths_before_release.begin(), depths_before_release.end(),
                                         [](int depth) { return depth >= 3; });
    CHECK(first_deep != depths_before_release.end());
    CHECK(std::find_if(first_deep + 1, depths_before_release.end(),
                       [](int depth) { return depth < 3; }) == depths_before_release.end());
    engine.allow_deep = true;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (repository.get(first.game.identity)->analysis &&
            repository.get(second.game.identity)->analysis)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(repository.get(first.game.identity)->analysis.has_value());
    CHECK(repository.get(second.game.identity)->analysis.has_value());
}

TEST_CASE("interactive work is selected before queued historical work") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-interactive-priority-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(directory / "events.log");
    app::Repository repository(log);
    import::ImportService importer;
    const auto historical = importer.from_pgn(
        "[White \"Archive\"]\n[Black \"Past\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    const auto interactive = importer.from_pgn(
        "[White \"Player\"]\n[Black \"Open\"]\n[Result \"0-1\"]\n\n1. d4 d5 0-1");
    static_cast<void>(repository.add(historical));
    static_cast<void>(repository.add(interactive));
    repository.set_background_paused(true);
    QuietEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    std::mutex observer_mutex;
    std::condition_variable observer_condition;
    std::vector<std::string> running;
    std::vector<std::string> completed;
    app::JobManager jobs(repository, analyzer, app::JobManagerOptions{1, 8, 0});
    static_cast<void>(jobs.start_batch({historical.game.identity}));
    const auto opened = jobs.start(interactive.game.identity);
    jobs.set_observer([&](const app::AnalysisJob& job) {
        if (job.status != app::JobStatus::Running &&
            job.status != app::JobStatus::Complete)
            return;
        {
            std::lock_guard lock(observer_mutex);
            (job.status == app::JobStatus::Running ? running : completed)
                .push_back(job.game_id);
        }
        observer_condition.notify_all();
    });
    jobs.resume();
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return !running.empty(); }));
        CHECK_EQ(running.front(), opened.game_id);
    }
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return completed.size() == 2; }));
    }
}

TEST_CASE("one worker preempts historical work to admit an interactive game") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-interactive-admission-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(directory / "events.log");
    app::Repository repository(log);
    import::ImportService importer;
    const auto historical = importer.from_pgn(
        "[White \"Archive\"]\n[Black \"Past\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    const auto interactive = importer.from_pgn(
        "[White \"Player\"]\n[Black \"Open\"]\n[Result \"0-1\"]\n\n1. d4 d5 0-1");
    static_cast<void>(repository.add(historical));
    static_cast<void>(repository.add(interactive));
    PreemptibleEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    std::mutex observer_mutex;
    std::condition_variable observer_condition;
    bool interactive_complete = false;
    bool historical_complete = false;
    app::JobManager jobs(repository, analyzer, app::JobManagerOptions{1, 1, 0});
    const auto archived = jobs.start_batch({historical.game.identity}).front();
    CHECK(engine.wait_for_historical_start());
    CHECK_EQ(jobs.queued_count(), 0ULL);
    jobs.set_observer([&](const app::AnalysisJob& job) {
        if (job.game_id == interactive.game.identity &&
            job.status == app::JobStatus::Complete) {
            {
                std::lock_guard lock(observer_mutex);
                interactive_complete = true;
            }
            observer_condition.notify_all();
        } else if (job.game_id == historical.game.identity &&
                   job.status == app::JobStatus::Complete) {
            {
                std::lock_guard lock(observer_mutex);
                historical_complete = true;
            }
            observer_condition.notify_all();
        }
    });
    const auto opened = jobs.start(interactive.game.identity);
    CHECK_EQ(opened.game_id, interactive.game.identity);
    CHECK(engine.wait_for_preemption());
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return interactive_complete; }));
    }
    CHECK(repository.get(interactive.game.identity)->analysis.has_value());
    CHECK(!repository.get(historical.game.identity)->analysis.has_value());
    engine.release_historical();
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return historical_complete; }));
    }
    CHECK(repository.get(historical.game.identity)->analysis.has_value());
    CHECK(jobs.get(archived.id)->status == app::JobStatus::Complete);
}

TEST_CASE("observer unsubscribe waits for callbacks and callback failures do not stop workers") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-observer-lifecycle-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(directory / "events.log");
    app::Repository repository(log);
    import::ImportService importer;
    const auto first = importer.from_pgn(
        "[White \"Observer\"]\n[Black \"First\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    const auto second = importer.from_pgn(
        "[White \"Observer\"]\n[Black \"Second\"]\n[Result \"0-1\"]\n\n1. d4 d5 0-1");
    static_cast<void>(repository.add(first));
    static_cast<void>(repository.add(second));
    repository.set_background_paused(true);
    QuietEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    std::mutex observer_mutex;
    std::condition_variable observer_condition;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_finished = false;
    bool second_complete = false;
    app::JobManager jobs(repository, analyzer, app::JobManagerOptions{1, 8, 0});
    jobs.set_observer([&](const app::AnalysisJob& job) {
        if (job.game_id != first.game.identity || job.status != app::JobStatus::Running)
            return;
        std::unique_lock lock(observer_mutex);
        callback_entered = true;
        observer_condition.notify_all();
        observer_condition.wait(lock, [&] { return release_callback; });
        callback_finished = true;
        lock.unlock();
        throw std::runtime_error("observer failure");
    });
    static_cast<void>(jobs.start(first.game.identity));
    jobs.resume();
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return callback_entered; }));
    }
    std::promise<void> unsubscribe_started;
    auto started = unsubscribe_started.get_future();
    auto unsubscribe = std::async(std::launch::async, [&] {
        unsubscribe_started.set_value();
        jobs.set_observer({});
        std::lock_guard lock(observer_mutex);
        return callback_finished;
    });
    started.wait();
    CHECK(unsubscribe.wait_for(std::chrono::milliseconds(50)) ==
          std::future_status::timeout);
    {
        std::lock_guard lock(observer_mutex);
        release_callback = true;
    }
    observer_condition.notify_all();
    CHECK(unsubscribe.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(unsubscribe.get());

    jobs.set_observer([&](const app::AnalysisJob& job) {
        if (job.game_id != second.game.identity || job.status != app::JobStatus::Complete)
            return;
        {
            std::lock_guard lock(observer_mutex);
            second_complete = true;
        }
        observer_condition.notify_all();
    });
    static_cast<void>(jobs.start(second.game.identity));
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return second_complete; }));
    }
    jobs.set_observer({});
    CHECK(repository.get(first.game.identity)->analysis.has_value());
    CHECK(repository.get(second.game.identity)->analysis.has_value());
}

TEST_CASE("additional job observers can be subscribed and removed independently") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-additional-observers-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(directory / "events.log");
    app::Repository repository(log);
    import::ImportService importer;
    const auto first = importer.from_pgn(
        "[White \"Observer\"]\n[Black \"First\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0");
    const auto second = importer.from_pgn(
        "[White \"Observer\"]\n[Black \"Second\"]\n[Result \"0-1\"]\n\n1. d4 d5 0-1");
    static_cast<void>(repository.add(first));
    static_cast<void>(repository.add(second));
    repository.set_background_paused(true);
    QuietEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    std::mutex observer_mutex;
    std::condition_variable observer_condition;
    std::size_t first_calls = 0;
    std::size_t second_calls = 0;
    {
        app::JobManager jobs(repository, analyzer, app::JobManagerOptions{1, 8, 0});
        const auto first_observer = jobs.add_observer([&](const app::AnalysisJob&) {
            {
                std::lock_guard lock(observer_mutex);
                ++first_calls;
            }
            observer_condition.notify_all();
        });
        const auto second_observer = jobs.add_observer([&](const app::AnalysisJob&) {
            {
                std::lock_guard lock(observer_mutex);
                ++second_calls;
            }
            observer_condition.notify_all();
        });
        CHECK(first_observer != 0);
        CHECK(second_observer != 0);
        static_cast<void>(jobs.start(first.game.identity));
        jobs.resume();
        {
            std::unique_lock lock(observer_mutex);
            CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                              [&] { return second_calls >= 3; }));
        }
        std::size_t first_before_remove = 0;
        {
            std::lock_guard lock(observer_mutex);
            first_before_remove = first_calls;
        }
        jobs.remove_observer(first_observer);
        static_cast<void>(jobs.start(second.game.identity));
        {
            std::unique_lock lock(observer_mutex);
            CHECK(observer_condition.wait_for(lock, std::chrono::seconds(2),
                                              [&] { return second_calls >= 6; }));
        }
        {
            std::lock_guard lock(observer_mutex);
            CHECK_EQ(first_calls, first_before_remove);
        }
        jobs.remove_observer(second_observer);
    }
}

TEST_CASE("batch scheduling prioritizes recent shallow work before resumed deep work") {
    for (std::size_t iteration = 0; iteration < 25; ++iteration)
        run_priority_cleanup_iteration(iteration);
}

TEST_CASE("completed batch games are retained without being requeued") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-jobs-complete-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const PathCleanup cleanup(directory);
    storage::EventLog log(path);
    app::Repository repository(log);
    import::ImportService importer;
    const auto imported = importer.from_pgn(
        "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. c4 c5 1-0");
    static_cast<void>(repository.add(imported));
    analysis::GameAnalysis completed;
    completed.game_id = imported.game.identity;
    repository.save_analysis(completed);
    QuietEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache);
    app::JobManager jobs(repository, analyzer);
    const auto job = jobs.start(imported.game.identity);
    CHECK(job.status == app::JobStatus::Complete);
    CHECK_EQ(job.progress.message, "Loaded from storage");
}

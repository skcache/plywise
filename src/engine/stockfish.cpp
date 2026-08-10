#include "pct/engine/stockfish.hpp"

#include "pct/common/error.hpp"
#include "pct/common/log.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <thread>

namespace pct::engine {
namespace {

int parse_int(std::string_view value, std::string_view field) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw Error(ErrorCode::EngineError, "invalid Stockfish " + std::string(field));
    }
    return result;
}

std::uint64_t parse_uint64(std::string_view value, std::string_view field) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw Error(ErrorCode::EngineError, "invalid Stockfish " + std::string(field));
    }
    return result;
}

std::vector<std::string> words(std::string_view line) {
    std::istringstream input{std::string(line)};
    std::vector<std::string> tokens;
    for (std::string token; input >> token;)
        tokens.push_back(std::move(token));
    return tokens;
}

bool executable_file(const std::filesystem::path& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}

std::optional<std::string> search_path(std::string_view executable) {
    const char* path = std::getenv("PATH");
    if (path == nullptr)
        return std::nullopt;
    std::string_view remaining(path);
    while (!remaining.empty()) {
        const std::size_t separator = remaining.find(':');
        const std::string_view entry =
            separator == std::string_view::npos ? remaining : remaining.substr(0, separator);
        if (!entry.empty()) {
            const std::filesystem::path candidate =
                std::filesystem::path(std::string(entry)) / std::string(executable);
            if (executable_file(candidate))
                return candidate.string();
        }
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

std::optional<std::string> common_stockfish_path() {
    for (const std::filesystem::path candidate :
         {"/opt/homebrew/bin/stockfish", "/usr/local/bin/stockfish", "/usr/games/stockfish"}) {
        if (executable_file(candidate))
            return candidate.string();
    }
    return std::nullopt;
}

} // namespace

Stockfish::Stockfish(StockfishOptions options) : options_(std::move(options)) {}

Stockfish::~Stockfish() {
    stop();
}

bool Stockfish::running() const noexcept {
    return process_id_ > 0;
}

std::string Stockfish::resolve_executable(std::string requested) {
    if (requested.empty())
        requested = "stockfish";

    if (requested == "stockfish") {
        if (const char* environment = std::getenv("PCT_STOCKFISH");
            environment != nullptr && environment[0] != '\0') {
            requested = environment;
        }
    }

    if (requested.find('/') != std::string::npos) {
        if (executable_file(requested))
            return requested;
        throw Error(ErrorCode::EngineError,
                    "Stockfish executable is missing or not executable: " + requested);
    }

    if (auto found = search_path(requested))
        return *found;
    if (requested == "stockfish") {
        if (auto found = common_stockfish_path())
            return *found;
    }

    throw Error(ErrorCode::EngineError,
                "Stockfish executable was not found. Install it with 'brew install stockfish', "
                "set PCT_STOCKFISH, or pass --stockfish /absolute/path/to/stockfish.");
}

void Stockfish::start() {
    if (running())
        return;
    const std::string executable = resolve_executable(options_.executable);
    static const auto ignored_sigpipe = signal(SIGPIPE, SIG_IGN);
    static_cast<void>(ignored_sigpipe);
    int parent_to_child[2];
    int child_to_parent[2];
    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        throw Error(ErrorCode::EngineError,
                    std::string("failed to create Stockfish pipes: ") + std::strerror(errno));
    }
    const pid_t child = fork();
    if (child < 0) {
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);
        throw Error(ErrorCode::EngineError,
                    std::string("failed to start Stockfish: ") + std::strerror(errno));
    }
    if (child == 0) {
        dup2(parent_to_child[0], STDIN_FILENO);
        dup2(child_to_parent[1], STDOUT_FILENO);
        dup2(child_to_parent[1], STDERR_FILENO);
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);
        execl(executable.c_str(), executable.c_str(), nullptr);
        _exit(127);
    }
    close(parent_to_child[0]);
    close(child_to_parent[1]);
    input_fd_ = parent_to_child[1];
    output_fd_ = child_to_parent[0];
    process_id_ = child;
    read_buffer_.clear();

    try {
        send("uci");
        wait_for("uciok", std::chrono::seconds(5));
        send("setoption name Hash value " + std::to_string(options_.hash_mb));
        send("setoption name Threads value " + std::to_string(options_.threads));
        send("isready");
        wait_for("readyok", std::chrono::seconds(5));
        log(LogLevel::Info, "stockfish", "engine process is ready: " + executable);
    } catch (...) {
        stop();
        throw;
    }
}

void Stockfish::stop() noexcept {
    if (input_fd_ >= 0) {
        const char quit[] = "quit\n";
        // Shutdown is best effort: the child may already have closed the pipe. Keep the return
        // value consumed so GCC's warn_unused_result does not turn this noexcept cleanup into a
        // build failure.
        [[maybe_unused]] const ssize_t written = write(input_fd_, quit, sizeof(quit) - 1);
        close(input_fd_);
        input_fd_ = -1;
    }
    if (output_fd_ >= 0) {
        close(output_fd_);
        output_fd_ = -1;
    }
    if (process_id_ > 0) {
        int status = 0;
        pid_t result = waitpid(process_id_, &status, WNOHANG);
        if (result == 0) {
            for (int attempt = 0; attempt < 20 && result == 0; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                result = waitpid(process_id_, &status, WNOHANG);
            }
        }
        if (result == 0) {
            kill(process_id_, SIGTERM);
            waitpid(process_id_, &status, 0);
        }
        process_id_ = -1;
    }
    read_buffer_.clear();
}

void Stockfish::restart() {
    stop();
    start();
}

void Stockfish::send(std::string_view command) {
    if (!running() || input_fd_ < 0) {
        throw Error(ErrorCode::EngineError, "Stockfish is not running");
    }
    std::string message(command);
    message.push_back('\n');
    std::size_t offset = 0;
    while (offset < message.size()) {
        const ssize_t written = write(input_fd_, message.data() + offset, message.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            throw Error(ErrorCode::EngineError,
                        std::string("failed to write to Stockfish: ") + std::strerror(errno));
        }
        offset += static_cast<std::size_t>(written);
    }
}

std::string Stockfish::read_line(std::chrono::steady_clock::time_point deadline,
                                 CancellationToken stop_token) {
    while (true) {
        if (const std::size_t newline = read_buffer_.find('\n'); newline != std::string::npos) {
            std::string line = read_buffer_.substr(0, newline);
            read_buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return line;
        }
        if (stop_token.stop_requested()) {
            throw Error(ErrorCode::Timeout, "Stockfish analysis was cancelled");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            throw Error(ErrorCode::Timeout, "Stockfish response timed out");
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd descriptor{output_fd_, POLLIN, 0};
        const int result = poll(
            &descriptor, 1,
            static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining.count(), 50)));
        if (result < 0) {
            if (errno == EINTR)
                continue;
            throw Error(ErrorCode::EngineError,
                        std::string("failed to poll Stockfish: ") + std::strerror(errno));
        }
        if (result == 0)
            continue;
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
            (descriptor.revents & POLLIN) == 0) {
            throw Error(ErrorCode::EngineError, "Stockfish process exited unexpectedly");
        }
        char buffer[4096];
        const ssize_t count = read(output_fd_, buffer, sizeof(buffer));
        if (count == 0)
            throw Error(ErrorCode::EngineError, "Stockfish closed its output pipe");
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw Error(ErrorCode::EngineError,
                        std::string("failed to read Stockfish: ") + std::strerror(errno));
        }
        read_buffer_.append(buffer, static_cast<std::size_t>(count));
    }
}

void Stockfish::wait_for(std::string_view token, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::string line = read_line(deadline);
        if (line == token || line.starts_with(std::string(token) + " "))
            return;
    }
}

std::optional<PrincipalVariation> Stockfish::parse_info_line(std::string_view line) {
    const auto tokens = words(line);
    if (tokens.empty() || tokens.front() != "info")
        return std::nullopt;
    PrincipalVariation result;
    bool has_score = false;
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token == "depth" && index + 1 < tokens.size()) {
            result.depth = parse_int(tokens[++index], "depth");
        } else if (token == "multipv" && index + 1 < tokens.size()) {
            result.multipv = parse_int(tokens[++index], "multipv");
        } else if (token == "score" && index + 2 < tokens.size()) {
            const std::string kind = tokens[++index];
            const int score = parse_int(tokens[++index], "score");
            if (kind == "cp")
                result.centipawns = score;
            if (kind == "mate")
                result.mate = score;
            has_score = kind == "cp" || kind == "mate";
        } else if (token == "nodes" && index + 1 < tokens.size()) {
            result.nodes = parse_uint64(tokens[++index], "node count");
        } else if (token == "time" && index + 1 < tokens.size()) {
            result.time_ms = parse_uint64(tokens[++index], "time");
        } else if (token == "pv") {
            for (++index; index < tokens.size(); ++index)
                result.moves.push_back(tokens[index]);
            break;
        }
    }
    return has_score ? std::optional(result) : std::nullopt;
}

AnalysisResult Stockfish::analyze(const AnalysisRequest& request, CancellationToken stop_token) {
    if (request.fen.empty() || request.multipv < 1 || request.multipv > 10 ||
        (request.depth < 1 && request.move_time.count() <= 0)) {
        throw Error(ErrorCode::InvalidArgument, "invalid Stockfish analysis request");
    }
    if (!running())
        start();
    try {
        send("setoption name MultiPV value " + std::to_string(request.multipv));
        send("position fen " + request.fen);
        if (request.move_time.count() > 0) {
            send("go movetime " + std::to_string(request.move_time.count()));
        } else {
            send("go depth " + std::to_string(request.depth));
        }

        std::map<int, PrincipalVariation> latest;
        AnalysisResult result;
        const auto deadline = std::chrono::steady_clock::now() + request.timeout;
        try {
            while (true) {
                const std::string line = read_line(deadline, stop_token);
                if (line.starts_with("bestmove ")) {
                    const auto tokens = words(line);
                    if (tokens.size() >= 2)
                        result.best_move = tokens[1];
                    if (tokens.size() >= 4 && tokens[2] == "ponder")
                        result.ponder_move = tokens[3];
                    break;
                }
                if (auto parsed = parse_info_line(line))
                    latest[parsed->multipv] = std::move(*parsed);
            }
        } catch (const Error& error) {
            send("stop");
            try {
                wait_for("bestmove", std::chrono::seconds(2));
            } catch (...) {
                restart();
            }
            throw Error(error.code(), error.what());
        }
        for (auto& [_, line] : latest)
            result.lines.push_back(std::move(line));
        if (result.best_move.empty()) {
            throw Error(ErrorCode::EngineError, "Stockfish returned no best move");
        }
        return result;
    } catch (const Error& error) {
        if (error.code() == ErrorCode::EngineError && running()) {
            try {
                restart();
            } catch (...) {
            }
        }
        throw;
    }
}

} // namespace pct::engine

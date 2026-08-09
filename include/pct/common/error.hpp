#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace pct {

enum class ErrorCode {
    InvalidArgument,
    ParseError,
    IllegalMove,
    IoError,
    NetworkError,
    EngineError,
    Timeout,
    QuotaExceeded,
    Corruption,
    NotFound,
    Unsupported,
};

class Error : public std::runtime_error {
  public:
    Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept {
        return code_;
    }

  private:
    ErrorCode code_;
};

} // namespace pct

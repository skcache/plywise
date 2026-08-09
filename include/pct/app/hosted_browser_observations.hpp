#pragma once

#include "pct/analysis/browser_observation.hpp"
#include "pct/app/repository.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace pct::app {

// Durable owner-scoped staging for browser engine observations. The API still validates every
// observation before calling this adapter; PostgreSQL supplies restart recovery and idempotency.
class HostedBrowserObservationStore final {
  public:
    explicit HostedBrowserObservationStore(std::string connection_string);
    ~HostedBrowserObservationStore();

    HostedBrowserObservationStore(const HostedBrowserObservationStore&) = delete;
    HostedBrowserObservationStore& operator=(const HostedBrowserObservationStore&) = delete;

    void begin(const OwnerId& owner, const analysis::BrowserObservationRunContext& context);
    [[nodiscard]] analysis::BrowserObservationReceipt submit(
        const OwnerId& owner, const analysis::BrowserObservationContext& context,
        const analysis::BrowserEngineObservation& observation);
    [[nodiscard]] analysis::BrowserObservationBundle finalize(const OwnerId& owner,
                                                              std::string_view game_id,
                                                              std::string_view analysis_run_id);

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace pct::app

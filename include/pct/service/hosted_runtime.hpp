#pragma once

#include "pct/analysis/analyzer.hpp"
#include "pct/app/job_manager.hpp"
#include "pct/import/import_service.hpp"
#include "pct/service/http_server.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace pct::service {

struct HostedRuntimeOptions {
    std::string postgres_connection;
    std::string oidc_issuer;
    std::string oidc_audience{"authenticated"};
    std::string oidc_provider{"supabase"};
    std::string oidc_jwks_url;
    // Owner resources are retained while active requests, jobs, or event observers use them.
    // Idle resources can be reclaimed when this bound is reached.
    std::size_t max_owner_resources{128};
    std::chrono::seconds owner_resource_idle_ttl{std::chrono::minutes(10)};
};

// Owns the hosted identity store, verified OIDC boundary, and bounded owner-scoped PostgreSQL
// resources. The runtime never exposes a database connection or token to React; it returns the
// callbacks needed by Api::AuthConfig and pins a scope only while it is active or in flight.
class HostedRuntime final {
  public:
    HostedRuntime(HostedRuntimeOptions options, import::ImportService& importer,
                  analysis::Analyzer& analyzer,
                  app::JobManagerOptions job_options = {});
    ~HostedRuntime();

    HostedRuntime(const HostedRuntime&) = delete;
    HostedRuntime& operator=(const HostedRuntime&) = delete;

    [[nodiscard]] AuthConfig::TokenVerifier token_verifier() const;
    [[nodiscard]] AuthConfig::ScopeResolver scope_resolver() const;
    [[nodiscard]] AuthConfig::GuestSessionCreator guest_session_creator() const;
    [[nodiscard]] AuthConfig::GuestAnalysisReservation guest_analysis_reservation() const;
    [[nodiscard]] AuthConfig::BrowserObservationBegin browser_observation_begin() const;
    [[nodiscard]] AuthConfig::BrowserObservationSubmit browser_observation_submit() const;
    [[nodiscard]] AuthConfig::BrowserObservationFinalize browser_observation_finalize() const;
    [[nodiscard]] AuthConfig::GuestClaimHandler guest_claim_handler() const;
    [[nodiscard]] AuthConfig::FreshTokenVerifier fresh_token_verifier() const;
    [[nodiscard]] AuthConfig::AccountExportHandler account_export_handler() const;
    [[nodiscard]] AuthConfig::AccountDeletionHandler account_deletion_handler() const;
    [[nodiscard]] AuthConfig::WebSocketTicketIssuer websocket_ticket_issuer() const;
    [[nodiscard]] AuthConfig::WebSocketTicketVerifier websocket_ticket_verifier() const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace pct::service

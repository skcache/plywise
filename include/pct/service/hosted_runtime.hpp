#pragma once

#include "pct/analysis/analyzer.hpp"
#include "pct/app/job_manager.hpp"
#include "pct/service/http_server.hpp"

#include <memory>
#include <string>

namespace pct::service {

struct HostedRuntimeOptions {
    std::string postgres_connection;
    std::string oidc_issuer;
    std::string oidc_audience{"authenticated"};
    std::string oidc_provider{"supabase"};
    std::string oidc_jwks_url;
};

// Owns the hosted identity store, verified OIDC boundary, and owner-scoped PostgreSQL resources.
// The runtime never exposes a database connection or token to React; it returns the callbacks
// needed by Api::AuthConfig and keeps all owner resources alive for the process lifetime.
class HostedRuntime final {
  public:
    HostedRuntime(HostedRuntimeOptions options, analysis::Analyzer& analyzer,
                  app::JobManagerOptions job_options = {});
    ~HostedRuntime();

    HostedRuntime(const HostedRuntime&) = delete;
    HostedRuntime& operator=(const HostedRuntime&) = delete;

    [[nodiscard]] AuthConfig::TokenVerifier token_verifier() const;
    [[nodiscard]] AuthConfig::ScopeResolver scope_resolver() const;
    [[nodiscard]] AuthConfig::GuestSessionCreator guest_session_creator() const;
    [[nodiscard]] AuthConfig::GuestAnalysisReservation guest_analysis_reservation() const;
    [[nodiscard]] AuthConfig::GuestClaimHandler guest_claim_handler() const;
    [[nodiscard]] AuthConfig::FreshTokenVerifier fresh_token_verifier() const;
    [[nodiscard]] AuthConfig::AccountExportHandler account_export_handler() const;
    [[nodiscard]] AuthConfig::AccountDeletionHandler account_deletion_handler() const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace pct::service

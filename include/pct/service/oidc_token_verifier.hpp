#pragma once

#include "pct/app/repository.hpp"

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pct::service {

struct OidcTokenVerifierOptions {
    std::string issuer;
    std::string audience;
    std::string provider;
    std::uint32_t clock_skew_seconds{60};

    // The deployment owns the HTTPS/JWKS transport. The verifier only accepts a bounded,
    // provider-configured key set through this callback, which keeps network policy outside the
    // token parser and makes key rotation testable without contacting a live provider.
    std::function<std::optional<std::string>()> load_jwks;
    std::function<std::optional<app::OwnerId>(std::string_view provider,
                                              std::string_view subject)>
        resolve_account;
    // Hosted deployments may use the signed issuance time to reject a bearer that predates
    // account deletion. Keep the two-argument callback for local/test integrations that do not
    // retain deletion tombstones; production identity resolution should provide this callback.
    std::function<std::optional<app::OwnerId>(std::string_view provider,
                                              std::string_view subject,
                                              std::int64_t issued_at_ms)>
        resolve_account_at;
    // Hosted access tokens are short-lived credentials. Require a signed issuance time and
    // reject provider misconfiguration that creates effectively permanent bearer tokens.
    std::chrono::seconds max_token_lifetime{std::chrono::hours(24)};
};

// Verifies compact OIDC JWTs at the C++ authorization boundary. Only RS256 is accepted in this
// first hosted slice; the caller must provide an HTTPS/JWKS loader and map verified subjects to
// account owners. Malformed, expired, wrongly-issued, or unverifiable tokens return nullopt.
class OidcTokenVerifier final {
  public:
    explicit OidcTokenVerifier(OidcTokenVerifierOptions options);
    ~OidcTokenVerifier();

    OidcTokenVerifier(const OidcTokenVerifier&) = delete;
    OidcTokenVerifier& operator=(const OidcTokenVerifier&) = delete;

    [[nodiscard]] std::optional<app::OwnerId> verify(std::string_view token) const;
    // Reauthentication is intentionally separate from ordinary bearer verification. Providers
    // must include a signed auth_time claim, and callers choose the short freshness window.
    [[nodiscard]] std::optional<app::OwnerId>
    verify_fresh(std::string_view token, std::chrono::seconds max_age) const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace pct::service

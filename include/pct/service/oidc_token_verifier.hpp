#pragma once

#include "pct/app/repository.hpp"

#include <cstdint>
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

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace pct::service

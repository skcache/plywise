#pragma once

#include "pct/app/repository.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "pct/common/json.hpp"

namespace pct::app {

struct HostedAccount {
    std::string id;

    [[nodiscard]] OwnerId owner() const { return OwnerId::account(id); }
    bool operator==(const HostedAccount&) const = default;
};

struct GuestSession {
    std::string id;
    std::int64_t expires_at_ms{0};

    bool operator==(const GuestSession&) const = default;
};

struct GuestClaimReceipt {
    std::string guest_id;
    std::string account_id;
    std::size_t transferred_games{0};
    bool already_claimed{false};
};

struct AccountExport {
    std::string request_id;
    json::Value data;
    std::int64_t completed_at_ms{0};
};

struct AccountDeletionReceipt {
    std::string request_id;
    std::string receipt_token;
    std::int64_t completed_at_ms{0};
    std::int64_t backup_retention_until_ms{0};
};

// Persistence for verified account subjects and opaque guest proofs. The caller owns token
// generation and verification; this class stores only a token hash and performs the atomic
// guest-to-account transfer.
class HostedIdentityStore final {
  public:
    explicit HostedIdentityStore(std::string connection_string);
    ~HostedIdentityStore();

    HostedIdentityStore(const HostedIdentityStore&) = delete;
    HostedIdentityStore& operator=(const HostedIdentityStore&) = delete;

    [[nodiscard]] HostedAccount ensure_account(std::string auth_provider,
                                               std::string auth_subject);
    [[nodiscard]] GuestSession create_guest_session(
        std::string guest_id, const std::array<unsigned char, 32>& token_hash,
        std::int64_t expires_at_ms);
    [[nodiscard]] std::optional<OwnerId> owner_for_guest_token(
        const std::array<unsigned char, 32>& token_hash) const;
    [[nodiscard]] GuestClaimReceipt claim_guest(std::string guest_id,
                                                std::string account_id,
                                                std::string idempotency_key);
    [[nodiscard]] AccountExport export_account(std::string account_id,
                                               std::string idempotency_key);
    [[nodiscard]] AccountDeletionReceipt delete_account(std::string account_id,
                                                        std::string idempotency_key);

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace pct::app

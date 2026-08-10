#include "test.hpp"

#ifdef PCT_HAS_OIDC

#include "pct/common/json.hpp"
#include "pct/service/oidc_token_verifier.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace pct;

namespace {

using Bytes = std::vector<unsigned char>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

std::string encode_base64url(const Bytes& value) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve((value.size() * 4U + 2U) / 3U);
    std::uint32_t accumulator = 0;
    unsigned int bits = 0;
    for (const unsigned char byte : value) {
        accumulator = (accumulator << 8U) | byte;
        bits += 8U;
        while (bits >= 6U) {
            bits -= 6U;
            result.push_back(alphabet[(accumulator >> bits) & 0x3fU]);
            accumulator = bits == 0U ? 0U : accumulator & ((1U << bits) - 1U);
        }
    }
    if (bits > 0U)
        result.push_back(alphabet[(accumulator << (6U - bits)) & 0x3fU]);
    return result;
}

Bytes bignum_bytes(const BIGNUM* value) {
    Bytes result(static_cast<std::size_t>(BN_num_bytes(value)));
    CHECK(BN_bn2bin(value, result.data()) == static_cast<int>(result.size()));
    return result;
}

struct SigningKey {
    PkeyPtr pkey{nullptr, &EVP_PKEY_free};
    std::string jwks;
};

SigningKey make_signing_key(std::string_view kid) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), &EVP_PKEY_CTX_free);
    CHECK(context != nullptr);
    CHECK(EVP_PKEY_keygen_init(context.get()) == 1);
    std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(
        OSSL_PARAM_BLD_new(), &OSSL_PARAM_BLD_free);
    CHECK(builder != nullptr);
    CHECK(OSSL_PARAM_BLD_push_uint(builder.get(), OSSL_PKEY_PARAM_RSA_BITS, 2048U) == 1);
    std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> parameters(
        OSSL_PARAM_BLD_to_param(builder.get()), &OSSL_PARAM_free);
    CHECK(parameters != nullptr);
    CHECK(EVP_PKEY_CTX_set_params(context.get(), parameters.get()) == 1);
    PkeyPtr pkey(nullptr, &EVP_PKEY_free);
    EVP_PKEY* generated = nullptr;
    CHECK(EVP_PKEY_keygen(context.get(), &generated) == 1);
    pkey.reset(generated);
    BIGNUM* modulus_raw = nullptr;
    BIGNUM* public_exponent_raw = nullptr;
    CHECK(EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_N, &modulus_raw) == 1);
    CHECK(EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_E, &public_exponent_raw) == 1);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> modulus(modulus_raw, &BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> public_exponent(public_exponent_raw, &BN_free);
    const std::string encoded_modulus = encode_base64url(bignum_bytes(modulus.get()));
    const std::string encoded_exponent = encode_base64url(bignum_bytes(public_exponent.get()));
    const std::string jwks = json::dump(json::Value::Object{
        {"keys", json::Value::Array{json::Value::Object{
                     {"kty", "RSA"},
                     {"alg", "RS256"},
                     {"kid", std::string(kid)},
                     {"n", encoded_modulus},
                     {"e", encoded_exponent},
                 }}}});
    return SigningKey{std::move(pkey), jwks};
}

std::string sign_token(EVP_PKEY* pkey, std::string_view kid, double expiration,
                       std::string_view issuer = "https://issuer.example",
                       std::string_view audience = "plywise-web",
                       std::optional<double> auth_time = std::nullopt,
                       std::optional<double> issued_at = std::nullopt) {
    const std::string header = json::dump(json::Value::Object{
        {"alg", "RS256"},
        {"kid", std::string(kid)},
        {"typ", "JWT"},
    });
    json::Value payload_value = json::Value::Object{
        {"aud", std::string(audience)},
        {"exp", expiration},
        {"iat", issued_at.value_or(expiration - 300)},
        {"iss", std::string(issuer)},
        {"sub", "subject-123"},
    };
    if (auth_time)
        payload_value.as_object().emplace("auth_time", *auth_time);
    const std::string payload = json::dump(payload_value);
    const std::string signed_data = encode_base64url(Bytes(header.begin(), header.end())) + "." +
                                    encode_base64url(Bytes(payload.begin(), payload.end()));
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                     &EVP_MD_CTX_free);
    CHECK(context != nullptr);
    CHECK(EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, pkey) == 1);
    CHECK(EVP_DigestSignUpdate(context.get(), signed_data.data(), signed_data.size()) == 1);
    std::size_t signature_size = 0;
    CHECK(EVP_DigestSignFinal(context.get(), nullptr, &signature_size) == 1);
    Bytes signature(signature_size);
    CHECK(EVP_DigestSignFinal(context.get(), signature.data(), &signature_size) == 1);
    signature.resize(signature_size);
    return signed_data + "." + encode_base64url(signature);
}

} // namespace

TEST_CASE("OIDC verifier accepts a valid RS256 token and resolves its account") {
    const SigningKey key = make_signing_key("key-1");
    const double now = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::size_t loads = 0;
    service::OidcTokenVerifier verifier(service::OidcTokenVerifierOptions{
        "https://issuer.example",
        "plywise-web",
        "test",
        0,
        [&] {
            ++loads;
            return std::optional<std::string>(key.jwks);
        },
        [](std::string_view provider, std::string_view subject) -> std::optional<app::OwnerId> {
            if (provider == "test" && subject == "subject-123")
                return app::OwnerId::account("acct-test");
            return std::nullopt;
        },
    });

    const auto owner = verifier.verify(sign_token(key.pkey.get(), "key-1", now + 300));
    CHECK(owner.has_value());
    CHECK_EQ(owner->value(), "acct-test");
    CHECK(owner->kind() == app::OwnerKind::Account);
    CHECK_EQ(loads, 1ULL);
    CHECK(verifier.verify("not-a-jwt").has_value() == false);
    CHECK_EQ(loads, 1ULL);

    // An unknown key id is untrusted input. It may trigger one rotation refresh, but repeated
    // misses must stay inside the cooldown instead of causing one JWKS request per token.
    const std::string unknown_key = sign_token(key.pkey.get(), "unknown-key", now + 300);
    CHECK(!verifier.verify(unknown_key).has_value());
    CHECK(!verifier.verify(unknown_key).has_value());
    CHECK_EQ(loads, 2ULL);
}

TEST_CASE("OIDC verifier does not refresh repeatedly for cold-cache unknown kids") {
    const SigningKey key = make_signing_key("key-1");
    const double now = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::size_t loads = 0;
    service::OidcTokenVerifier verifier(service::OidcTokenVerifierOptions{
        "https://issuer.example",
        "plywise-web",
        "test",
        0,
        [&] {
            ++loads;
            return std::optional<std::string>(key.jwks);
        },
        [](std::string_view, std::string_view) {
            return std::optional<app::OwnerId>(app::OwnerId::account("acct-test"));
        },
    });

    CHECK(!verifier.verify(sign_token(key.pkey.get(), "missing-a", now + 300)).has_value());
    CHECK(!verifier.verify(sign_token(key.pkey.get(), "missing-b", now + 300)).has_value());
    CHECK_EQ(loads, 1ULL);
}

TEST_CASE("OIDC verifier rejects expired, forged, and wrongly issued tokens") {
    const SigningKey key = make_signing_key("key-1");
    const double now = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    service::OidcTokenVerifier verifier(service::OidcTokenVerifierOptions{
        "https://issuer.example",
        "plywise-web",
        "test",
        0,
        [&] { return std::optional<std::string>(key.jwks); },
        [](std::string_view, std::string_view) {
            return std::optional<app::OwnerId>(app::OwnerId::account("acct-test"));
        },
    });

    CHECK(!verifier.verify(sign_token(key.pkey.get(), "key-1", now - 1)).has_value());
    CHECK(!verifier
               .verify(sign_token(key.pkey.get(), "key-1", now + 300, "https://other.example"))
               .has_value());
    std::string forged = sign_token(key.pkey.get(), "key-1", now + 300);
    forged.back() = forged.back() == 'A' ? 'B' : 'A';
    CHECK(!verifier.verify(forged).has_value());
    CHECK(!verifier.verify(sign_token(key.pkey.get(), "key-1", now + 90000,
                                      "https://issuer.example", "plywise-web", std::nullopt,
                                      now))
               .has_value());
    CHECK(!verifier.verify(sign_token(key.pkey.get(), "key-1", now + 300,
                                      "https://issuer.example", "plywise-web", std::nullopt,
                                      now + 301))
               .has_value());
}

TEST_CASE("OIDC fresh verification requires a recent signed auth_time claim") {
    const SigningKey key = make_signing_key("key-1");
    const double now = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    service::OidcTokenVerifier verifier(service::OidcTokenVerifierOptions{
        "https://issuer.example",
        "plywise-web",
        "test",
        0,
        [&] { return std::optional<std::string>(key.jwks); },
        [](std::string_view, std::string_view) {
            return std::optional<app::OwnerId>(app::OwnerId::account("acct-test"));
        },
    });

    const auto fresh = verifier.verify_fresh(
        sign_token(key.pkey.get(), "key-1", now + 300, "https://issuer.example", "plywise-web",
                   now - 60),
        std::chrono::minutes(5));
    CHECK(fresh.has_value());
    CHECK(!verifier
               .verify_fresh(sign_token(key.pkey.get(), "key-1", now + 300),
                             std::chrono::minutes(5))
               .has_value());
    CHECK(!verifier
               .verify_fresh(sign_token(key.pkey.get(), "key-1", now + 300,
                                        "https://issuer.example", "plywise-web", now - 600),
                             std::chrono::minutes(5))
               .has_value());
}

#endif

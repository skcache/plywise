#include "pct/service/oidc_token_verifier.hpp"

#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pct::service {
namespace {

constexpr std::size_t max_token_size = 16U * 1024U;
constexpr std::size_t max_jwks_size = 512U * 1024U;
constexpr std::size_t max_key_count = 32;
constexpr std::size_t max_kid_size = 256;
constexpr std::size_t max_subject_size = 512;
constexpr std::chrono::minutes key_cache_ttl{10};

using Bytes = std::vector<unsigned char>;

std::optional<unsigned char> base64url_digit(char character) {
    if (character >= 'A' && character <= 'Z')
        return static_cast<unsigned char>(character - 'A');
    if (character >= 'a' && character <= 'z')
        return static_cast<unsigned char>(character - 'a' + 26);
    if (character >= '0' && character <= '9')
        return static_cast<unsigned char>(character - '0' + 52);
    if (character == '-')
        return 62;
    if (character == '_')
        return 63;
    return std::nullopt;
}

std::optional<Bytes> decode_base64url(std::string_view value) {
    if (value.empty() || value.find('=') != std::string_view::npos || value.size() % 4 == 1)
        return std::nullopt;
    Bytes result;
    result.reserve(value.size() * 3U / 4U);
    unsigned int accumulator = 0;
    unsigned int bits = 0;
    for (const char character : value) {
        const auto digit = base64url_digit(character);
        if (!digit)
            return std::nullopt;
        accumulator = (accumulator << 6U) | *digit;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            result.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xffU));
            accumulator = bits == 0U ? 0U : accumulator & ((1U << bits) - 1U);
        }
    }
    if (bits >= 6U || (bits > 0U && (accumulator & ((1U << bits) - 1U)) != 0U))
        return std::nullopt;
    return result;
}

std::string_view next_segment(std::string_view& value) {
    const std::size_t separator = value.find('.');
    const std::string_view segment = value.substr(0, separator);
    if (separator == std::string_view::npos)
        value = {};
    else
        value.remove_prefix(separator + 1);
    return segment;
}

bool bounded_text(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20U && character != 0x7fU;
    });
}

bool finite_integer(double value) {
    return std::isfinite(value) && value >= 0.0 && std::floor(value) == value &&
           value <= static_cast<double>(std::numeric_limits<std::int64_t>::max());
}

std::optional<std::string> string_claim(const json::Value& object, std::string_view key,
                                        std::size_t maximum) {
    const json::Value value = object.get(key, json::Value{});
    if (!value.is_string() || !bounded_text(value.as_string(), maximum))
        return std::nullopt;
    return value.as_string();
}

bool audience_matches(const json::Value& payload, std::string_view expected) {
    const json::Value audience = payload.get("aud", json::Value{});
    if (audience.is_string())
        return audience.as_string() == expected;
    if (!audience.is_array() || audience.as_array().size() > 32)
        return false;
    for (const auto& item : audience.as_array()) {
        if (item.is_string() && item.as_string() == expected)
            return true;
    }
    return false;
}

struct Jwk {
    Bytes modulus;
    Bytes exponent;
};

using JwkSet = std::map<std::string, Jwk>;

std::optional<JwkSet> parse_jwks(std::string_view text) {
    if (text.empty() || text.size() > max_jwks_size)
        return std::nullopt;
    try {
        const json::Value root = json::parse(text);
        const auto& keys = root.at("keys");
        if (!keys.is_array() || keys.as_array().empty() || keys.as_array().size() > max_key_count)
            return std::nullopt;
        JwkSet result;
        for (const auto& value : keys.as_array()) {
            if (!value.is_object())
                return std::nullopt;
            const auto kid = string_claim(value, "kid", max_kid_size);
            const auto kty = string_claim(value, "kty", 16);
            const auto alg = string_claim(value, "alg", 16);
            const auto modulus = string_claim(value, "n", 1024);
            const auto exponent = string_claim(value, "e", 32);
            if (!kid || !kty || *kty != "RSA" || (alg && *alg != "RS256") || !modulus ||
                !exponent)
                continue;
            const auto decoded_modulus = decode_base64url(*modulus);
            const auto decoded_exponent = decode_base64url(*exponent);
            // Keep RSA work bounded and require at least a 2048-bit modulus.
            if (!decoded_modulus || decoded_modulus->size() < 256 ||
                decoded_modulus->size() > 512 || !decoded_exponent ||
                decoded_exponent->empty() || decoded_exponent->size() > 8 ||
                (decoded_exponent->back() & 1U) == 0U ||
                (decoded_exponent->size() == 1U && decoded_exponent->front() <= 1U))
                continue;
            if (!result.emplace(*kid, Jwk{*decoded_modulus, *decoded_exponent}).second)
                return std::nullopt;
        }
        return result.empty() ? std::nullopt : std::optional<JwkSet>(std::move(result));
    } catch (const Error&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

EVP_PKEY* rsa_public_key(const Jwk& jwk) {
    std::unique_ptr<BIGNUM, decltype(&BN_free)> modulus(
        BN_bin2bn(jwk.modulus.data(), static_cast<int>(jwk.modulus.size()), nullptr), &BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> exponent(
        BN_bin2bn(jwk.exponent.data(), static_cast<int>(jwk.exponent.size()), nullptr), &BN_free);
    if (modulus == nullptr || exponent == nullptr)
        return nullptr;
    std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(
        OSSL_PARAM_BLD_new(), &OSSL_PARAM_BLD_free);
    if (builder == nullptr ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulus.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponent.get()) != 1)
        return nullptr;
    std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> parameters(
        OSSL_PARAM_BLD_to_param(builder.get()), &OSSL_PARAM_free);
    if (parameters == nullptr)
        return nullptr;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), &EVP_PKEY_CTX_free);
    if (context == nullptr || EVP_PKEY_fromdata_init(context.get()) != 1)
        return nullptr;
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_fromdata(context.get(), &key, EVP_PKEY_PUBLIC_KEY, parameters.get()) != 1)
        return nullptr;
    return key;
}

bool verify_rsa_sha256(const Jwk& jwk, std::string_view signed_data, const Bytes& signature) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rsa_public_key(jwk), &EVP_PKEY_free);
    if (key == nullptr)
        return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return false;
    }
    const bool initialized = EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key.get()) == 1;
    const bool updated = initialized && EVP_DigestVerifyUpdate(context, signed_data.data(), signed_data.size()) == 1;
    const bool verified = updated && EVP_DigestVerifyFinal(context, signature.data(), signature.size()) == 1;
    EVP_MD_CTX_free(context);
    return verified;
}

} // namespace

struct OidcTokenVerifier::Impl {
    explicit Impl(OidcTokenVerifierOptions input) : options(std::move(input)) {
        if (!options.issuer.starts_with("https://") || !bounded_text(options.issuer, 2048) ||
            !bounded_text(options.audience, 512) ||
            !bounded_text(options.provider, 128) || !options.load_jwks ||
            !options.resolve_account || options.clock_skew_seconds > 300) {
            throw Error(ErrorCode::InvalidArgument, "OIDC verifier configuration is invalid");
        }
    }

    std::optional<Jwk> key_for(std::string_view kid) const {
        const auto needs_refresh = [&] {
            return !keys_loaded ||
                   std::chrono::steady_clock::now() - keys_loaded_at > key_cache_ttl;
        };
        {
            std::lock_guard lock(mutex);
            if (!needs_refresh()) {
                const auto found = keys.find(std::string(kid));
                if (found != keys.end())
                    return found->second;
            }
        }
        std::optional<JwkSet> loaded;
        try {
            loaded = parse_jwks(options.load_jwks().value_or(std::string{}));
        } catch (...) {
            loaded = std::nullopt;
        }
        if (!loaded)
            return std::nullopt;
        std::lock_guard lock(mutex);
        keys = std::move(*loaded);
        keys_loaded = true;
        keys_loaded_at = std::chrono::steady_clock::now();
        const auto found = keys.find(std::string(kid));
        return found == keys.end() ? std::nullopt : std::optional<Jwk>(found->second);
    }

    OidcTokenVerifierOptions options;
    mutable std::mutex mutex;
    mutable bool keys_loaded{false};
    mutable std::chrono::steady_clock::time_point keys_loaded_at{};
    mutable JwkSet keys;
};

OidcTokenVerifier::OidcTokenVerifier(OidcTokenVerifierOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

OidcTokenVerifier::~OidcTokenVerifier() = default;

std::optional<app::OwnerId> OidcTokenVerifier::verify(std::string_view token) const {
    if (token.empty() || token.size() > max_token_size ||
        std::any_of(token.begin(), token.end(), [](unsigned char character) {
            return character < 0x21U || character == 0x7fU;
        }))
        return std::nullopt;
    std::string_view remaining = token;
    const std::string_view encoded_header = next_segment(remaining);
    const std::string_view encoded_payload = next_segment(remaining);
    const std::string_view encoded_signature = remaining;
    if (encoded_header.empty() || encoded_payload.empty() || encoded_signature.empty() ||
        encoded_signature.find('.') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto header_bytes = decode_base64url(encoded_header);
    const auto payload_bytes = decode_base64url(encoded_payload);
    const auto signature = decode_base64url(encoded_signature);
    if (!header_bytes || !payload_bytes || !signature || signature->empty()) {
        return std::nullopt;
    }

    try {
        const json::Value header = json::parse(
            std::string_view(reinterpret_cast<const char*>(header_bytes->data()), header_bytes->size()));
        const json::Value payload = json::parse(
            std::string_view(reinterpret_cast<const char*>(payload_bytes->data()), payload_bytes->size()));
        const auto algorithm = string_claim(header, "alg", 16);
        const auto kid = string_claim(header, "kid", max_kid_size);
        const auto issuer = string_claim(payload, "iss", 2048);
        const auto subject = string_claim(payload, "sub", max_subject_size);
        if (!algorithm || *algorithm != "RS256" || !kid || !issuer || *issuer != impl_->options.issuer ||
            !subject || !audience_matches(payload, impl_->options.audience)) {
            return std::nullopt;
        }

        const json::Value expiration = payload.get("exp", json::Value{});
        if (!expiration.is_number() || !finite_integer(expiration.as_number())) {
            return std::nullopt;
        }
        const double now = static_cast<double>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const double skew = static_cast<double>(impl_->options.clock_skew_seconds);
        if (expiration.as_number() <= now - skew) {
            return std::nullopt;
        }

        const json::Value not_before = payload.get("nbf", json::Value{});
        if (!not_before.is_null() &&
            (!not_before.is_number() || !finite_integer(not_before.as_number()) ||
             not_before.as_number() > now + skew))
            return std::nullopt;

        const auto key = impl_->key_for(*kid);
        if (!key) {
            return std::nullopt;
        }
        const std::string signed_data(token.substr(0, token.size() - encoded_signature.size() - 1));
        if (!verify_rsa_sha256(*key, signed_data, *signature)) {
            return std::nullopt;
        }
        const auto owner = impl_->options.resolve_account(impl_->options.provider, *subject);
        if (!owner || owner->kind() != app::OwnerKind::Account)
            return std::nullopt;
        return owner;
    } catch (const Error&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<app::OwnerId> OidcTokenVerifier::verify_fresh(
    std::string_view token, std::chrono::seconds max_age) const {
    if (max_age <= std::chrono::seconds::zero() || max_age > std::chrono::minutes(15))
        return std::nullopt;
    const auto owner = verify(token);
    if (!owner)
        return std::nullopt;

    std::string_view remaining = token;
    static_cast<void>(next_segment(remaining));
    const std::string_view encoded_payload = next_segment(remaining);
    const auto payload_bytes = decode_base64url(encoded_payload);
    if (!payload_bytes)
        return std::nullopt;
    try {
        const json::Value payload = json::parse(
            std::string_view(reinterpret_cast<const char*>(payload_bytes->data()),
                             payload_bytes->size()));
        const json::Value auth_time = payload.get("auth_time", json::Value{});
        if (!auth_time.is_number() || !finite_integer(auth_time.as_number()))
            return std::nullopt;
        const double now = static_cast<double>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const double skew = static_cast<double>(impl_->options.clock_skew_seconds);
        const double timestamp = auth_time.as_number();
        if (timestamp > now + skew || timestamp < now - static_cast<double>(max_age.count()) - skew)
            return std::nullopt;
        return owner;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace pct::service

#pragma once

#include <string>

namespace pct::app {

// Hosted PostgreSQL connections must state a TLS mode when they leave the local machine. Local
// loopback/socket connections remain available for development and CI; remote connections fail
// closed instead of silently accepting libpq's default `sslmode=prefer`.
[[nodiscard]] std::string validate_postgres_connection_security(std::string connection_string);

} // namespace pct::app

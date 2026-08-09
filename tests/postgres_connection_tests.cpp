#include "test.hpp"

#include "pct/app/postgres_connection.hpp"

using namespace pct;

TEST_CASE("PostgreSQL connection security allows local development connections") {
    CHECK_EQ(app::validate_postgres_connection_security(
                  "postgresql://postgres:password@127.0.0.1:5432/plywise"),
              "postgresql://postgres:password@127.0.0.1:5432/plywise");
    CHECK_EQ(app::validate_postgres_connection_security(
                  "host=localhost dbname=plywise user=postgres"),
              "host=localhost dbname=plywise user=postgres");
}

TEST_CASE("PostgreSQL connection security rejects remote plaintext defaults") {
    CHECK_THROWS(app::validate_postgres_connection_security(
        "postgresql://user:password@db.example/plywise"));
    CHECK_THROWS(app::validate_postgres_connection_security(
        "host=db.example dbname=plywise sslmode=disable"));
    CHECK_EQ(app::validate_postgres_connection_security(
                  "postgresql://user:password@db.example/plywise?sslmode=verify-full"),
              "postgresql://user:password@db.example/plywise?sslmode=verify-full");
}

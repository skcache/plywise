import assert from "node:assert/strict";
import { createConfig } from "../../vercel.mjs";

function env(overrides = {}) {
  return {
    VERCEL_ENV: "production",
    PCT_PLYWISE_API_ORIGIN: "https://api.example.test",
    VITE_PLYWISE_EVENT_ORIGIN: "wss://events.example.test",
    VITE_SUPABASE_URL: "https://auth.example.test",
    VITE_SUPABASE_PUBLISHABLE_KEY: "sb_publishable_test",
    ...overrides,
  };
}

const proxied = createConfig(env());
assert.equal(proxied.rewrites[0].source, "/api/:path*");
assert.equal(proxied.rewrites[0].destination, "https://api.example.test/api/:path*");
const proxiedCsp = proxied.headers[0].headers.find((header) => header.key === "Content-Security-Policy").value;
assert.match(proxiedCsp, /connect-src 'self'/);
assert.match(proxiedCsp, /https:\/\/auth\.example\.test/);
assert.equal(
  proxied.headers.find((entry) => entry.source === "/api/:path*").headers[0].value,
  "no-store",
);

const direct = createConfig(env({
  PCT_PLYWISE_API_ORIGIN: "",
  VITE_PLYWISE_API_ORIGIN: "https://api.example.test",
  VITE_PLYWISE_EVENT_ORIGIN: "wss://events.example.test",
}));
assert.equal(direct.rewrites[0].source, "/auth/callback");
const directCsp = direct.headers[0].headers.find((header) => header.key === "Content-Security-Policy").value;
assert.match(directCsp, /https:\/\/api\.example\.test/);
assert.match(directCsp, /wss:\/\/events\.example\.test/);

assert.throws(
  () => createConfig(env({ PCT_PLYWISE_API_ORIGIN: "", VITE_PLYWISE_API_ORIGIN: "" })),
  /require PCT_PLYWISE_API_ORIGIN/,
);
const staticOnly = createConfig(env({
  PCT_PLYWISE_API_ORIGIN: "",
  PCT_ALLOW_STATIC_ONLY_DEPLOYMENT: "true",
}));
assert.equal(staticOnly.rewrites[0].source, "/auth/callback");
assert.match(
  staticOnly.headers[0].headers.find((header) => header.key === "Content-Security-Policy").value,
  /https:\/\/auth\.example\.test/,
);
assert.throws(
  () => createConfig(env({ VITE_PLYWISE_EVENT_ORIGIN: "" })),
  /require VITE_PLYWISE_EVENT_ORIGIN/,
);
assert.throws(
  () => createConfig(env({ PCT_PLYWISE_API_ORIGIN: "http://api.example.test" })),
  /credential-free https: origin/,
);
assert.throws(
  () => createConfig(env({ VITE_SUPABASE_URL: "" })),
  /require VITE_SUPABASE_URL and VITE_SUPABASE_PUBLISHABLE_KEY/,
);
assert.throws(
  () => createConfig(env({ VITE_SUPABASE_PUBLISHABLE_KEY: "" })),
  /require VITE_SUPABASE_URL and VITE_SUPABASE_PUBLISHABLE_KEY/,
);

console.log("vercel config tests passed");

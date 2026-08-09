import fs from "node:fs";
import path from "node:path";
import { validatedPublicOrigins } from "./check-public-env.mjs";

const connectSources = new Set(["'self'"]);
if (validatedPublicOrigins.api) connectSources.add(validatedPublicOrigins.api);
if (validatedPublicOrigins.events) {
  connectSources.add(validatedPublicOrigins.events);
} else if (validatedPublicOrigins.api) {
  const events = new URL(validatedPublicOrigins.api);
  events.protocol = events.protocol === "https:" ? "wss:" : "ws:";
  connectSources.add(events.origin);
}

const contentSecurityPolicy = [
  "default-src 'self'",
  "base-uri 'self'",
  "object-src 'none'",
  "form-action 'self'",
  "frame-ancestors 'none'",
  "script-src 'self'",
  "worker-src 'self'",
  "style-src 'self'",
  "img-src 'self' data:",
  "font-src 'self'",
  `connect-src ${[...connectSources].join(" ")}`,
].join("; ");

const headers = `/*
  Content-Security-Policy: ${contentSecurityPolicy}
  Permissions-Policy: camera=(), microphone=(), geolocation=()
  Referrer-Policy: no-referrer
  X-Content-Type-Options: nosniff
  X-Frame-Options: DENY

/assets/*
  Cache-Control: public, max-age=31536000, immutable
`;

const output = path.resolve("dist", "_headers");
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, headers);

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
if (validatedPublicOrigins.supabase) connectSources.add(validatedPublicOrigins.supabase);

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

function escapeHtmlAttribute(value) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll('"', "&quot;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

const indexOutput = path.resolve("dist", "index.html");
const indexHtml = fs.readFileSync(indexOutput, "utf8");
const cspMeta = `<meta http-equiv="Content-Security-Policy" content="${escapeHtmlAttribute(contentSecurityPolicy)}" />`;
if (!indexHtml.includes('http-equiv="Content-Security-Policy"')) {
  fs.writeFileSync(indexOutput, indexHtml.replace("</head>", `    ${cspMeta}\n  </head>`));
}

for (const privateBuildFile of ["pieces/ASSET_AUDIT.md"]) {
  fs.rmSync(path.resolve("dist", privateBuildFile), { force: true });
}

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

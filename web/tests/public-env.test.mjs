import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const script = new URL("../scripts/check-public-env.mjs", import.meta.url);
const cleanDirectory = mkdtempSync(join(tmpdir(), "plywise-public-env-"));

function legacyKey(role) {
  const encode = (value) => Buffer.from(JSON.stringify(value)).toString("base64url");
  return `${encode({ alg: "HS256", typ: "JWT" })}.${encode({ role })}.synthetic-signature`;
}

function run(key) {
  try {
    execFileSync(process.execPath, [script.pathname, "--mode", "production"], {
      cwd: cleanDirectory,
      env: {
        PATH: process.env.PATH,
        VITE_SUPABASE_URL: "https://auth.example.test",
        VITE_SUPABASE_PUBLISHABLE_KEY: key,
      },
      stdio: "pipe",
    });
    return true;
  } catch {
    return false;
  }
}

try {
  assert.equal(run("sb_publishable_synthetic-public-key"), true);
  assert.equal(run(legacyKey("anon")), true);
  assert.equal(run(legacyKey("service_role")), false);
  assert.equal(run("not-a-public-supabase-key"), false);
} finally {
  rmSync(cleanDirectory, { recursive: true, force: true });
}

console.log("public environment tests passed");

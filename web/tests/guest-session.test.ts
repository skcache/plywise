import { clearGuestSession, loadGuestSession, saveGuestSession } from "../src/guest-session";

function assert(actual: unknown, expected: unknown, label: string): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

class MemoryStorage {
  private values = new Map<string, string>();
  getItem(key: string): string | null { return this.values.get(key) ?? null; }
  setItem(key: string, value: string): void { this.values.set(key, value); }
  removeItem(key: string): void { this.values.delete(key); }
}

const storage = new MemoryStorage();
const token = "a".repeat(64);
const saved = saveGuestSession({ guestId: "guest-test", token, expiresAtMs: 1000 }, storage);
assert(saved.version, 1, "guest sessions are versioned");
assert(loadGuestSession(storage, 0)?.guestId, "guest-test", "saved session reloads");
assert(loadGuestSession(storage, 1001), null, "expired session is removed");

let rejected = false;
try {
  saveGuestSession({ guestId: "guest-test", token: "not-a-token", expiresAtMs: 2000 }, storage);
} catch {
  rejected = true;
}
assert(rejected, true, "invalid guest tokens are rejected");

saveGuestSession({ guestId: "guest-test", token, expiresAtMs: 3000 }, storage);
clearGuestSession(storage);
assert(loadGuestSession(storage, 0), null, "clearing removes the session");

console.log("guest session tests passed");

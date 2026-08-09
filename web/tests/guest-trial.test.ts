import { GUEST_TRIAL_RETENTION_MS, loadGuestTrial, markGuestAnalysisUsed, releaseGuestAnalysis, reserveGuestAnalysis } from "../src/guest-trial";

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
const start = Date.parse("2026-08-09T00:00:00.000Z");
const fresh = loadGuestTrial(storage, start);
assert(fresh.status, "available", "new guests start with one review");
assert(Date.parse(fresh.expiresAt), start + GUEST_TRIAL_RETENTION_MS, "guest retention is visible and bounded");

const reservation = reserveGuestAnalysis("game-one", storage, start + 1);
assert(reservation.ok, true, "first analysis reserves the guest slot");
assert(reserveGuestAnalysis("game-two", storage, start + 2).reason, "already_reserved", "concurrent second analysis is blocked");
assert(markGuestAnalysisUsed("game-one", storage, start + 3).status, "used", "started analysis consumes the slot");
assert(reserveGuestAnalysis("game-two", storage, start + 4).reason, "already_used", "used guest cannot start another analysis");

const releaseStorage = new MemoryStorage();
const releaseStart = loadGuestTrial(releaseStorage, start);
reserveGuestAnalysis("failed-game", releaseStorage, start + 1);
assert(releaseGuestAnalysis("failed-game", releaseStorage, start + 2).status, "available", "failed start releases the reserved slot");
assert(loadGuestTrial(releaseStorage, start + 3).guestId, releaseStart.guestId, "release keeps the guest identity");

const expiredStorage = new MemoryStorage();
loadGuestTrial(expiredStorage, start);
assert(loadGuestTrial(expiredStorage, start + GUEST_TRIAL_RETENTION_MS + 1).status, "available", "expired retention resets the guest slot");

console.log("guest trial tests passed");

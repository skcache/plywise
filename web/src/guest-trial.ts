export type GuestTrialStatus = "available" | "reserved" | "used";

export type GuestTrialState = {
  version: 1;
  guestId: string;
  status: GuestTrialStatus;
  expiresAt: string;
  gameId?: string;
};

type StorageLike = Pick<Storage, "getItem" | "setItem" | "removeItem">;

export type GuestTrialResult = {
  ok: boolean;
  state: GuestTrialState;
  reason?: "already_reserved" | "already_used";
};

export const GUEST_TRIAL_RETENTION_MS = 24 * 60 * 60 * 1000;
const guestTrialKey = "plywise-guest-trial-v1";

function newGuestId(): string {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") return crypto.randomUUID();
  return `guest-${Math.random().toString(36).slice(2)}-${Date.now().toString(36)}`;
}

function freshState(now: number): GuestTrialState {
  return {
    version: 1,
    guestId: newGuestId(),
    status: "available",
    expiresAt: new Date(now + GUEST_TRIAL_RETENTION_MS).toISOString(),
  };
}

function browserStorage(): StorageLike | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

function saveState(storage: StorageLike | null, state: GuestTrialState): void {
  try { storage?.setItem(guestTrialKey, JSON.stringify(state)); } catch { /* Private browsing can reject local storage. */ }
}

function parseState(raw: string | null, now: number): GuestTrialState | null {
  if (!raw) return null;
  try {
    const value = JSON.parse(raw) as Partial<GuestTrialState>;
    if (value.version !== 1 || typeof value.guestId !== "string" || !value.guestId || !["available", "reserved", "used"].includes(value.status ?? "") || typeof value.expiresAt !== "string") return null;
    const expiresAt = Date.parse(value.expiresAt);
    if (!Number.isFinite(expiresAt) || expiresAt <= now) return null;
    return {
      version: 1,
      guestId: value.guestId,
      status: value.status as GuestTrialStatus,
      expiresAt: value.expiresAt,
      ...(typeof value.gameId === "string" && value.gameId ? { gameId: value.gameId } : {}),
    };
  } catch {
    return null;
  }
}

export function loadGuestTrial(storage: StorageLike | null = browserStorage(), now = Date.now()): GuestTrialState {
  const current = parseState(storage?.getItem(guestTrialKey) ?? null, now);
  if (current) return current;
  const next = freshState(now);
  saveState(storage, next);
  return next;
}

export function reserveGuestAnalysis(gameId: string, storage: StorageLike | null = browserStorage(), now = Date.now()): GuestTrialResult {
  const current = loadGuestTrial(storage, now);
  if (current.status !== "available") {
    return { ok: false, state: current, reason: current.status === "reserved" ? "already_reserved" : "already_used" };
  }
  const next: GuestTrialState = { ...current, status: "reserved", gameId };
  saveState(storage, next);
  return { ok: true, state: next };
}

export function markGuestAnalysisUsed(gameId: string, storage: StorageLike | null = browserStorage(), now = Date.now()): GuestTrialState {
  const current = loadGuestTrial(storage, now);
  if (current.gameId !== gameId || current.status !== "reserved") return current;
  const next: GuestTrialState = { ...current, status: "used" };
  saveState(storage, next);
  return next;
}

export function releaseGuestAnalysis(gameId: string, storage: StorageLike | null = browserStorage(), now = Date.now()): GuestTrialState {
  const current = loadGuestTrial(storage, now);
  if (current.gameId !== gameId || current.status !== "reserved") return current;
  const next: GuestTrialState = { version: 1, guestId: current.guestId, status: "available", expiresAt: current.expiresAt };
  saveState(storage, next);
  return next;
}

export function guestTrialLabel(state: GuestTrialState): string {
  if (state.status === "available") return "One free guest review available";
  if (state.status === "reserved") return "Your guest review is in progress";
  return "Your free guest review is complete";
}

export function guestTrialRetention(state: GuestTrialState, now = Date.now()): string {
  const remaining = Math.max(0, Date.parse(state.expiresAt) - now);
  const hours = Math.max(1, Math.ceil(remaining / (60 * 60 * 1000)));
  return `Kept on this device for about ${hours} more hour${hours === 1 ? "" : "s"}`;
}

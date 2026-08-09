export type GuestSession = {
  version: 1;
  guestId: string;
  token: string;
  expiresAtMs: number;
};

type StorageLike = Pick<Storage, "getItem" | "setItem" | "removeItem">;

const guestSessionKey = "plywise-guest-session-v1";

function browserStorage(): StorageLike | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

function validHexToken(value: unknown): value is string {
  return typeof value === "string" && /^[0-9a-f]{64}$/i.test(value);
}

function parseSession(raw: string | null, now: number): GuestSession | null {
  if (!raw) return null;
  try {
    const value = JSON.parse(raw) as Partial<GuestSession>;
    if (value.version !== 1 || typeof value.guestId !== "string" || !value.guestId ||
        !validHexToken(value.token) || typeof value.expiresAtMs !== "number" ||
        !Number.isSafeInteger(value.expiresAtMs) || value.expiresAtMs <= now) return null;
    return {
      version: 1,
      guestId: value.guestId,
      token: value.token,
      expiresAtMs: value.expiresAtMs,
    };
  } catch {
    return null;
  }
}

export function loadGuestSession(storage: StorageLike | null = browserStorage(), now = Date.now()): GuestSession | null {
  const session = parseSession(storage?.getItem(guestSessionKey) ?? null, now);
  if (!session) {
    try { storage?.removeItem(guestSessionKey); } catch { /* Private browsing can reject storage. */ }
  }
  return session;
}

export function saveGuestSession(
  session: Pick<GuestSession, "guestId" | "token" | "expiresAtMs">,
  storage: StorageLike | null = browserStorage(),
): GuestSession {
  if (!session.guestId || !validHexToken(session.token) || !Number.isSafeInteger(session.expiresAtMs)) {
    throw new Error("Guest session response is invalid.");
  }
  const next: GuestSession = { version: 1, ...session };
  try { storage?.setItem(guestSessionKey, JSON.stringify(next)); } catch { /* Continue without persistence. */ }
  return next;
}

export function clearGuestSession(storage: StorageLike | null = browserStorage()): void {
  try { storage?.removeItem(guestSessionKey); } catch { /* Private browsing can reject storage. */ }
}

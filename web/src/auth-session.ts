import {
  createClient,
  type AuthChangeEvent,
  type Session,
  type SupabaseClient,
} from "@supabase/supabase-js";
import { authRedirectMessage } from "./auth-redirect";

export type AuthProvider = "google" | "apple" | "github";

export type AuthSnapshot = {
  configured: boolean;
  session: Session | null;
  event: AuthChangeEvent | null;
  message: string;
};

export type AuthListener = (snapshot: AuthSnapshot) => void;
export type AuthIntent = {
  context: "landing";
};

const providerLabels: Record<AuthProvider, string> = {
  google: "Google",
  apple: "Apple",
  github: "GitHub",
};

const supabaseUrl = import.meta.env.VITE_SUPABASE_URL?.trim() ?? "";
const supabasePublishableKey = import.meta.env.VITE_SUPABASE_PUBLISHABLE_KEY?.trim() ?? "";
const authStorage = safeSessionStorage();
const client: SupabaseClient | null = supabaseUrl && supabasePublishableKey
  ? createClient(supabaseUrl, supabasePublishableKey, {
      auth: {
        autoRefreshToken: true,
        detectSessionInUrl: true,
        flowType: "pkce",
        // Keep provider sessions scoped to this tab; the server remains the source of account history.
        persistSession: authStorage !== null,
        storage: authStorage ?? undefined,
      },
    })
  : null;

let currentSession: Session | null = null;
let currentMessage = "";
const listeners = new Set<AuthListener>();

if (client) {
  client.auth.onAuthStateChange((event, session) => {
    currentSession = session;
    if (event === "SIGNED_OUT") currentMessage = "You are signed out.";
    else if (event === "TOKEN_REFRESHED") currentMessage = "";
    notify({ configured: true, session, event, message: currentMessage });
  });
}

let initialization: Promise<AuthSnapshot> | null = null;

export function authProviderLabel(provider: AuthProvider): string {
  return providerLabels[provider];
}

export function authConfigured(): boolean {
  return client !== null;
}

export function currentAuthSnapshot(): AuthSnapshot {
  return {
    configured: authConfigured(),
    session: currentSession,
    event: null,
    message: currentMessage,
  };
}

export function subscribeAuth(listener: AuthListener): () => void {
  listeners.add(listener);
  listener(currentAuthSnapshot());
  return () => listeners.delete(listener);
}

export async function initializeAuth(): Promise<AuthSnapshot> {
  if (!client) return currentAuthSnapshot();
  if (!initialization) {
    initialization = client.auth.getSession().then(({ data, error }) => {
      if (error) {
        currentSession = null;
        currentMessage = "Your account session could not be restored. Sign in again.";
      } else {
        currentSession = data.session;
        currentMessage = "";
      }
      const snapshot = { ...currentAuthSnapshot(), event: "INITIAL_SESSION" as AuthChangeEvent };
      notify(snapshot);
      return snapshot;
    }).catch(() => {
      currentSession = null;
      currentMessage = "Your account session could not be restored. Sign in again.";
      const snapshot = { ...currentAuthSnapshot(), event: "INITIAL_SESSION" as AuthChangeEvent };
      notify(snapshot);
      return snapshot;
    });
  }
  return initialization;
}

export async function signInWithProvider(
  provider: AuthProvider,
  returnPath = typeof window === "undefined"
    ? "/"
    : `${window.location.pathname}${window.location.search}${window.location.hash}`,
): Promise<{ ok: boolean; message: string }> {
  if (!client) {
    return { ok: false, message: "Hosted account sign-in is not configured yet." };
  }
  try {
    if (typeof window === "undefined") {
      return { ok: false, message: "Sign-in is only available in a browser." };
    }
    const redirectTo = new URL(returnPath || "/", window.location.origin);
    if (redirectTo.origin !== window.location.origin) {
      return { ok: false, message: "The sign-in return path is invalid." };
    }
    const { error } = await client.auth.signInWithOAuth({
      provider,
      options: { redirectTo: redirectTo.href },
    });
    if (error) {
      return { ok: false, message: `${authProviderLabel(provider)} sign-in could not start. Try again.` };
    }
  } catch {
    return { ok: false, message: `${authProviderLabel(provider)} sign-in could not start. Try again.` };
  }
  return { ok: true, message: `Continuing with ${authProviderLabel(provider)}…` };
}

export async function signOut(): Promise<{ ok: boolean; message: string }> {
  if (!client) return { ok: true, message: "You are signed out." };
  try {
    const { error } = await client.auth.signOut();
    if (error) return { ok: false, message: "Sign out could not complete. Try again." };
    return { ok: true, message: "You are signed out." };
  } catch {
    return { ok: false, message: "Sign out could not complete. Try again." };
  }
}

export async function accountAccessToken(): Promise<string | null> {
  if (!client) return null;
  try {
    const { data, error } = await client.auth.getSession();
    if (error || !data.session) return null;
    currentSession = data.session;
    return data.session.access_token;
  } catch {
    return null;
  }
}

export function cachedAccountAccessToken(): string | null {
  return currentSession?.access_token ?? null;
}

export function saveAuthIntent(intent: AuthIntent): void {
  if (typeof window === "undefined") return;
  try {
    window.sessionStorage.setItem("plywise-auth-intent-v1", JSON.stringify(intent));
  } catch {
    // Private browsing can reject session storage; sign-in can still continue.
  }
}

export function loadAuthIntent(): AuthIntent | null {
  if (typeof window === "undefined") return null;
  try {
    const value = JSON.parse(window.sessionStorage.getItem("plywise-auth-intent-v1") ?? "null") as Partial<AuthIntent> | null;
    if (!value || value.context !== "landing") return null;
    return { context: "landing" };
  } catch {
    return null;
  }
}

export function clearAuthIntent(): void {
  if (typeof window === "undefined") return;
  try {
    window.sessionStorage.removeItem("plywise-auth-intent-v1");
  } catch {
    // Private browsing can reject session storage.
  }
}

export function consumeAuthRedirectMessage(): string | null {
  if (typeof window === "undefined") return null;
  const url = new URL(window.location.href);
  const params = new URLSearchParams(url.search);
  const errorCode = params.get("error_code") ?? params.get("error") ?? "";
  const errorDescription = params.get("error_description") ?? "";
  const hasAuthError = Boolean(errorCode || errorDescription);
  if (!hasAuthError) return null;

  for (const key of ["error", "error_code", "error_description", "error_reason", "error_status"]) {
    params.delete(key);
  }
  url.search = params.toString();
  window.history.replaceState(null, "", `${url.pathname}${url.search}${url.hash}`);

  return authRedirectMessage(errorCode, errorDescription);
}

function notify(snapshot: AuthSnapshot): void {
  for (const listener of listeners) listener(snapshot);
}

function safeSessionStorage(): Storage | null {
  try {
    return typeof window === "undefined" ? null : window.sessionStorage;
  } catch {
    return null;
  }
}

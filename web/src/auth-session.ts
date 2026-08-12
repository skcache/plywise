import {
  createClient,
  type AuthChangeEvent,
  type Session,
  type SupabaseClient,
} from "@supabase/supabase-js";
import { authRedirectMessage, classifyAuthRedirect } from "./auth-redirect";

export type AuthProvider = "google" | "github";
export type AuthEntryMode = "sign-up" | "sign-in";

export type AuthSnapshot = {
  configured: boolean;
  local: boolean;
  session: Session | null;
  event: AuthChangeEvent | null;
  message: string;
};

export type AuthResult = {
  ok: boolean;
  message: string;
};

export type AuthListener = (snapshot: AuthSnapshot) => void;
export type AuthIntent = {
  context: "landing";
  mode: AuthEntryMode;
  destination: "review" | "home";
};

const providerLabels: Record<AuthProvider, string> = {
  google: "Google",
  github: "GitHub",
};

const supabaseUrl = import.meta.env.VITE_SUPABASE_URL?.trim() ?? "";
const supabasePublishableKey = import.meta.env.VITE_SUPABASE_PUBLISHABLE_KEY?.trim() ?? "";
// PKCE password-reset links commonly open in a new tab. Supabase must be able
// to recover the verifier created by the original tab, so use origin-scoped
// local storage when the browser permits it. The API still authorizes every
// request from the short-lived provider session rather than trusting storage.
const authStorage = safeLocalStorage() ?? safeSessionStorage();
const localAuthStorage = authStorage;
const client: SupabaseClient | null = supabaseUrl && supabasePublishableKey
  ? createClient(supabaseUrl, supabasePublishableKey, {
      auth: {
        autoRefreshToken: true,
        // OAuth codes are exchanged explicitly on /auth/callback below. This keeps
        // the callback state visible to one trusted path instead of relying on
        // automatic URL parsing in the client constructor.
        detectSessionInUrl: false,
        flowType: "pkce",
        // Persist the provider session across tabs and reloads; the server remains
        // the source of account history and tenant authorization.
        persistSession: authStorage !== null,
        storage: authStorage ?? undefined,
      },
    })
  : null;
// This is a development-only test harness. It never creates credentials, never
// talks to Supabase, and is disabled in production builds by Vite's DEV constant.
const localAuthMode = import.meta.env.DEV && client === null;
const localAuthStorageKey = "plywise-local-test-session-v1";

let currentSession: Session | null = null;
let currentMessage = "";
const listeners = new Set<AuthListener>();

if (client) {
  client.auth.onAuthStateChange((event, session) => {
    currentSession = session;
    if (event === "SIGNED_OUT") currentMessage = "You are signed out.";
    else if (event === "TOKEN_REFRESHED") currentMessage = "";
    notify({ configured: true, local: false, session, event, message: currentMessage });
  });
}

let initialization: Promise<AuthSnapshot> | null = null;

export function authProviderLabel(provider: AuthProvider): string {
  return providerLabels[provider];
}

export function authConfigured(): boolean {
  return client !== null;
}

export function localAuthEnabled(): boolean {
  return localAuthMode;
}

export function currentAuthSnapshot(): AuthSnapshot {
  return {
    configured: authConfigured(),
    local: localAuthMode,
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
  if (!client) {
    if (localAuthMode) currentSession = loadLocalSession();
    return currentAuthSnapshot();
  }
  if (!initialization) {
    initialization = (async () => {
      const callbackMessage = await completeAuthRedirect();
      if (callbackMessage) {
        currentSession = null;
        currentMessage = callbackMessage;
      } else {
        const { data, error } = await client.auth.getSession();
        if (error) {
          currentSession = null;
          currentMessage = "Your account session could not be restored. Sign in again.";
        } else {
          currentSession = data.session;
          currentMessage = "";
        }
      }
      const snapshot = { ...currentAuthSnapshot(), event: "INITIAL_SESSION" as AuthChangeEvent };
      notify(snapshot);
      return snapshot;
    })().catch(() => {
      currentSession = null;
      currentMessage = "Your account session could not be restored. Sign in again.";
      const snapshot = { ...currentAuthSnapshot(), event: "INITIAL_SESSION" as AuthChangeEvent };
      notify(snapshot);
      return snapshot;
    });
  }
  return initialization;
}

/** Sign into the local repository without creating credentials or contacting Supabase. */
export async function signInWithLocalAccount(): Promise<AuthResult> {
  if (!localAuthMode) return { ok: false, message: "Local test access is only available in development." };
  currentSession = createLocalSession();
  try {
    localAuthStorage?.setItem(localAuthStorageKey, "1");
  } catch {
    // Private browsing can reject storage; the current tab still works.
  }
  currentMessage = "Local test account ready.";
  notify({ ...currentAuthSnapshot(), event: "SIGNED_IN" });
  return { ok: true, message: currentMessage };
}

export async function signInWithProvider(
  provider: AuthProvider,
  mode: AuthEntryMode = "sign-in",
): Promise<AuthResult> {
  if (!client) {
    return { ok: false, message: "Hosted account sign-in is not configured yet." };
  }
  try {
    if (typeof window === "undefined") {
      return { ok: false, message: "Sign-in is only available in a browser." };
    }
    const redirectTo = new URL("/auth/callback", window.location.origin);
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
  return {
    ok: true,
    message: mode === "sign-up"
      ? `Continuing account setup with ${authProviderLabel(provider)}…`
      : `Continuing with ${authProviderLabel(provider)}…`,
  };
}

export async function signUpWithPassword(name: string, email: string, password: string): Promise<AuthResult> {
  if (!client) return { ok: false, message: "Hosted account sign-in is not configured yet." };
  try {
    const { data, error } = await client.auth.signUp({
      email: normalizeEmail(email),
      password,
      options: { data: { name: name.trim() } },
    });
    if (error) return { ok: false, message: signUpErrorMessage(error) };
    currentSession = data.session;
    currentMessage = data.session
      ? "Your account is ready."
      : "Check your email to confirm your account, then sign in.";
    notify({ ...currentAuthSnapshot(), event: data.session ? "SIGNED_IN" : null });
    return { ok: true, message: currentMessage };
  } catch {
    return { ok: false, message: "We couldn't create that account. Check your details and try again." };
  }
}

export async function signInWithPassword(email: string, password: string): Promise<AuthResult> {
  if (!client) return { ok: false, message: "Hosted account sign-in is not configured yet." };
  try {
    const { data, error } = await client.auth.signInWithPassword({ email: normalizeEmail(email), password });
    if (error || !data.session) return { ok: false, message: "We couldn't sign you in. Check your email and password, then try again." };
    currentSession = data.session;
    currentMessage = "";
    notify({ ...currentAuthSnapshot(), event: "SIGNED_IN" });
    return { ok: true, message: "Signed in." };
  } catch {
    return { ok: false, message: "We couldn't sign you in. Check your email and password, then try again." };
  }
}

export async function requestPasswordReset(email: string): Promise<AuthResult> {
  if (!client) return { ok: false, message: "Hosted account sign-in is not configured yet." };
  try {
    const redirectTo = new URL("/auth/reset", window.location.origin).href;
    const { error } = await client.auth.resetPasswordForEmail(normalizeEmail(email), { redirectTo });
    if (error) return { ok: false, message: "We couldn't send a reset email. Try again in a moment." };
    return { ok: true, message: "If an account matches that email, a reset link is on its way." };
  } catch {
    return { ok: false, message: "We couldn't send a reset email. Try again in a moment." };
  }
}

export async function updatePassword(password: string): Promise<AuthResult> {
  if (!client) return { ok: false, message: "Hosted account sign-in is not configured yet." };
  try {
    const { error } = await client.auth.updateUser({ password });
    if (error) return { ok: false, message: "We couldn't update your password. Try again." };
    return { ok: true, message: "Your password has been updated." };
  } catch {
    return { ok: false, message: "We couldn't update your password. Try again." };
  }
}

export async function signOut(): Promise<{ ok: boolean; message: string }> {
  if (!client) {
    currentSession = null;
    try {
      localAuthStorage?.removeItem(localAuthStorageKey);
    } catch {
      // Private browsing can reject storage.
    }
    currentMessage = "You are signed out.";
    if (localAuthMode) notify({ ...currentAuthSnapshot(), event: "SIGNED_OUT" });
    return { ok: true, message: currentMessage };
  }
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

export async function refreshAccountAccessToken(): Promise<string | null> {
  if (!client) return null;
  try {
    const { data, error } = await client.auth.refreshSession();
    if (error || !data.session) return null;
    currentSession = data.session;
    currentMessage = "";
    notify({ ...currentAuthSnapshot(), event: "TOKEN_REFRESHED" });
    return data.session.access_token;
  } catch {
    return null;
  }
}

export function cachedAccountAccessToken(): string | null {
  return localAuthMode ? null : currentSession?.access_token ?? null;
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
    return {
      context: "landing",
      mode: value.mode === "sign-in" ? "sign-in" : "sign-up",
      destination: value.destination === "home" ? "home" : "review",
    };
  } catch {
    return null;
  }
}

export function isAuthCallbackPath(): boolean {
  return typeof window !== "undefined" && window.location.pathname === "/auth/callback";
}

export function isPasswordResetPath(): boolean {
  return typeof window !== "undefined" && window.location.pathname === "/auth/reset";
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
  if (typeof window === "undefined" || isAuthCallbackPath()) return null;
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

function safeLocalStorage(): Storage | null {
  try {
    return typeof window === "undefined" ? null : window.localStorage;
  } catch {
    return null;
  }
}

async function completeAuthRedirect(): Promise<string | null> {
  if (typeof window === "undefined" || !client) return null;
  const url = new URL(window.location.href);
  const request = classifyAuthRedirect(url);
  if (request.kind === "none") return null;
  if (request.kind === "error") {
    cleanAuthUrl(request.cleanPath);
    return authRedirectMessage(request.errorCode, request.errorDescription)
      ?? (request.purpose === "recovery"
        ? "That password reset link is no longer valid. Request a new one."
        : "The account provider could not complete sign-in. Try again.");
  }
  if (request.kind === "unsupported") {
    cleanAuthUrl(request.cleanPath);
    return request.purpose === "recovery"
      ? "That password reset link is no longer valid. Request a new one."
      : "The account provider did not return a secure sign-in response. Try again.";
  }

  const { data, error } = await client.auth.exchangeCodeForSession(request.code);
  cleanAuthUrl(request.cleanPath);
  if (error || !data.session) {
    return request.purpose === "recovery"
      ? "That password reset link is no longer valid. Request a new one."
      : "The account provider could not complete sign-in. Try again.";
  }
  currentSession = data.session;
  return null;
}

function cleanAuthUrl(path = "/"): void {
  if (typeof window === "undefined") return;
  window.history.replaceState(null, "", path);
}

function loadLocalSession(): Session | null {
  try {
    return localAuthStorage?.getItem(localAuthStorageKey) === "1" ? createLocalSession() : null;
  } catch {
    return null;
  }
}

function createLocalSession(): Session {
  const now = Math.floor(Date.now() / 1000);
  return {
    access_token: "",
    refresh_token: "",
    expires_in: 86400,
    expires_at: now + 86400,
    token_type: "bearer",
    user: {
      id: "local-dev-user",
      aud: "authenticated",
      role: "authenticated",
      email: "local@plywise.test",
      app_metadata: { provider: "local", providers: ["local"] },
      user_metadata: { name: "Local test account" },
      created_at: new Date(now * 1000).toISOString(),
    },
  };
}


function signUpErrorMessage(error: unknown): string {
  const code = typeof error === "object" && error !== null && "code" in error
    ? String((error as { code?: unknown }).code ?? "").toLowerCase()
    : "";
  const message = typeof error === "object" && error !== null && "message" in error
    ? String((error as { message?: unknown }).message ?? "").toLowerCase()
    : "";
  if (code.includes("rate") || message.includes("rate limit") || message.includes("too many")) {
    return "Too many attempts. Wait a moment and try again.";
  }
  if (code.includes("weak") || message.includes("password")) {
    return "Use a stronger password with at least 10 characters.";
  }
  if (code.includes("email") && (code.includes("invalid") || message.includes("invalid email"))) {
    return "Enter a valid email address.";
  }
  if (code.includes("already") || code.includes("exists") || message.includes("already registered") || message.includes("already been registered")) {
    return "That email may already be in use. Try signing in or resetting your password.";
  }
  return "We couldn't create that account. Check your email and password, then try again.";
}
function normalizeEmail(email: string): string {
  return email.trim().toLowerCase();
}

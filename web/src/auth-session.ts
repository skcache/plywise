import {
  createClient,
  type AuthChangeEvent,
  type Session,
  type SupabaseClient,
} from "@supabase/supabase-js";
import { authRedirectMessage } from "./auth-redirect";

export type AuthProvider = "google" | "github";
export type AuthEntryMode = "sign-up" | "sign-in";

export type AuthSnapshot = {
  configured: boolean;
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
const authStorage = safeSessionStorage();
const client: SupabaseClient | null = supabaseUrl && supabasePublishableKey
  ? createClient(supabaseUrl, supabasePublishableKey, {
      auth: {
        autoRefreshToken: true,
        // OAuth codes are exchanged explicitly on /auth/callback below. This keeps
        // the callback state visible to one trusted path instead of relying on
        // automatic URL parsing in the client constructor.
        detectSessionInUrl: false,
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

async function completeAuthRedirect(): Promise<string | null> {
  if (typeof window === "undefined" || !client) return null;
  const url = new URL(window.location.href);
  const isCallback = url.pathname === "/auth/callback";
  const recoveryTokens = readRecoveryTokens(url.hash);
  // Supabase can still use the project Site URL while its redirect allowlist is
  // being updated. Accept an OAuth response on the same origin's root as a
  // compatibility path, but only when an auth code/token/error is present.
  const isRootAuthResponse = url.pathname === "/" && (
    url.searchParams.has("code") ||
    url.searchParams.has("error") ||
    url.searchParams.has("error_code") ||
    Boolean(recoveryTokens)
  );

  if (isCallback || isRootAuthResponse) {
    const errorCode = url.searchParams.get("error_code") ?? url.searchParams.get("error") ?? "";
    const errorDescription = url.searchParams.get("error_description") ?? "";
    if (errorCode || errorDescription) {
      cleanAuthUrl();
      return authRedirectMessage(errorCode, errorDescription) ?? "The account provider could not complete sign-in. Try again.";
    }
    const code = url.searchParams.get("code");
    if (code) {
      const { data, error } = await client.auth.exchangeCodeForSession(code);
      cleanAuthUrl();
      if (error || !data.session) return "The account provider could not complete sign-in. Try again.";
      currentSession = data.session;
      return null;
    }
    // Older provider configurations can return tokens in the hash instead of a PKCE code.
    // Accept that response only on this exact callback path, then immediately remove it.
    if (recoveryTokens) {
      const { data, error } = await client.auth.setSession(recoveryTokens);
      cleanAuthUrl();
      if (error || !data.session) return "The account provider could not complete sign-in. Try again.";
      currentSession = data.session;
      return null;
    }
    cleanAuthUrl();
    return "The account provider did not return a usable sign-in response. Try again.";
  }

  if (isPasswordResetPath() && recoveryTokens) {
    const { error } = await client.auth.setSession(recoveryTokens);
    cleanAuthUrl("/auth/reset");
    if (error) return "That password reset link is no longer valid. Request a new one.";
  }
  return null;
}

function readRecoveryTokens(hash: string): { access_token: string; refresh_token: string } | null {
  const params = new URLSearchParams(hash.replace(/^#/, ""));
  const accessToken = params.get("access_token");
  const refreshToken = params.get("refresh_token");
  return accessToken && refreshToken ? { access_token: accessToken, refresh_token: refreshToken } : null;
}

function cleanAuthUrl(path = "/"): void {
  if (typeof window === "undefined") return;
  window.history.replaceState(null, "", path);
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

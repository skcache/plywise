export function authRedirectMessage(errorCode: string, errorDescription: string): string | null {
  const context = `${errorCode} ${errorDescription}`;
  if (!context.trim()) return null;
  if (/cancel|denied|abort/i.test(context)) {
    return "Sign-in was cancelled. Start again when you are ready.";
  }
  if (/expired|invalid|session/i.test(context)) {
    return "That account session expired. Start sign-in again when you are ready.";
  }
  return "The account provider could not complete sign-in. Try again.";
}

export type AuthRedirectRequest =
  | { kind: "none" }
  | { kind: "code"; purpose: "sign-in" | "recovery"; code: string; cleanPath: "/" | "/auth/reset" }
  | { kind: "error"; purpose: "sign-in" | "recovery"; errorCode: string; errorDescription: string; cleanPath: "/" | "/auth/reset" }
  | { kind: "unsupported"; purpose: "sign-in" | "recovery"; cleanPath: "/" | "/auth/reset" };

/**
 * Classify only same-origin routes that Plywise deliberately uses for auth.
 * Raw bearer tokens are never accepted from a URL: hosted auth is PKCE-code
 * only, which binds the response to the browser that started the flow.
 */
export function classifyAuthRedirect(value: URL | string): AuthRedirectRequest {
  const url = value instanceof URL ? value : new URL(value, "https://plywise.invalid");
  const recovery = url.pathname === "/auth/reset";
  const callback = url.pathname === "/auth/callback";
  const rootCompatibility = url.pathname === "/";
  const purpose = recovery ? "recovery" : "sign-in";
  const cleanPath = recovery ? "/auth/reset" : "/";
  const code = url.searchParams.get("code")?.trim() ?? "";
  const errorCode = url.searchParams.get("error_code") ?? url.searchParams.get("error") ?? "";
  const errorDescription = url.searchParams.get("error_description") ?? "";
  const hash = new URLSearchParams(url.hash.replace(/^#/, ""));
  const hasRawTokens = hash.has("access_token") || hash.has("refresh_token");

  if (!callback && !recovery && !rootCompatibility) return { kind: "none" };
  if (hasRawTokens) return { kind: "unsupported", purpose, cleanPath };
  if (errorCode || errorDescription) {
    return { kind: "error", purpose, errorCode, errorDescription, cleanPath };
  }
  if (code) return { kind: "code", purpose, code, cleanPath };
  if (callback) return { kind: "unsupported", purpose, cleanPath };
  return { kind: "none" };
}

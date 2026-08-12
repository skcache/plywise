import { authRedirectMessage, classifyAuthRedirect } from "../src/auth-redirect";

if (authRedirectMessage("access_denied", "user cancelled") !== "Sign-in was cancelled. Start again when you are ready.") {
  throw new Error("cancelled provider sign-in should have an honest message");
}
if (authRedirectMessage("session_expired", "") !== "That account session expired. Start sign-in again when you are ready.") {
  throw new Error("expired provider session should have an honest message");
}
if (authRedirectMessage("server_error", "") !== "The account provider could not complete sign-in. Try again.") {
  throw new Error("provider failures should not expose raw details");
}
if (authRedirectMessage("", "") !== null) {
  throw new Error("empty provider state should not create an error");
}

const callback = classifyAuthRedirect("https://plywise.test/auth/callback?code=oauth-code");
if (callback.kind !== "code" || callback.purpose !== "sign-in" || callback.cleanPath !== "/") {
  throw new Error("OAuth callbacks should exchange a sign-in PKCE code");
}

const reset = classifyAuthRedirect("https://plywise.test/auth/reset?code=recovery-code");
if (reset.kind !== "code" || reset.purpose !== "recovery" || reset.cleanPath !== "/auth/reset") {
  throw new Error("password reset links should exchange a recovery PKCE code");
}

const rootCompatibility = classifyAuthRedirect("https://plywise.test/?code=site-url-code");
if (rootCompatibility.kind !== "code" || rootCompatibility.purpose !== "sign-in") {
  throw new Error("the configured Site URL should remain a PKCE-code compatibility path");
}

for (const path of ["/", "/auth/callback", "/auth/reset"]) {
  const rawTokens = classifyAuthRedirect(`https://plywise.test${path}#access_token=attacker&refresh_token=attacker`);
  if (rawTokens.kind !== "unsupported") {
    throw new Error(`raw bearer tokens must be rejected on ${path}`);
  }
}

if (classifyAuthRedirect("https://plywise.test/auth/reset").kind !== "none") {
  throw new Error("the reset form without redirect parameters should remain available");
}
if (classifyAuthRedirect("https://plywise.test/home?code=ignored").kind !== "none") {
  throw new Error("auth parameters must be ignored outside the allowlisted routes");
}

console.log("auth redirect tests passed");

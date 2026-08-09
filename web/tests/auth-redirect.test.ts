import { authRedirectMessage } from "../src/auth-redirect";

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

console.log("auth redirect tests passed");

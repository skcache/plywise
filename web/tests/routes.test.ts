import { isAccountEntryRoute, routeForAuthState, routeForSession, routeFromHash } from "../src/routes";

function assert(actual: unknown, expected: unknown, label: string): void {
  if (actual !== expected) throw new Error(`${label}: expected ${String(expected)}, got ${String(actual)}`);
}

assert(routeFromHash("#/analysis"), "analysis", "known routes are parsed");
assert(routeFromHash("#/not-a-route"), "landing", "unknown routes fail closed");
assert(routeForSession("analysis", false), "landing", "signed-out users cannot keep protected routes");
assert(routeForSession("home", false), "landing", "signed-out home deep links return to landing");
assert(routeForSession("sign-up", false), "sign-up", "signed-out sign-up remains reachable");
assert(routeForSession("sign-in", false), "sign-in", "signed-out sign-in remains reachable");
assert(routeForSession("sign-up", true), "home", "signed-in users leave account entry");
assert(routeForSession("analysis", true), "analysis", "signed-in users keep protected routes");
assert(routeForAuthState("analysis", false, true), "analysis", "unknown sessions keep the requested route while loading");
assert(routeForAuthState("analysis", false, false), "landing", "confirmed signed-out sessions leave protected routes");
assert(routeForAuthState("analysis", true, false), "analysis", "restored sessions keep protected routes");
assert(isAccountEntryRoute("sign-in"), true, "sign-in is an account entry route");
assert(isAccountEntryRoute("landing"), false, "landing is not an account entry route");

console.log("route policy tests passed");

export type Route = "landing" | "sign-up" | "sign-in" | "home" | "recent" | "analysis" | "explore" | "progress" | "settings";

const routes = new Set<Route>(["landing", "sign-up", "sign-in", "home", "recent", "analysis", "explore", "progress", "settings"]);

export function routeFromHash(hash: string): Route {
  const candidate = hash.replace(/^#\/?/, "") as Route;
  return candidate && routes.has(candidate) ? candidate : "landing";
}

export function routeForSession(route: Route, signedIn: boolean): Route {
  if (signedIn) {
    return route === "landing" || route === "sign-up" || route === "sign-in" ? "home" : route;
  }
  return route === "landing" || route === "sign-up" || route === "sign-in" ? route : "landing";
}

export function isAccountEntryRoute(route: Route): route is "sign-up" | "sign-in" {
  return route === "sign-up" || route === "sign-in";
}

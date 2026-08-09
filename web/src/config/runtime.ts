import { resolveServiceOrigins, serviceUrl } from "./service-origin";
import { cachedAccountAccessToken } from "../auth-session";

const origins = resolveServiceOrigins(
  {
    // Vite runs on its own port in local development. Keep production same-origin, but point
    // the dev shell at the local C++ service unless an explicit origin is supplied.
    apiOrigin: import.meta.env.VITE_PLYWISE_API_ORIGIN ?? (import.meta.env.DEV ? "http://127.0.0.1:8787" : undefined),
    eventOrigin: import.meta.env.VITE_PLYWISE_EVENT_ORIGIN,
  },
  window.location.origin,
);

export function apiUrl(path: string): string {
  return serviceUrl(origins.api, path);
}

export function eventUrl(path: string): string {
  return serviceUrl(origins.events, path);
}

/** WebSocket cannot set Authorization headers; pass the short-lived account proof as a subprotocol. */
export function eventProtocols(): string[] {
  const accountToken = cachedAccountAccessToken();
  return accountToken ? ["plywise-auth", accountToken] : [];
}

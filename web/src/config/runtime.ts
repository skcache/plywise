import { resolveServiceOrigins, serviceUrl } from "./service-origin";
import { loadGuestSession } from "../guest-session";

const origins = resolveServiceOrigins(
  {
    apiOrigin: import.meta.env.VITE_PLYWISE_API_ORIGIN,
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

/** WebSocket cannot set Authorization headers; offer the guest proof as a subprotocol instead. */
export function eventProtocols(): string[] {
  const guestSession = loadGuestSession();
  return guestSession ? ["plywise-auth", guestSession.token] : [];
}

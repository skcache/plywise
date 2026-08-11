import { retryWithFreshAuth, type AuthRetryResponse } from "../src/api-auth-retry";

function assert(actual: unknown, expected: unknown, label: string): void {
  if (actual !== expected) throw new Error(`${label}: expected ${String(expected)}, got ${String(actual)}`);
}

async function run(): Promise<void> {
  let requests = 0;
  let refreshes = 0;
  const success: AuthRetryResponse = await retryWithFreshAuth(
    "expired",
    async (token) => {
      requests += 1;
      return { status: token === "fresh" ? 200 : 401 };
    },
    async () => {
      refreshes += 1;
      return "fresh";
    },
  );
  assert(success.status, 200, "fresh token retries the request");
  assert(requests, 2, "fresh token makes one retry");
  assert(refreshes, 1, "fresh token refreshes once");

  requests = 0;
  refreshes = 0;
  const unchanged: AuthRetryResponse = await retryWithFreshAuth(
    "expired",
    async () => {
      requests += 1;
      return { status: 401 };
    },
    async () => {
      refreshes += 1;
      return "expired";
    },
  );
  assert(unchanged.status, 401, "unchanged token keeps the original failure");
  assert(requests, 1, "unchanged token does not retry");
  assert(refreshes, 1, "unchanged token refreshes only once");

  requests = 0;
  refreshes = 0;
  const anonymous: AuthRetryResponse = await retryWithFreshAuth(
    null,
    async () => {
      requests += 1;
      return { status: 401 };
    },
    async () => {
      refreshes += 1;
      return "fresh";
    },
  );
  assert(anonymous.status, 401, "anonymous failures stay failures");
  assert(requests, 1, "anonymous requests do not retry");
  assert(refreshes, 0, "anonymous requests never refresh");

  console.log("API auth retry tests passed");
}

void run();

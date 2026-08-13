import {
  checkingRetryFeedback,
  completedRetryFeedback,
  emptyRetryFeedback,
  failedRetryFeedback,
} from "../src/retry-review";

function assert(actual: unknown, expected: unknown, label: string): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

assert(emptyRetryFeedback, { status: "idle", uci: "", message: "" }, "retry begins idle");
assert(checkingRetryFeedback("d2d4"), { status: "checking", uci: "d2d4", message: "Checking that move…" }, "checking preserves attempted move");
assert(completedRetryFeedback("d2d4", true).status, "correct", "accepted move becomes correct");
assert(completedRetryFeedback("a2a3", false).status, "incorrect", "legal alternative remains distinct from illegal move");
assert(failedRetryFeedback(true).status, "illegal", "illegal API response stays retryable");
assert(failedRetryFeedback(false).status, "error", "service failure stays distinct from chess result");

console.log("retry review tests passed");

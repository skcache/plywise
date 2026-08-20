export type RetryFeedback = {
  status: "idle" | "checking" | "illegal" | "incorrect" | "correct" | "error";
  uci: string;
  message: string;
};

export const emptyRetryFeedback: RetryFeedback = { status: "idle", uci: "", message: "" };

export function checkingRetryFeedback(uci: string): RetryFeedback {
  return { status: "checking", uci, message: "Checking that move…" };
}

export function completedRetryFeedback(uci: string, accepted: boolean): RetryFeedback {
  return accepted
    ? { status: "correct", uci, message: "That move matches the review." }
    : { status: "incorrect", uci, message: "That move is legal, but another continuation is stronger here." };
}

export function failedRetryFeedback(illegal: boolean): RetryFeedback {
  return illegal
    ? { status: "illegal", uci: "", message: "That move is not legal in this position. Try another move." }
    : { status: "error", uci: "", message: "Plywise could not check that move. Try again." };
}

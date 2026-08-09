export function authRedirectMessage(errorCode: string, errorDescription: string): string | null {
  const context = `${errorCode} ${errorDescription}`;
  if (!context.trim()) return null;
  if (/cancel|denied|abort/i.test(context)) {
    return "Sign-in was cancelled. Your guest review is still here.";
  }
  if (/expired|invalid|session/i.test(context)) {
    return "That account session expired. Start sign-in again when you are ready.";
  }
  return "The account provider could not complete sign-in. Try again.";
}

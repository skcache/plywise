import { useEffect, useRef } from "react";
import { authProviderLabel, type AuthProvider, type AuthSnapshot } from "../auth-session";
import { Icon } from "./Icon";

const providers: Array<{ id: AuthProvider; mark: string }> = [
  { id: "google", mark: "G" },
  { id: "apple", mark: "" },
  { id: "github", mark: "GH" },
];

export function AccountPrompt({
  auth,
  busyProvider,
  message,
  onProvider,
  onSignOut,
  onClose,
}: {
  auth: AuthSnapshot;
  busyProvider: AuthProvider | null;
  message: string;
  onProvider: (provider: AuthProvider) => void;
  onSignOut: () => void;
  onClose: () => void;
}) {
  const dialogRef = useRef<HTMLElement>(null);
  const closeRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    closeRef.current?.focus();
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        event.preventDefault();
        onClose();
        return;
      }
      if (event.key !== "Tab") return;
      const dialog = dialogRef.current;
      if (!dialog) return;
      const focusable = Array.from(dialog.querySelectorAll<HTMLElement>(
        "button:not([disabled]), a[href], input:not([disabled]), [tabindex]:not([tabindex='-1'])",
      ));
      if (!focusable.length) return;
      const first = focusable[0]!;
      const last = focusable[focusable.length - 1]!;
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onClose]);

  const accountLabel = auth.session ? displayName(auth.session.user.user_metadata, auth.session.user.email) : "";

  return <div className="modal-backdrop account-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>
    <section ref={dialogRef} className="account-modal" role="dialog" aria-modal="true" aria-labelledby="account-prompt-title" aria-describedby="account-prompt-description">
      <header>
        <div>
          <span>Account entry</span>
          <h2 id="account-prompt-title">Sign in to start a review.</h2>
          <p id="account-prompt-description">Plywise keeps your games and reviews with your account, so you can come back to them later. No Chess.com password is needed.</p>
        </div>
        <button ref={closeRef} type="button" aria-label="Close account prompt" onClick={onClose}><Icon name="close" /></button>
      </header>

      {auth.session ? <>
        <div className="account-connected" role="status">
          <span className="account-connected-mark" aria-hidden="true"><Icon name="check" /></span>
          <span><strong>Signed in</strong><small>{accountLabel}</small></span>
        </div>
        <p className="account-prompt-status" role="status">{message || "Your account is ready. Add a completed game to begin."}</p>
        <footer>
          <button type="button" onClick={onSignOut}>Sign out</button>
          <button type="button" className="account-primary" onClick={onClose}>Start a review</button>
        </footer>
      </> : <>
        <div className="account-providers" aria-label="Sign-in providers">
          {providers.map(({ id, mark }) => <button
            key={id}
            type="button"
            disabled={busyProvider !== null || !auth.configured}
            onClick={() => onProvider(id)}
          >
            <span className="account-provider-mark" aria-hidden="true">{mark}</span>
            <span><strong>Continue with {authProviderLabel(id)}</strong><small>{auth.configured ? "Secure account sign-in" : "Not configured in this build"}</small></span>
            <span className="account-provider-arrow" aria-hidden="true">{busyProvider === id ? "…" : "→"}</span>
          </button>)}
        </div>

        <p className="account-prompt-status" role="status" aria-live="polite">
          {message || (auth.configured
            ? "Choose the account you already use. You will return here when sign-in finishes."
            : "Hosted sign-in is not configured in this local build. Add the public Supabase settings before publishing.")}
        </p>
        <footer>
          <small>One account, one private library. Provider passwords stay with the provider.</small>
          <button type="button" onClick={onClose}>Back to Plywise</button>
        </footer>
      </>}
    </section>
  </div>;
}

function displayName(metadata: Record<string, unknown>, email: string | undefined): string {
  for (const key of ["full_name", "name", "user_name"]) {
    const value = metadata[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  return email || "Plywise account";
}

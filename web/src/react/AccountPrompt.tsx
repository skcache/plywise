import { useEffect, useRef } from "react";
import { authProviderLabel, type AuthEntryMode, type AuthProvider, type AuthSnapshot } from "../auth-session";
import { Icon } from "./Icon";

const providers: Array<{ id: AuthProvider; mark: string }> = [
  { id: "google", mark: "G" },
  { id: "github", mark: "GH" },
];

export function AccountPrompt({
  auth,
  mode,
  busyProvider,
  message,
  onProvider,
  onModeChange,
  onSignOut,
  onClose,
}: {
  auth: AuthSnapshot;
  mode: AuthEntryMode;
  busyProvider: AuthProvider | null;
  message: string;
  onProvider: (provider: AuthProvider) => void;
  onModeChange: (mode: AuthEntryMode) => void;
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
  const signUpMode = mode === "sign-up";

  return <div className="modal-backdrop account-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>
    <section ref={dialogRef} className="account-modal" role="dialog" aria-modal="true" aria-labelledby="account-prompt-title" aria-describedby="account-prompt-description">
      <header>
        <div>
          <span>{signUpMode ? "Create your account" : "Welcome back"}</span>
          <h2 id="account-prompt-title">{signUpMode ? "Sign up to analyze." : "Sign in to analyze."}</h2>
          <p id="account-prompt-description">Plywise keeps your games and reviews with your account. The first sign-up creates your account; the next time, the same provider signs you back in.</p>
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
        <div className="account-providers" aria-label={signUpMode ? "Sign-up providers" : "Sign-in providers"}>
          {providers.map(({ id, mark }) => <button
            key={id}
            type="button"
            disabled={busyProvider !== null || !auth.configured}
            onClick={() => onProvider(id)}
          >
            <span className="account-provider-mark" aria-hidden="true">{mark}</span>
            <span><strong>{signUpMode ? "Sign up with" : "Sign in with"} {authProviderLabel(id)}</strong><small>{auth.configured ? "Secure provider authentication" : "Not configured in this build"}</small></span>
            <span className="account-provider-arrow" aria-hidden="true">{busyProvider === id ? "…" : "→"}</span>
          </button>)}
        </div>

        <p className="account-prompt-status" role="status" aria-live="polite">
          {message || (auth.configured
            ? signUpMode
              ? "Choose a provider to create your account. You will return here after approval."
              : "Choose the provider attached to your Plywise account."
            : "Hosted account entry is not configured in this local build. Add the public Supabase settings before publishing.")}
        </p>
        <footer>
          <small>Plywise never receives your provider password.</small>
          <div className="account-entry-actions">
            <button type="button" className="account-mode-switch" onClick={() => onModeChange(signUpMode ? "sign-in" : "sign-up")}>
              {signUpMode ? "Already have an account? Sign in" : "New to Plywise? Sign up"}
            </button>
            <button type="button" onClick={onClose}>Back to Plywise</button>
          </div>
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

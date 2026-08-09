import { useEffect, useRef } from "react";
import { authProviderLabel, type AuthProvider, type AuthSnapshot } from "../auth-session";
import { Icon } from "./Icon";

type AccountContext = "landing" | "save";

const providers: AuthProvider[] = ["google", "apple", "github"];

export function AccountPrompt({
  context,
  auth,
  busyProvider,
  message,
  onProvider,
  onSignOut,
  onClose,
}: {
  context: AccountContext;
  auth: AuthSnapshot;
  busyProvider: AuthProvider | null;
  message: string;
  onProvider: (provider: AuthProvider) => void;
  onSignOut: () => void;
  onClose: () => void;
}) {
  const closeRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    closeRef.current?.focus();
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onClose]);

  const title = auth.session
    ? "Your account is connected."
    : context === "save"
      ? "Keep this review."
      : "Sign in when you are ready.";
  const description = auth.session
    ? "Your saved games and reviews use the account boundary enforced by the hosted service."
    : context === "save"
      ? "Create an account to keep this review across devices. Your guest review stays available while sign-in is being configured."
      : "Account entry connects saved games and history. You can still review one completed game without an account.";
  const accountLabel = auth.session ? displayName(auth.session.user.user_metadata, auth.session.user.email) : "";

  return <div className="modal-backdrop account-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>
    <section className="account-modal" role="dialog" aria-modal="true" aria-labelledby="account-prompt-title">
      <header>
        <div>
          <span>Account entry</span>
          <h2 id="account-prompt-title">{title}</h2>
          <p>{description}</p>
        </div>
        <button ref={closeRef} type="button" aria-label="Close account prompt" onClick={onClose}><Icon name="close" /></button>
      </header>

      {auth.session ? <>
        <div className="account-connected" role="status">
          <span className="account-connected-mark" aria-hidden="true"><Icon name="check" /></span>
          <span><strong>Signed in</strong><small>{accountLabel}</small></span>
        </div>
        <p className="account-prompt-status" role="status">{message || "Your account is ready for saved history."}</p>
        <footer>
          <button type="button" onClick={onSignOut}>Sign out</button>
          <button type="button" onClick={onClose}>Continue</button>
        </footer>
      </> : <>
        <div className="account-providers" aria-label="Sign-in providers">
          {providers.map((provider) => <button
            key={provider}
            type="button"
            disabled={busyProvider !== null}
            onClick={() => onProvider(provider)}
          >
            <span className="account-provider-mark" aria-hidden="true">{authProviderLabel(provider)[0]}</span>
            <span><strong>Continue with {authProviderLabel(provider)}</strong><small>{auth.configured ? "Managed account sign-in" : "Provider connection coming next"}</small></span>
            <span className="account-provider-arrow" aria-hidden="true">{busyProvider === provider ? "…" : "→"}</span>
          </button>)}
        </div>

        <p className="account-prompt-status" role="status">
          {message || (auth.configured
            ? "Choose a provider. Plywise never receives your provider password."
            : "Hosted account sign-in is not configured in this build. No account data was sent.")}
        </p>
        <footer>
          <small>Guest analysis stays free and does not require a password.</small>
          <button type="button" onClick={onClose}>Continue as guest</button>
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

import { useEffect, useRef, useState, type FormEvent } from "react";
import { authProviderLabel, type AuthEntryMode, type AuthProvider, type AuthResult, type AuthSnapshot } from "../auth-session";
import { Icon } from "./Icon";

const providers: Array<{ id: AuthProvider; mark: string }> = [
  { id: "google", mark: "G" },
  { id: "github", mark: "GH" },
];

export function AccountPrompt({
  auth,
  mode,
  presentation = "modal",
  busyProvider,
  message,
  onProvider,
  onModeChange,
  onPasswordSignUp,
  onPasswordSignIn,
  onPasswordReset,
  onSignOut,
  onClose,
}: {
  auth: AuthSnapshot;
  mode: AuthEntryMode;
  presentation?: "modal" | "page";
  busyProvider: AuthProvider | null;
  message: string;
  onProvider: (provider: AuthProvider) => void;
  onModeChange: (mode: AuthEntryMode) => void;
  onPasswordSignUp: (name: string, email: string, password: string) => Promise<AuthResult>;
  onPasswordSignIn: (email: string, password: string) => Promise<AuthResult>;
  onPasswordReset: (email: string) => Promise<AuthResult>;
  onSignOut: () => void;
  onClose: () => void;
}) {
  const dialogRef = useRef<HTMLElement>(null);
  const closeRef = useRef<HTMLButtonElement>(null);
  const [name, setName] = useState("");
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [showPassword, setShowPassword] = useState(false);
  const [resetMode, setResetMode] = useState(false);
  const [submitMessage, setSubmitMessage] = useState("");
  const [submitting, setSubmitting] = useState(false);

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

  const signUpMode = mode === "sign-up";
  const accountLabel = auth.session ? displayName(auth.session.user.user_metadata, auth.session.user.email) : "";
  const statusMessage = submitMessage || message || (auth.configured
    ? resetMode
      ? "Enter your email and we will send a reset link if an account matches it."
      : signUpMode
        ? "Create one Plywise account, then use the same sign-in method each time."
        : "Use the email, Google, or GitHub method already connected to your account."
    : "Hosted account entry is not configured in this build.");

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    setSubmitting(true);
    setSubmitMessage("");
    const result = resetMode
      ? await onPasswordReset(email)
      : signUpMode
        ? await onPasswordSignUp(name, email, password)
        : await onPasswordSignIn(email, password);
    setSubmitMessage(result.message);
    setSubmitting(false);
    if (result.ok && resetMode) setResetMode(false);
    if (result.ok && !resetMode) setPassword("");
  }

  const dialog = <section ref={dialogRef} className="account-modal" role="dialog" aria-modal="true" aria-labelledby="account-prompt-title" aria-describedby="account-prompt-description">
    <header>
      <div>
        <span>{resetMode ? "Reset your password" : signUpMode ? "Create your account" : "Welcome back"}</span>
        <h2 id="account-prompt-title">{resetMode ? "Reset your password." : signUpMode ? "Sign up to analyze." : "Sign in to analyze."}</h2>
        <p id="account-prompt-description">{resetMode
          ? "We will send a time-limited reset link if an account matches that email."
          : signUpMode
            ? "Free game analysis starts with one account. Save your reviews and pick up where you left off."
            : "Sign in to see your saved games and continue a review."}</p>
      </div>
      <button ref={closeRef} type="button" aria-label={presentation === "page" ? "Back to landing page" : "Close account prompt"} onClick={onClose}><Icon name="close" /></button>
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
      {resetMode ? <form className="account-form" onSubmit={(event) => void submit(event)}>
        <div className="account-field">
          <label htmlFor="account-reset-email">Email</label>
          <input id="account-reset-email" name="email" type="email" inputMode="email" autoComplete="email" required value={email} onChange={(event) => setEmail(event.target.value)} disabled={!auth.configured || submitting} />
        </div>
        <button type="submit" className="account-primary account-form-submit" disabled={!auth.configured || submitting}>{submitting ? "Sending…" : "Send reset link"}</button>
        <button type="button" className="account-text-button" onClick={() => { setResetMode(false); setSubmitMessage(""); }}>Back to sign in</button>
      </form> : <form className="account-form" onSubmit={(event) => void submit(event)}>
        {signUpMode && <div className="account-field">
          <label htmlFor="account-name">Name</label>
          <input id="account-name" name="name" type="text" autoComplete="name" required value={name} onChange={(event) => setName(event.target.value)} disabled={!auth.configured || submitting} />
        </div>}
        <div className="account-field">
          <label htmlFor="account-email">Email</label>
          <input id="account-email" name="email" type="email" inputMode="email" autoComplete="email" required value={email} onChange={(event) => setEmail(event.target.value)} disabled={!auth.configured || submitting} />
        </div>
        <div className="account-field">
          <label htmlFor="account-password">Password</label>
          <div className="account-password-field">
            <input id="account-password" name="password" type={showPassword ? "text" : "password"} autoComplete={signUpMode ? "new-password" : "current-password"} minLength={10} required value={password} onChange={(event) => setPassword(event.target.value)} disabled={!auth.configured || submitting} />
            <button type="button" className="account-password-toggle" onClick={() => setShowPassword((value) => !value)} aria-label={showPassword ? "Hide password" : "Show password"}>{showPassword ? "Hide" : "Show"}</button>
          </div>
          {signUpMode && <small className="account-field-hint">Use at least 10 characters. Password managers and paste are welcome.</small>}
        </div>
        <button type="submit" className="account-primary account-form-submit" disabled={!auth.configured || submitting}>{submitting ? (signUpMode ? "Creating account…" : "Signing in…") : signUpMode ? "Create account" : "Sign in"}</button>
        {!signUpMode && <button type="button" className="account-text-button account-forgot" onClick={() => { setResetMode(true); setSubmitMessage(""); }}>Forgot password?</button>}
      </form>}

      {!resetMode && <>
        <div className="account-divider"><span>Or continue with</span></div>
        <div className="account-providers" aria-label={signUpMode ? "Sign-up providers" : "Sign-in providers"}>
          {providers.map(({ id, mark }) => <button
            key={id}
            type="button"
            disabled={busyProvider !== null || !auth.configured || submitting}
            onClick={() => onProvider(id)}
          >
            <span className="account-provider-mark" aria-hidden="true">{mark}</span>
            <span><strong>{signUpMode ? "Sign up with" : "Sign in with"} {authProviderLabel(id)}</strong><small>{auth.configured ? "Secure provider authentication" : "Not configured in this build"}</small></span>
            <span className="account-provider-arrow" aria-hidden="true">{busyProvider === id ? "…" : "→"}</span>
          </button>)}
        </div>
      </>}

      <p className="account-prompt-status" role="status" aria-live="polite">{statusMessage}</p>
      <footer>
        <small>Plywise never receives your provider password.</small>
        <div className="account-entry-actions">
          <button type="button" className="account-mode-switch" onClick={() => { setResetMode(false); setSubmitMessage(""); onModeChange(signUpMode ? "sign-in" : "sign-up"); }}>
            {signUpMode ? "Already have an account? Sign in" : "New to Plywise? Create account"}
          </button>
          {presentation === "modal" && <button type="button" onClick={onClose}>Back to Plywise</button>}
        </div>
      </footer>
    </>}
  </section>;

  if (presentation === "page") {
    return <main className="account-page"><div className="account-page-shell"><button className="account-page-brand" type="button" onClick={onClose}>Plywise</button>{dialog}</div></main>;
  }
  return <div className="modal-backdrop account-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>{dialog}</div>;
}

function displayName(metadata: Record<string, unknown>, email: string | undefined): string {
  for (const key of ["full_name", "name", "user_name"]) {
    const value = metadata[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  return email || "Plywise account";
}

export function PasswordResetPrompt({ auth, message, onSubmit, onClose }: {
  auth: AuthSnapshot;
  message: string;
  onSubmit: (password: string) => Promise<AuthResult>;
  onClose: () => void;
}) {
  const [password, setPassword] = useState("");
  const [showPassword, setShowPassword] = useState(false);
  const [busy, setBusy] = useState(false);
  const [complete, setComplete] = useState(false);

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true);
    const result = await onSubmit(password);
    setBusy(false);
    setComplete(result.ok);
    if (result.ok) setPassword("");
  }

  return <main className="account-page"><div className="account-page-shell">
    <button className="account-page-brand" type="button" onClick={onClose}>Plywise</button>
    <section className="account-modal" role="dialog" aria-labelledby="password-reset-title" aria-describedby="password-reset-description">
      <header>
        <div>
          <span>Account security</span>
          <h2 id="password-reset-title">Choose a new password.</h2>
          <p id="password-reset-description">Use at least 10 characters. You can paste from a password manager.</p>
        </div>
      </header>
      {complete ? <>
        <p className="account-prompt-status" role="status">{message || "Your password has been updated."}</p>
        <button type="button" className="account-primary account-form-submit" onClick={onClose}>Continue to Plywise</button>
      </> : <form className="account-form" onSubmit={(event) => void submit(event)}>
        <div className="account-field">
          <label htmlFor="account-new-password">New password</label>
          <div className="account-password-field">
            <input id="account-new-password" type={showPassword ? "text" : "password"} autoComplete="new-password" minLength={10} required value={password} onChange={(event) => setPassword(event.target.value)} disabled={!auth.configured || busy} />
            <button type="button" className="account-password-toggle" onClick={() => setShowPassword((value) => !value)} aria-label={showPassword ? "Hide password" : "Show password"}>{showPassword ? "Hide" : "Show"}</button>
          </div>
        </div>
        <button type="submit" className="account-primary account-form-submit" disabled={!auth.configured || busy}>{busy ? "Updating…" : "Update password"}</button>
        {message && <p className="account-prompt-status" role="status" aria-live="polite">{message}</p>}
      </form>}
    </section>
  </div></main>;
}

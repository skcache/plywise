import { useEffect, useRef, useState, type FormEvent } from "react";
import { authProviderLabel, type AuthEntryMode, type AuthProvider, type AuthResult, type AuthSnapshot } from "../auth-session";
import { Icon } from "./Icon";

const providers: Array<{ id: AuthProvider }> = [
  { id: "google" },
  { id: "github" },
];

export function AccountPrompt({
  auth,
  mode,
  presentation = "modal",
  busyProvider,
  message,
  onProvider,
  onLocalSignIn,
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
  onLocalSignIn: () => void;
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
  const [submitError, setSubmitError] = useState(false);
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
  const statusMessage = submitMessage || message || (auth.local
    ? "Local test mode is active. This account stays on this machine."
    : auth.configured
    ? resetMode
      ? "Enter your email and we will send a reset link if an account matches it."
      : ""
    : "Hosted account entry is not configured in this build.");

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (signUpMode && !name.trim()) {
      setSubmitError(true);
      setSubmitMessage("Enter your name.");
      return;
    }
    if (!email.trim()) {
      setSubmitError(true);
      setSubmitMessage("Enter your email address.");
      return;
    }
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email.trim())) {
      setSubmitError(true);
      setSubmitMessage("Enter a valid email address.");
      return;
    }
    if (!resetMode && !password) {
      setSubmitError(true);
      setSubmitMessage("Enter your password.");
      return;
    }
    if (signUpMode && password.length < 10) {
      setSubmitError(true);
      setSubmitMessage("Use at least 10 characters for your password.");
      return;
    }
    setSubmitting(true);
    setSubmitMessage("");
    setSubmitError(false);
    try {
      const result = resetMode
        ? await onPasswordReset(email)
        : signUpMode
          ? await onPasswordSignUp(name, email, password)
          : await onPasswordSignIn(email, password);
      setSubmitMessage(result.message);
      setSubmitError(!result.ok);
      if (result.ok && resetMode) setResetMode(false);
      if (result.ok && !resetMode) setPassword("");
    } catch {
      setSubmitError(true);
      setSubmitMessage("Something went wrong while contacting the account service. Try again.");
    } finally {
      setSubmitting(false);
    }
  }

  const dialog = <section ref={dialogRef} className="account-modal" role="dialog" aria-modal="true" aria-labelledby="account-prompt-title" aria-describedby="account-prompt-description">
    <header>
      <div>
        <span>{resetMode ? "Reset your password" : signUpMode ? "Create your account" : "Welcome back"}</span>
        <h2 id="account-prompt-title">{resetMode ? "Reset your password." : signUpMode ? "Sign up to analyze." : "Sign in to analyze."}</h2>
        <p id="account-prompt-description">{resetMode
          ? "We will send a time-limited reset link if an account matches that email."
          : signUpMode
            ? "Create an account to start free chess analysis."
            : "Sign in to continue your chess analysis."}</p>
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
      {resetMode ? <form className="account-form" noValidate aria-describedby={submitMessage ? "account-form-status" : undefined} onSubmit={(event) => void submit(event)}>
        <div className="account-field">
          <label htmlFor="account-reset-email">Email</label>
          <input id="account-reset-email" name="email" type="email" inputMode="email" autoComplete="email" required value={email} onChange={(event) => setEmail(event.target.value)} disabled={!auth.configured || submitting} />
        </div>
        <button type="submit" className="account-primary account-form-submit" disabled={!auth.configured || submitting}>{submitting ? "Sending…" : "Send reset link"}</button>
        <button type="button" className="account-text-button" onClick={() => { setResetMode(false); setSubmitMessage(""); setSubmitError(false); }}>Back to sign in</button>
      </form> : <form className="account-form" noValidate aria-describedby={submitMessage ? "account-form-status" : undefined} onSubmit={(event) => void submit(event)}>
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
            <input id="account-password" name="password" type={showPassword ? "text" : "password"} autoComplete={signUpMode ? "new-password" : "current-password"} minLength={signUpMode ? 10 : undefined} required value={password} onChange={(event) => setPassword(event.target.value)} disabled={!auth.configured || submitting} />
            <button type="button" className="account-password-toggle" onClick={() => setShowPassword((value) => !value)} aria-label={showPassword ? "Hide password" : "Show password"} aria-pressed={showPassword} title={showPassword ? "Hide password" : "Show password"}><Icon name={showPassword ? "eye-off" : "eye"} /></button>
          </div>
          {signUpMode && <small className="account-field-hint">Use at least 10 characters. Password managers and paste are welcome.</small>}
        </div>
        <button type="submit" className="account-primary account-form-submit" disabled={!auth.configured || submitting}>{submitting ? (signUpMode ? "Creating account…" : "Signing in…") : signUpMode ? "Create account" : "Sign in"}</button>
        {!signUpMode && <button type="button" className="account-text-button account-forgot" onClick={() => { setResetMode(true); setSubmitMessage(""); setSubmitError(false); }}>Forgot password?</button>}
      </form>}

      {!resetMode && <>
        <div className="account-divider"><span>Or continue with</span></div>
        <div className="account-providers" aria-label={signUpMode ? "Sign-up providers" : "Sign-in providers"}>
          {providers.map(({ id }) => <button
            key={id}
            type="button"
            disabled={busyProvider !== null || !auth.configured || submitting}
            onClick={() => onProvider(id)}
          >
            <span className="account-provider-mark" aria-hidden="true"><ProviderLogo provider={id} /></span>
            <span><strong>{signUpMode ? "Sign up with" : "Sign in with"} {authProviderLabel(id)}</strong><small>{auth.configured ? "Secure provider authentication" : "Not configured in this build"}</small></span>
            <span className="account-provider-arrow" aria-hidden="true">{busyProvider === id ? "…" : "→"}</span>
          </button>)}
        </div>
      </>}

      {auth.local && <div className="account-local-access">
        <button type="button" className="account-secondary" onClick={onLocalSignIn}>Use local test account</button>
        <small>Development only · uses the local C++ repository</small>
      </div>}

      <p id="account-form-status" className={`account-prompt-status ${submitError ? "is-error" : ""}`} role={submitError ? "alert" : "status"} aria-live="polite">{statusMessage}</p>
      <footer>
        <small>All data is secure.</small>
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

function ProviderLogo({ provider }: { provider: AuthProvider }) {
  if (provider === "google") {
    return <svg className="account-provider-logo account-provider-logo-google" viewBox="0 0 24 24" aria-hidden="true">
      <path fill="#4285f4" d="M21.35 12.27c0-.79-.07-1.55-.22-2.27H12v4.3h5.24a4.48 4.48 0 0 1-1.94 2.94v2.45h3.14c1.84-1.69 2.91-4.18 2.91-7.42Z" />
      <path fill="#34a853" d="M12 21.75c2.63 0 4.84-.87 6.45-2.36l-3.14-2.45c-.87.58-1.98.92-3.31.92-2.54 0-4.69-1.72-5.46-4.03H3.3v2.53A9.74 9.74 0 0 0 12 21.75Z" />
      <path fill="#fbbc05" d="M6.54 13.83A5.86 5.86 0 0 1 6.23 12c0-.64.11-1.26.31-1.83V7.64H3.3A9.75 9.75 0 0 0 2.25 12c0 1.57.38 3.05 1.05 4.36l3.24-2.53Z" />
      <path fill="#ea4335" d="M12 6.14c1.43 0 2.71.49 3.72 1.45l2.79-2.79C16.84 3.26 14.63 2.25 12 2.25a9.74 9.74 0 0 0-8.7 5.39l3.24 2.53C7.31 7.86 9.46 6.14 12 6.14Z" />
    </svg>;
  }
  return <svg className="account-provider-logo account-provider-logo-github" viewBox="0 0 24 24" aria-hidden="true">
    <path fill="currentColor" d="M12 .5a11.5 11.5 0 0 0-3.64 22.41c.58.1.79-.25.79-.56v-2.17c-3.22.7-3.9-1.37-3.9-1.37-.53-1.34-1.28-1.7-1.28-1.7-1.05-.72.08-.7.08-.7 1.16.08 1.77 1.19 1.77 1.19 1.03 1.76 2.71 1.25 3.37.95.1-.74.4-1.25.73-1.54-2.57-.29-5.28-1.29-5.28-5.72 0-1.26.45-2.3 1.19-3.11-.12-.29-.52-1.47.11-3.06 0 0 .97-.31 3.17 1.19a10.98 10.98 0 0 1 5.77 0c2.2-1.5 3.17-1.19 3.17-1.19.63 1.59.23 2.77.11 3.06.74.81 1.19 1.85 1.19 3.11 0 4.44-2.71 5.43-5.29 5.72.42.36.78 1.08.78 2.18v3.2c0 .31.21.67.8.56A11.5 11.5 0 0 0 12 .5Z" />
  </svg>;
}

function displayName(metadata: Record<string, unknown>, email: string | undefined): string {
  for (const key of ["full_name", "name", "user_name"]) {
    const value = metadata[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  return email || "Plywise account";
}

export function PasswordResetPrompt({ auth, message, onSubmit, onCancel, onComplete }: {
  auth: AuthSnapshot;
  message: string;
  onSubmit: (password: string) => Promise<AuthResult>;
  onCancel: () => void;
  onComplete: () => void;
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
    <button className="account-page-brand" type="button" onClick={onCancel}>Plywise</button>
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
        <button type="button" className="account-primary account-form-submit" onClick={onComplete}>Continue to Plywise</button>
      </> : <form className="account-form" aria-describedby={message ? "password-reset-status" : undefined} onSubmit={(event) => void submit(event)}>
        <div className="account-field">
          <label htmlFor="account-new-password">New password</label>
          <div className="account-password-field">
            <input id="account-new-password" type={showPassword ? "text" : "password"} autoComplete="new-password" minLength={10} required value={password} onChange={(event) => setPassword(event.target.value)} disabled={!auth.configured || busy} />
            <button type="button" className="account-password-toggle" onClick={() => setShowPassword((value) => !value)} aria-label={showPassword ? "Hide password" : "Show password"} aria-pressed={showPassword} title={showPassword ? "Hide password" : "Show password"}><Icon name={showPassword ? "eye-off" : "eye"} /></button>
          </div>
        </div>
        <button type="submit" className="account-primary account-form-submit" disabled={!auth.configured || busy}>{busy ? "Updating…" : "Update password"}</button>
        {message && <p id="password-reset-status" className="account-prompt-status" role="status" aria-live="polite">{message}</p>}
      </form>}
    </section>
  </div></main>;
}

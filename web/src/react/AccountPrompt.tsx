import { useEffect, useRef, useState } from "react";
import { Icon } from "./Icon";

type AccountContext = "landing" | "save";
type Provider = "Google" | "Apple" | "GitHub";

const providers: Provider[] = ["Google", "Apple", "GitHub"];

export function AccountPrompt({ context, onClose }: { context: AccountContext; onClose: () => void }) {
  const closeRef = useRef<HTMLButtonElement>(null);
  const [selectedProvider, setSelectedProvider] = useState<Provider | null>(null);

  useEffect(() => {
    closeRef.current?.focus();
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onClose]);

  const title = context === "save" ? "Keep this review." : "Sign in when you are ready.";
  const description = context === "save"
    ? "Create an account to keep this review across devices. Your guest review stays available while sign-in is being configured."
    : "Account entry will connect saved games and history. You can still review one completed game without an account.";

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

      <div className="account-providers" aria-label="Sign-in providers">
        {providers.map((provider) => <button key={provider} type="button" onClick={() => setSelectedProvider(provider)}>
          <span className="account-provider-mark" aria-hidden="true">{provider[0]}</span>
          <span><strong>Continue with {provider}</strong><small>Provider connection coming next</small></span>
          <span className="account-provider-arrow" aria-hidden="true">→</span>
        </button>)}
      </div>

      <p className="account-prompt-status" role="status">
        {selectedProvider
          ? `${selectedProvider} sign-in is not configured yet. No account data was sent.`
          : "Choose a provider when the hosted account service is connected."}
      </p>
      <footer>
        <small>Guest analysis stays free and does not require a password.</small>
        <button type="button" onClick={onClose}>Continue as guest</button>
      </footer>
    </section>
  </div>;
}

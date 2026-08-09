import { ChessBoard } from "./Board";
import { Icon } from "./Icon";

const previewFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

export function LandingView({ onStart, onSignIn }: {
  onStart: () => void;
  onSignIn: () => void;
}) {
  const scrollTo = (id: string) => document.getElementById(id)?.scrollIntoView({ behavior: "smooth", block: "start" });

  return <main className="landing-page">
    <div className="landing-shell">
      <header className="landing-header">
        <button className="landing-brand" onClick={() => window.scrollTo({ top: 0, behavior: "smooth" })}>
          <span className="landing-brand-mark"><img src="/pieces/lasker/white_knight.svg" alt="" width={24} height={24} /></span>
          <span>Plywise</span>
        </button>
        <nav className="landing-nav" aria-label="Landing page">
          <button onClick={() => scrollTo("landing-flow")}>How it works</button>
          <button onClick={() => scrollTo("landing-intelligence")}>What’s next</button>
        </nav>
        <button className="landing-signin" onClick={onSignIn}>Sign in</button>
      </header>

      <section className="landing-hero" aria-labelledby="landing-title">
        <div className="landing-copy">
          <h1 id="landing-title">Free chess game analysis.</h1>
          <p className="landing-lede">Bring a public link or PGN, then follow the review. Create an account to get started.</p>
          <div className="landing-actions">
            <button className="landing-primary" onClick={onStart}>Sign up to analyze <Icon name="analysis" /></button>
            <button className="landing-secondary" onClick={onSignIn}>Already have an account? Sign in <span aria-hidden="true">→</span></button>
          </div>
        </div>

        <section className="landing-preview" aria-label="Board-first review preview">
          <span className="landing-preview-header"><span>Board-first review</span><small>Built for finished games</small></span>
          <div className="landing-preview-body">
            <div className="landing-preview-board"><ChessBoard fen={previewFen} orientation="white" compact={false} interactive onSquare={onStart} /></div>
            <div className="landing-preview-copy">
              <span>Review, not noise</span>
              <strong>Start with the position.</strong>
              <p>See the verdict first. Open the engine line when you want the detail.</p>
              <div className="landing-preview-rule"><Icon name="analysis" /><span>Analysis starts when you choose a game.</span></div>
            </div>
          </div>
        </section>
      </section>

      <section className="landing-flow" id="landing-flow" aria-labelledby="landing-flow-title">
        <div className="landing-section-heading">
          <p className="landing-eyebrow">The flow</p>
          <h2 id="landing-flow-title">Three steps. No clutter.</h2>
        </div>
        <ol className="landing-steps">
          <li><span>1</span><div><h3>Sign in</h3><p>Your reviews belong to your account, so they are still there when you come back.</p></div></li>
          <li><span>2</span><div><h3>Add a finished game</h3><p>Paste a public Chess.com link or drop in a PGN. Live games stay out of scope.</p></div></li>
          <li><span>3</span><div><h3>Review the important moments</h3><p>Move through the board, compare the engine’s line, and try a better idea.</p></div></li>
        </ol>
      </section>

      <section className="landing-intelligence" id="landing-intelligence" aria-labelledby="landing-intelligence-title">
        <div><p className="landing-eyebrow">Coming later</p><h2 id="landing-intelligence-title">Your games will teach the system what to look for.</h2></div>
        <p>Personal patterns, practice positions, and progress are on the roadmap. They will be tied to real games and positions, not made-up scores.</p>
      </section>

      <footer className="landing-footer"><span>Plywise</span><span>Free finished-game analysis · open source · independent from Chess.com</span></footer>
    </div>
  </main>;
}

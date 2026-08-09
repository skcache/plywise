import { ChessBoard } from "./Board";
import { Icon } from "./Icon";

const previewFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

export function LandingView({ onStart, onSignIn, accountMessage }: {
  onStart: () => void;
  onSignIn: () => void;
  accountMessage: string;
}) {
  const scrollTo = (id: string) => document.getElementById(id)?.scrollIntoView({ behavior: "smooth", block: "start" });

  return <main className="landing-page">
    <div className="landing-shell">
      <header className="landing-header">
        <button className="landing-brand" onClick={() => window.scrollTo({ top: 0, behavior: "smooth" })}>
          <span className="landing-brand-mark"><img src="/pieces/lasker/white_knight.svg" alt="" /></span>
          <span>Plywise</span>
        </button>
        <nav className="landing-nav" aria-label="Landing page">
          <button onClick={() => scrollTo("landing-flow")}>How it works</button>
          <button onClick={() => scrollTo("landing-intelligence")}>Personal intelligence</button>
        </nav>
        <button className="landing-signin" onClick={onSignIn}>Sign in</button>
      </header>

      <section className="landing-hero" aria-labelledby="landing-title">
        <div className="landing-copy">
          <p className="landing-eyebrow"><span aria-hidden="true" /> Open-source chess review</p>
          <h1 id="landing-title">Free analysis for the games you actually played.</h1>
          <p className="landing-lede">Bring in one completed game, see what changed on the board, and understand the moments worth revisiting. Start as a guest. Keep your history when you are ready.</p>
          <div className="landing-actions">
            <button className="landing-primary" onClick={onStart}>Analyze a game free <Icon name="analysis" /></button>
            <button className="landing-secondary" onClick={onSignIn}>Sign in when you want saved history <span aria-hidden="true">→</span></button>
          </div>
          <p className="landing-note"><Icon name="check" /> One completed game at a time · no subscription · no Chess.com password</p>
          {accountMessage && <p className="landing-status" role="status">{accountMessage}</p>}
        </div>

        <div className="landing-preview" aria-label="Board-first review preview">
          <div className="landing-preview-header">
            <span><i aria-hidden="true" /> Board-first review</span>
            <small>Built for completed games</small>
          </div>
          <div className="landing-preview-body">
            <div className="landing-preview-board" aria-hidden="true"><ChessBoard fen={previewFen} orientation="white" compact /></div>
            <div className="landing-preview-copy">
              <span>Review, not noise</span>
              <h2>See the position. Understand the moment.</h2>
              <p>Start with the verdict and the board. Engine detail stays available when you want to go deeper.</p>
              <div className="landing-preview-rule"><Icon name="analysis" /><span>Analysis begins only after you choose a game.</span></div>
            </div>
          </div>
        </div>
      </section>

      <section className="landing-flow" id="landing-flow" aria-labelledby="landing-flow-title">
        <div className="landing-section-heading">
          <p className="landing-eyebrow">A small, useful loop</p>
          <h2 id="landing-flow-title">From game to understanding in a few quiet steps.</h2>
        </div>
        <ol className="landing-steps">
          <li><span>01</span><div><h3>Bring in a game</h3><p>Paste a public game link or PGN. Completed games only.</p></div></li>
          <li><span>02</span><div><h3>Review what changed</h3><p>Follow the board, key moments, explanations, and legal alternatives.</p></div></li>
          <li><span>03</span><div><h3>Keep it when useful</h3><p>Save a review to an account when you want history across devices.</p></div></li>
        </ol>
      </section>

      <section className="landing-intelligence" id="landing-intelligence" aria-labelledby="landing-intelligence-title">
        <div><p className="landing-eyebrow">Coming next</p><h2 id="landing-intelligence-title">Personal intelligence, grounded in your own games.</h2></div>
        <p>Once there is enough evidence, Plywise will connect reviews across time: recurring weaknesses, useful practice positions, and progress you can actually trace back to the board.</p>
      </section>

      <footer className="landing-footer"><span>Plywise</span><span>Free completed-game analysis · open source · independent from Chess.com</span></footer>
    </div>
  </main>;
}

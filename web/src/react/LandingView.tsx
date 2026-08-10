import { type PointerEvent } from "react";
import { ChessBoard } from "./Board";
import { Icon } from "./Icon";

const reviewFen = "r1b1r1k1/ppq2ppp/3p1n2/3Pp3/2P5/5NP1/PPQ2PBP/R3R1K1 w - - 0 18";
const demoFen = "r2q1rk1/ppp1bppp/2np1n2/8/2BPP3/2N1BN2/PPP2PPP/R2Q1RK1 w - - 0 10";

function setPreviewTilt(event: PointerEvent<HTMLElement>, reset = false) {
  const preview = event.currentTarget;
  if (reset) {
    preview.style.setProperty("--preview-tilt-x", "0deg");
    preview.style.setProperty("--preview-tilt-y", "0deg");
    preview.style.setProperty("--preview-lift", "0px");
    return;
  }

  const bounds = preview.getBoundingClientRect();
  const x = ((event.clientX - bounds.left) / bounds.width - 0.5) * 2;
  const y = ((event.clientY - bounds.top) / bounds.height - 0.5) * 2;
  preview.style.setProperty("--preview-tilt-x", `${(-y * 2.8).toFixed(2)}deg`);
  preview.style.setProperty("--preview-tilt-y", `${(x * 2.8).toFixed(2)}deg`);
  preview.style.setProperty("--preview-lift", `${(1.5 - y * 1.5).toFixed(2)}px`);
}

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
        </nav>
        <button className="landing-signin" onClick={onSignIn}>Sign in</button>
      </header>

      <section className="landing-hero" aria-labelledby="landing-title">
        <div className="landing-copy">
          <h1 id="landing-title">Free chess analysis.</h1>
          <p className="landing-lede">Bring a public game link or PGN, then review the game.</p>
          <div className="landing-actions">
            <button className="landing-primary" onClick={onStart}>Sign up to analyze <Icon name="analysis" /></button>
            <button className="landing-secondary" onClick={onSignIn}>Already have an account? Sign in</button>
          </div>
        </div>

        <section
          className="landing-preview"
          aria-label="Game analysis preview"
          onPointerMove={setPreviewTilt}
          onPointerLeave={(event) => setPreviewTilt(event, true)}
        >
          <span className="landing-preview-header"><span>Game analysis</span><small>Report</small></span>
          <div className="landing-preview-body">
            <div className="landing-preview-board">
              <ChessBoard fen={demoFen} orientation="white" compact={false} showCoordinates={false} interactive activeUci="c4f7" showArrow onSquare={onStart} />
            </div>
            <aside className="landing-preview-inspector" aria-label="Analysis report preview">
              <div className="landing-demo-tabs"><strong>Report</strong><span>Moves</span></div>
              <div className="landing-demo-classifications" aria-label="Illustrative move classifications">
                <div className="landing-demo-classification-head"><span>Move quality</span><span>White</span><span>Black</span></div>
                <div className="landing-demo-classification-row"><span><i className="is-brilliant" />Brilliant</span><b>0</b><b>0</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-great" />Great</span><b>0</b><b>0</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-best" />Best</span><b>14</b><b>9</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-excellent" />Excellent</span><b>11</b><b>15</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-good" />Good</span><b>3</b><b>6</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-book" />Book</span><b>5</b><b>5</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-inaccuracy" />Inaccuracy</span><b>1</b><b>0</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-mistake" />Mistake</span><b>1</b><b>1</b></div>
                <div className="landing-demo-classification-row"><span><i className="is-blunder" />Blunder</span><b>0</b><b>0</b></div>
              </div>
            </aside>
          </div>
        </section>
      </section>

      <section className="landing-flow" id="landing-flow" aria-labelledby="landing-flow-title">
        <div className="landing-section-heading">
          <p className="landing-eyebrow">How it works</p>
          <h2 id="landing-flow-title">From game link to useful review.</h2>
        </div>
        <div className="landing-visual-flow">
          <article className="landing-flow-stage">
            <div className="landing-stage-heading"><strong>Bring a game</strong></div>
            <div className="landing-stage-input" role="img" aria-label="Paste a public game link or PGN">
              <span>chess.com/game/...</span><b>Analyze</b>
            </div>
            <p>Paste a public link or PGN.</p>
          </article>
          <article className="landing-flow-stage">
            <div className="landing-stage-heading"><strong>Review</strong></div>
            <div className="landing-stage-loading" role="img" aria-label="Reviewing the game">
              <span className="landing-thinking-line" aria-hidden="true">
                <span className="landing-thinking-text" data-text="Reviewing the game...">Reviewing the game...</span>
              </span>
            </div>
            <p>Let the review run when you are ready.</p>
          </article>
          <article className="landing-flow-stage landing-flow-stage-board">
            <div className="landing-stage-heading"><strong>Follow the review</strong></div>
            <div className="landing-stage-board" role="img" aria-label="Chess analysis board with a suggested move">
              <ChessBoard fen={reviewFen} orientation="white" compact activeUci="c2f5" showArrow />
              <span>Analysis ready</span>
            </div>
            <p>See the position and the move worth another look.</p>
          </article>
        </div>
      </section>
    </div>
  </main>;
}

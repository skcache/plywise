import { moveOverlayGeometry, squaresFromFen, uciSquares, type BoardOrientation } from "../chess";

const blackPieces = new Set(["♟", "♜", "♞", "♝", "♛", "♚"]);
const pieceKinds: Record<string, string> = {
  "♔": "king", "♕": "queen", "♖": "rook", "♗": "bishop", "♘": "knight", "♙": "pawn",
  "♚": "king", "♛": "queen", "♜": "rook", "♝": "bishop", "♞": "knight", "♟": "pawn",
};

export function ChessBoard({ fen, orientation, activeUci = "", sourceSquare = "", interactive = false, showArrow = false, compact = false, showCoordinates = true, onSquare }: {
  fen: string;
  orientation: BoardOrientation;
  activeUci?: string;
  sourceSquare?: string;
  interactive?: boolean;
  showArrow?: boolean;
  compact?: boolean;
  showCoordinates?: boolean;
  onSquare?: (square: string) => void;
}) {
  const highlighted = uciSquares(activeUci);
  const squares = squaresFromFen(fen, orientation);
  const geometry = showArrow && activeUci ? moveOverlayGeometry(activeUci, orientation) : null;
  return <div className={`chess-board ${compact ? "compact-board" : ""}`} role="grid" aria-label={compact ? "Position preview" : "Chess position"}>
    {squares.map((square, index) => {
      const light = (Math.floor(index / 8) + index) % 2 === 0;
      const from = highlighted?.[0] === square.name;
      const to = highlighted?.[1] === square.name;
      const selected = sourceSquare === square.name;
      const side = blackPieces.has(square.piece) ? "black" : "white";
      const kind = pieceKinds[square.piece];
      const squareContent = <>
        {!compact && showCoordinates && index % 8 === 0 && <span className="rank-label">{square.rank}</span>}
        {!compact && showCoordinates && index >= 56 && <span className="file-label">{square.file}</span>}
        {kind && (
          <img className="board-piece" src={`/pieces/lasker/${side}_${kind}.svg`} alt="" draggable={false}/>
        )}
      </>;
      const className = `board-square ${light ? "light" : "dark"} ${from ? "from" : ""} ${to ? "to" : ""} ${selected ? "source" : ""}`;
      const label = interactive ? `Choose ${square.name}` : `${kind ? `${kind} on` : "empty"} ${square.name}`;
      return interactive ? <button
        type="button"
        key={`${square.name}-${index}`}
        className={className}
        aria-label={label}
        onClick={() => onSquare?.(square.name)}
      >{squareContent}</button> : <div
        key={`${square.name}-${index}`}
        className={className}
        role="gridcell"
        aria-label={label}
      >{squareContent}</div>;
    })}
    {geometry && <svg className="best-arrow" viewBox="0 0 100 100" aria-hidden="true"><defs><marker id="react-arrowhead" markerWidth="4" markerHeight="4" viewBox="0 0 4 4" refX="3.5" refY="2" orient="auto"><path d="M0 0 4 2 0 4Z"/></marker></defs><line x1={geometry.source.x} y1={geometry.source.y} x2={geometry.destination.x} y2={geometry.destination.y} markerEnd="url(#react-arrowhead)"/></svg>}
  </div>;
}

export function EvaluationBar({ value }: { value?: number }) {
  const clamped = Math.max(-600, Math.min(600, value ?? 0));
  const whiteShare = Math.max(4, Math.min(96, 50 + clamped / 12));
  const label = formatEval(value);
  return <aside className="evaluation-column" aria-label={`Current engine evaluation ${label}`}>
    <strong>{label}</strong>
    <svg className="evaluation-track" viewBox="0 0 10 100" preserveAspectRatio="none" aria-hidden="true">
      <rect className="evaluation-track-base" x="0" y="0" width="10" height="100" rx="5"/>
      <rect className="evaluation-track-value" x="0" y={100 - whiteShare} width="10" height={whiteShare} rx="5"/>
      <line className="evaluation-track-zero" x1="0" y1="50" x2="10" y2="50"/>
    </svg>
    <small>{formatEval(value === undefined ? undefined : -value)}</small>
  </aside>;
}

export function formatEval(value?: number) {
  if (value === undefined) return "—";
  const pawns = value / 100;
  return `${pawns > 0 ? "+" : ""}${pawns.toFixed(2)}`;
}

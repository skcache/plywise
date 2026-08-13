import { useEffect, useRef, useState, type KeyboardEvent } from "react";
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
  const [focusIndex, setFocusIndex] = useState(0);
  const squareRefs = useRef<Array<HTMLButtonElement | null>>([]);

  useEffect(() => {
    if (!interactive) return;
    const selectedIndex = squares.findIndex((square) => square.name === sourceSquare);
    setFocusIndex(selectedIndex >= 0 ? selectedIndex : 0);
  }, [interactive, orientation, sourceSquare]);

  const moveFocus = (index: number, event: KeyboardEvent<HTMLButtonElement>) => {
    const row = Math.floor(index / 8);
    const column = index % 8;
    let next = index;
    if (event.key === "ArrowLeft") next = row * 8 + (column + 7) % 8;
    else if (event.key === "ArrowRight") next = row * 8 + (column + 1) % 8;
    else if (event.key === "ArrowUp") next = ((row + 7) % 8) * 8 + column;
    else if (event.key === "ArrowDown") next = ((row + 1) % 8) * 8 + column;
    else if (event.key === "Home") next = row * 8;
    else if (event.key === "End") next = row * 8 + 7;
    else return;
    event.preventDefault();
    setFocusIndex(next);
    squareRefs.current[next]?.focus();
  };

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
        ref={(element) => { squareRefs.current[index] = element; }}
        key={`${square.name}-${index}`}
        className={className}
        role="gridcell"
        aria-label={label}
        aria-selected={selected}
        tabIndex={index === focusIndex ? 0 : -1}
        onFocus={() => setFocusIndex(index)}
        onKeyDown={(event) => moveFocus(index, event)}
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
  const clamped = Math.max(-800, Math.min(800, value ?? 0));
  // Keep a large engine swing legible without letting the rail become a
  // thermometer. The rail is a visual summary; the exact score remains text.
  const whiteShare = Math.max(8, Math.min(92, 50 + Math.tanh(clamped / 340) * 42));
  const label = formatEval(value);
  const side = value === undefined ? "Even position" : value >= 0 ? `White advantage, ${label}` : `Black advantage, ${formatEval(Math.abs(value))}`;
  return <aside className="evaluation-column" aria-label={`Current engine evaluation: ${side}`}>
    <span className="evaluation-value">{label}</span>
    <svg className="evaluation-rail" viewBox="0 0 10 100" preserveAspectRatio="none" aria-hidden="true">
      <rect className="evaluation-rail-black" x="0" y="0" width="10" height="100" />
      <rect className="evaluation-rail-white" x="0" y={100 - whiteShare} width="10" height={whiteShare} />
      <line className="evaluation-rail-marker" x1="0" x2="10" y1={100 - whiteShare} y2={100 - whiteShare} vectorEffect="non-scaling-stroke" />
    </svg>
  </aside>;
}

export function formatEval(value?: number) {
  if (value === undefined) return "—";
  const pawns = value / 100;
  return `${pawns > 0 ? "+" : ""}${pawns.toFixed(2)}`;
}

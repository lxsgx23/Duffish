/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2022 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <limits>

#include "movepick.h"

namespace Stockfish {

namespace {

  constexpr bool XiangqiHasMandatoryCapture = false;

  enum Stages {
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, REFUTATION, QUIET_INIT, GOOD_QUIET, BAD_CAPTURE, BAD_QUIET,
    EVASION_TT, EVASION_INIT, EVASION,
    PROBCUT_TT, PROBCUT_INIT, PROBCUT,
    QSEARCH_TT, QCAPTURE_INIT, QCAPTURE, QCHECK_INIT, QCHECK
  };

  // partial_insertion_sort() sorts moves in descending order up to and including
  // a given limit. The order of moves smaller than the limit is left unspecified.
  void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {

    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit)
        {
            ExtMove tmp = *p, *q;
            *p = *++sortedEnd;
            for (q = sortedEnd; q != begin && *(q - 1) < tmp; --q)
                *q = *(q - 1);
            *q = tmp;
        }
  }

  bool xiangqi_breaking_capture(Piece pc) {
      PieceType pt = type_of(pc);
      return pt == FERS || pt == ELEPHANT || pt == CANNON || pt == HORSE || pt == ROOK;
  }

  bool xiangqi_attacker_type(PieceType pt) {
      return pt == ROOK || pt == CANNON || pt == HORSE || pt == SOLDIER;
  }

  bool xiangqi_trade_piece(PieceType pt) {
      return pt == ROOK || pt == CANNON || pt == HORSE;
  }

  bool xiangqi_near_enemy_palace(const Position& pos, Color us, Square s) {
      Color them = ~us;
      if (!pos.count<KING>(them))
          return false;

      Square ksq = pos.square<KING>(them);
      return distance<File>(s, ksq) <= 1 && distance<Rank>(s, ksq) <= 3;
  }

  Bitboard xiangqi_attacks_by(const Position& pos, Color c, PieceType pt) {
      Bitboard attacks = 0;
      Bitboard pieces = pos.pieces(c, pt);

      while (pieces)
      {
          Square s = pop_lsb(pieces);
          attacks |= pos.attacks_from(c, pt, s);
      }

      return attacks;
  }

  bool xiangqi_same_orthogonal_line(Square a, Square b) {
      return file_of(a) == file_of(b) || rank_of(a) == rank_of(b);
  }

  Bitboard xiangqi_between_without_target(Square from, Square target) {
      return between_bb(from, target) ^ square_bb(target);
  }

  int xiangqi_cannon_battery_bonus(const Position& pos, Move m, int attackBias) {
      if (attackBias <= 0)
          return 0;

      Color us = pos.side_to_move();
      Color them = ~us;
      if (!pos.count<KING>(them))
          return 0;

      Square from = from_sq(m);
      Square to = to_sq(m);
      Square ksq = pos.square<KING>(them);
      PieceType pt = type_of(pos.moved_piece(m));
      Bitboard toBB = square_bb(to);
      Bitboard occupiedAfter = (pos.pieces() ^ square_bb(from)) | toBB;
      int score = 0;

      if (pt == CANNON && xiangqi_same_orthogonal_line(to, ksq))
      {
          Bitboard screens = xiangqi_between_without_target(to, ksq) & occupiedAfter;
          if (popcount(screens) == 1)
              score += pos.gives_check(m) ? 132 : 96;
      }

      Bitboard cannons = pos.pieces(us, CANNON);
      while (cannons)
      {
          Square csq = pop_lsb(cannons);
          if (csq == from || !xiangqi_same_orthogonal_line(csq, ksq))
              continue;

          Bitboard screens = xiangqi_between_without_target(csq, ksq) & occupiedAfter;
          if (popcount(screens) == 1 && (screens & toBB))
              score += pos.gives_check(m) ? 112 : 78;
          else if (popcount(screens) == 2 && (screens & toBB))
              score += 32;
      }

      if (score && xiangqi_near_enemy_palace(pos, us, to))
          score += 24;

      return score * attackBias / 28;
  }

  int xiangqi_quiet_threat_bonus(const Position& pos, Move m, int attackBias) {
      if (attackBias <= 0)
          return 0;

      Color us = pos.side_to_move();
      Color them = ~us;
      if (!pos.count<KING>(them))
          return 0;

      Piece moved = pos.moved_piece(m);
      PieceType pt = type_of(moved);
      if (!xiangqi_attacker_type(pt))
          return 0;

      Square to = to_sq(m);
      Square ksq = pos.square<KING>(them);
      bool nearPalace = xiangqi_near_enemy_palace(pos, us, to);
      bool linePressure = (pt == ROOK || pt == CANNON)
                       && (file_of(to) == file_of(ksq) || rank_of(to) == rank_of(ksq));
      bool advancedSoldier = pt == SOLDIER
                           && relative_rank(us, to, pos.max_rank()) >= 5
                           && distance<File>(to, ksq) <= 2;

      if (!nearPalace && !linePressure && !advancedSoldier)
          return 0;

      Bitboard attacks = pos.attacks_from(us, pt, to);
      int bonus = 0;

      if (nearPalace)
          bonus += 48;
      if (attacks & ksq)
          bonus += 96;
      if (linePressure)
          bonus += 64;
      if (attacks & (pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT)))
          bonus += 64;

      return bonus * attackBias / 16;
  }

  int xiangqi_capture_simplification_penalty(const Position& pos, Move m, int attackBias) {
      if (attackBias <= 0 || !pos.capture(m))
          return 0;

      Piece moved = pos.moved_piece(m);
      Piece captured = pos.piece_on(to_sq(m));
      PieceType movedType = type_of(moved);
      PieceType capturedType = type_of(captured);

      if (!xiangqi_trade_piece(movedType) || !xiangqi_trade_piece(capturedType))
          return 0;

      if (pos.gives_check(m))
          return 0;

      int movedValue = int(PieceValue[MG][moved]);
      int capturedValue = int(PieceValue[MG][captured]);
      if (capturedValue > movedValue + 180)
          return 0;

      Color us = pos.side_to_move();
      Color them = ~us;
      if (!pos.count<KING>(them))
          return 0;

      Square to = to_sq(m);
      Square ksq = pos.square<KING>(them);
      Bitboard attacks = pos.attacks_from(us, movedType, to);
      if (xiangqi_near_enemy_palace(pos, us, to)
          || (attacks & (square_bb(ksq) | pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT))))
          return 0;

      Bitboard attackers = pos.pieces(us, ROOK) | pos.pieces(us, CANNON) | pos.pieces(us, HORSE);
      int pressure = popcount(pos.attackers_to(ksq, us) & attackers);
      if (!pressure && !xiangqi_near_enemy_palace(pos, us, from_sq(m)))
          return 0;

      return attackBias * (capturedValue >= movedValue - 180 ? 72 : 40);
  }

} // namespace


/// Constructors of the MovePicker class. As arguments we pass information
/// to help it to return the (presumably) good moves first, to decide which
/// moves to return (in the quiescence search, for instance, we only want to
/// search captures, promotions, and some checks) and how important good move
/// ordering is at the current node.

/// MovePicker constructor for the main search
MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const LowPlyHistory* lp,
                       const CapturePieceToHistory* cph, const PieceToHistory** ch, Move cm, const Move* killers, int pl,
                       const PawnPieceToHistory* ph, const AttackPieceToHistory* ah, int ab, Color rc)
           : pos(p), mainHistory(mh), lowPlyHistory(lp), captureHistory(cph), continuationHistory(ch),
             pawnHistory(ph), attackHistory(ah), ttMove(ttm), refutations{{killers[0], 0}, {killers[1], 0}, {cm, 0}}, depth(d), ply(pl),
             captureScoreScale(7), attackBias(ab), rootColor(rc) {

  assert(d > 0);

  stage = (pos.checkers() ? EVASION_TT : MAIN_TT) +
          !(ttm && pos.pseudo_legal(ttm));
}

/// MovePicker constructor for quiescence search
MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh,
                       const CapturePieceToHistory* cph, const PieceToHistory** ch, Square rs,
                       const PawnPieceToHistory* ph, const AttackPieceToHistory* ah, int ab, Color rc)
           : pos(p), mainHistory(mh), captureHistory(cph), continuationHistory(ch),
             pawnHistory(ph), attackHistory(ah), ttMove(ttm), recaptureSquare(rs), depth(d),
             captureScoreScale(7), attackBias(ab), rootColor(rc) {

  assert(d <= 0);

  stage = (pos.checkers() ? EVASION_TT : QSEARCH_TT) +
          !(   ttm
            && (pos.checkers() || depth > DEPTH_QS_RECAPTURES || to_sq(ttm) == recaptureSquare)
            && pos.pseudo_legal(ttm));
}

/// MovePicker constructor for ProbCut: we generate captures with SEE greater
/// than or equal to the given threshold.
MovePicker::MovePicker(const Position& p, Move ttm, Value th, const CapturePieceToHistory* cph)
           : pos(p), captureHistory(cph), pawnHistory(nullptr), attackHistory(nullptr), ttMove(ttm), threshold(th),
             captureScoreScale(6), attackBias(0), rootColor(COLOR_NB) {

  assert(!pos.checkers());

  stage = PROBCUT_TT + !(ttm && pos.capture(ttm)
                             && pos.pseudo_legal(ttm)
                             && pos.see_ge(ttm, threshold));
}

/// MovePicker::score() assigns a numerical value to each move in a list, used
/// for sorting. Captures are ordered by Most Valuable Victim (MVV), preferring
/// captures with a good history. Quiets moves are ordered using the histories.
template<GenType Type>
void MovePicker::score() {

  static_assert(Type == CAPTURES || Type == QUIETS || Type == EVASIONS, "Wrong type");

  Color us = pos.side_to_move();
  bool rootSide = rootColor < COLOR_NB && us == rootColor;
  int sideAttackBias = rootSide ? attackBias : 0;
  Bitboard threatByLesser[PIECE_TYPE_NB] = {};
  if constexpr (Type == QUIETS)
  {
      Color them = ~us;
      threatByLesser[SOLDIER] = 0;
      threatByLesser[FERS] = threatByLesser[ELEPHANT] = xiangqi_attacks_by(pos, them, SOLDIER);
      threatByLesser[HORSE] = threatByLesser[CANNON] =
          threatByLesser[FERS] | xiangqi_attacks_by(pos, them, FERS)
                                | xiangqi_attacks_by(pos, them, ELEPHANT);
      threatByLesser[ROOK] = threatByLesser[HORSE] | xiangqi_attacks_by(pos, them, HORSE)
                                                   | xiangqi_attacks_by(pos, them, CANNON);
      threatByLesser[KING] = 0;
  }

  for (auto& m : *this)
      if constexpr (Type == CAPTURES)
      {
          Piece captured = pos.piece_on(to_sq(m));
          Piece moved = pos.moved_piece(m);
          int to = history_square(to_sq(m));
          m.value =  int(PieceValue[MG][captured]) * captureScoreScale
                   + (*captureHistory)[history_slot(moved)][to][capture_history_slot(type_of(captured))]
                   + (attackHistory && rootSide ? (*attackHistory)[history_slot(moved)][to] : 0);

          if (sideAttackBias > 0 && captured)
          {
              if (xiangqi_breaking_capture(captured))
                  m.value += sideAttackBias * (type_of(captured) == FERS || type_of(captured) == ELEPHANT ? 96 : 42);

              if (pos.gives_check(m) && pos.see_ge(m, Value(-120)))
                  m.value += sideAttackBias * 72;

              m.value += xiangqi_cannon_battery_bonus(pos, m, sideAttackBias);
              m.value -= xiangqi_capture_simplification_penalty(pos, m, sideAttackBias);
          }
      }

      else if constexpr (Type == QUIETS)
      {
          Piece moved = pos.moved_piece(m);
          int to = history_square(to_sq(m));
          m.value =      (*mainHistory)[pos.side_to_move()][history_move(m)]
                   + 2 * (*continuationHistory[0])[history_slot(moved)][to]
                   +     (*continuationHistory[1])[history_slot(moved)][to]
                   +     (*continuationHistory[3])[history_slot(moved)][to]
                   +     (*continuationHistory[5])[history_slot(moved)][to]
                   + (ply < MAX_LPH ? std::min(4, depth / 3) * (*lowPlyHistory)[ply][history_move(m)] : 0);

          if (pawnHistory)
          {
              m.value += 2 * (*pawnHistory)[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)][history_slot(moved)][to];
              if (attackHistory && rootSide)
                  m.value += 2 * (*attackHistory)[history_slot(moved)][to];

              PieceType pt = type_of(moved);
              Bitboard checkTargets = pt == CANNON && pos.count<KING>(~us)
                                    ? pos.check_squares(pt) & ~line_bb(from_sq(m), pos.square<KING>(~us))
                                    : pos.check_squares(pt);
              if ((checkTargets & to_sq(m)) && pos.see_ge(m, Value(-75)))
                  m.value += 16384 + sideAttackBias * 64;

              m.value += xiangqi_quiet_threat_bonus(pos, m, sideAttackBias);
              m.value += xiangqi_cannon_battery_bonus(pos, m, sideAttackBias);

              int threatDelta = 20 * (bool(threatByLesser[pt] & from_sq(m))
                                    - bool(threatByLesser[pt] & to_sq(m)));
              m.value += int(PieceValue[MG][moved]) * threatDelta;
          }
      }

      else // Type == EVASIONS
      {
          if (pos.capture(m))
              m.value =  PieceValue[MG][pos.piece_on(to_sq(m))]
                       - Value(type_of(pos.moved_piece(m)));
          else
              m.value =      (*mainHistory)[pos.side_to_move()][history_move(m)]
                       + 2 * (*continuationHistory[0])[history_slot(pos.moved_piece(m))][history_square(to_sq(m))]
                       - (1 << 28);
      }
}

/// MovePicker::select() returns the next move satisfying a predicate function.
/// It never returns the TT move.
template<MovePicker::PickType T, typename Pred>
Move MovePicker::select(Pred filter) {

  while (cur < endMoves)
  {
      if (T == Best)
          std::swap(*cur, *std::max_element(cur, endMoves));

      if (*cur != ttMove && filter())
          return *cur++;

      cur++;
  }
  return MOVE_NONE;
}

/// MovePicker::next_move() is the most important method of the MovePicker class. It
/// returns a new pseudo-legal move every time it is called until there are no more
/// moves left, picking the move with the highest score from a list of generated moves.
Move MovePicker::next_move(bool skipQuiets) {

top:
  switch (stage) {

  case MAIN_TT:
  case EVASION_TT:
  case QSEARCH_TT:
  case PROBCUT_TT:
      ++stage;
      assert(pos.legal(ttMove) == MoveList<LEGAL>(pos).contains(ttMove));
      return ttMove;

  case CAPTURE_INIT:
  case PROBCUT_INIT:
  case QCAPTURE_INIT:
      cur = endBadCaptures = moves;
      endMoves = generate<CAPTURES>(pos, cur);

      score<CAPTURES>();
      partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
      ++stage;
      goto top;

  case GOOD_CAPTURE:
      if (select<Next>([&](){
                       return pos.see_ge(*cur, Value(-69 * cur->value / 1024))?
                              // Move losing capture to endBadCaptures to be tried later
                              true : (*endBadCaptures++ = *cur, false); }))
          return *(cur - 1);

      // Prepare the pointers to loop over the refutations array
      cur = std::begin(refutations);
      endMoves = std::end(refutations);

      // If the countermove is the same as a killer, skip it
      if (   refutations[0].move == refutations[2].move
          || refutations[1].move == refutations[2].move)
          --endMoves;

      ++stage;
      [[fallthrough]];

  case REFUTATION:
      if (select<Next>([&](){ return    *cur != MOVE_NONE
                                    && !pos.capture(*cur)
                                    &&  pos.pseudo_legal(*cur); }))
          return *(cur - 1);
      ++stage;
      [[fallthrough]];

  case QUIET_INIT:
      cur = endBadQuiets = endBadCaptures;
      if (!skipQuiets && !(XiangqiHasMandatoryCapture && pos.must_capture() && pos.has_capture()))
      {
          endMoves = generate<QUIETS>(pos, cur);

          score<QUIETS>();
          partial_insertion_sort(cur, endMoves, -3330 * depth);
      }
      else
          endMoves = cur;

      ++stage;
      [[fallthrough]];

  case GOOD_QUIET:
      if (   !skipQuiets
          && select<Next>([&](){
                 if (   *cur == refutations[0].move
                     || *cur == refutations[1].move
                     || *cur == refutations[2].move)
                     return false;

                 if (cur->value <= -14000)
                 {
                     *endBadQuiets++ = *cur;
                     return false;
                 }

                 return true;
             }))
          return *(cur - 1);

      // Prepare the pointers to loop over the bad captures
      cur = moves;
      endMoves = endBadCaptures;

      ++stage;
      [[fallthrough]];

  case BAD_CAPTURE:
      if (select<Next>([](){ return true; }))
          return *(cur - 1);

      cur = endBadCaptures;
      endMoves = endBadQuiets;

      ++stage;
      [[fallthrough]];

  case BAD_QUIET:
      if (!skipQuiets)
          return select<Next>([](){ return true; });

      return MOVE_NONE;

  case EVASION_INIT:
      cur = moves;
      endMoves = generate<EVASIONS>(pos, cur);

      score<EVASIONS>();
      partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
      ++stage;
      [[fallthrough]];

  case EVASION:
      return select<Next>([](){ return true; });

  case PROBCUT:
      return select<Next>([&](){ return pos.see_ge(*cur, threshold); });

  case QCAPTURE:
      if (select<Best>([&](){ return   depth > DEPTH_QS_RECAPTURES
                                    || to_sq(*cur) == recaptureSquare; }))
          return *(cur - 1);

      // If we did not find any move and we do not try checks, we have finished
      if (depth != DEPTH_QS_CHECKS)
          return MOVE_NONE;

      ++stage;
      [[fallthrough]];

  case QCHECK_INIT:
      cur = moves;
      endMoves = generate<QUIET_CHECKS>(pos, cur);

      ++stage;
      [[fallthrough]];

  case QCHECK:
      return select<Next>([](){ return true; });
  }

  assert(false);
  return MOVE_NONE; // Silence warning
}

} // namespace Stockfish

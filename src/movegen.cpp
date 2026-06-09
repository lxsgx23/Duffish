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

#include "movegen.h"
#include "position.h"

namespace Stockfish {

namespace {

  template<Color Us, GenType Type>
  ExtMove* generate_piece_moves(const Position& pos, ExtMove* moveList, PieceType pt, Bitboard target) {

    Bitboard bb = pos.pieces(Us, pt);

    while (bb)
    {
        Square from = pop_lsb(bb);

        Bitboard b = ((pos.attacks_from(Us, pt, from) & pos.pieces())
                    | (pos.moves_from(Us, pt, from) & ~pos.pieces())) & target;

        if (Type == QUIET_CHECKS)
            b &= pos.check_squares(pt);

        while (b)
            *moveList++ = make<NORMAL>(from, pop_lsb(b));
    }

    return moveList;
  }


  template<Color Us, GenType Type>
  ExtMove* generate_all(const Position& pos, ExtMove* moveList) {

    static_assert(Type != LEGAL, "Unsupported type in generate_all()");

    constexpr bool Checks = Type == QUIET_CHECKS; // Reduce template instantiations
    const Square ksq = pos.count<KING>(Us) ? pos.square<KING>(Us) : SQ_NONE;
    Bitboard target = Type == EVASIONS     ? between_bb(ksq, lsb(pos.checkers()))
                    : Type == NON_EVASIONS ? ~pos.pieces( Us)
                    : Type == CAPTURES     ?  pos.pieces(~Us)
                                           : ~pos.pieces(   ); // QUIETS || QUIET_CHECKS

    if (Type == EVASIONS)
    {
        if (pos.checkers() & pos.non_sliding_riders())
            target = ~pos.pieces(Us);
        // Leaper attacks can not be blocked
        Square checksq = lsb(pos.checkers());
        if (LeaperAttacks[~Us][type_of(pos.piece_on(checksq))][checksq] & pos.square<KING>(Us))
            target = pos.checkers();
    }

    target &= pos.board_bb();

    // Skip generating non-king moves when in double check
    if (Type != EVASIONS || !more_than_one(pos.checkers() & ~pos.non_sliding_riders()))
    {
        for (PieceType pt : { ROOK, HORSE, ELEPHANT, FERS, CANNON, SOLDIER })
            moveList = generate_piece_moves<Us, Type>(pos, moveList, pt, target);
    }

    // King moves
    if (pos.count<KING>(Us) && (!Checks || pos.blockers_for_king(~Us) & ksq))
    {
        Bitboard b = (  (pos.attacks_from(Us, KING, ksq) & pos.pieces())
                      | (pos.moves_from(Us, KING, ksq) & ~pos.pieces())) & (Type == EVASIONS ? ~pos.pieces(Us) : target);
        while (b)
            *moveList++ = make<NORMAL>(ksq, pop_lsb(b));

    }

    return moveList;
  }

} // namespace


/// <CAPTURES>     Generates all pseudo-legal captures plus queen promotions
/// <QUIETS>       Generates all pseudo-legal non-captures and underpromotions
/// <EVASIONS>     Generates all pseudo-legal check evasions when the side to move is in check
/// <QUIET_CHECKS> Generates all pseudo-legal non-captures giving check, except castling and promotions
/// <NON_EVASIONS> Generates all pseudo-legal captures and non-captures
///
/// Returns a pointer to the end of the move list.

template<GenType Type>
ExtMove* generate(const Position& pos, ExtMove* moveList) {

  static_assert(Type != LEGAL, "Unsupported type in generate()");
  assert((Type == EVASIONS) == (bool)pos.checkers());

  Color us = pos.side_to_move();

  return us == WHITE ? generate_all<WHITE, Type>(pos, moveList)
                     : generate_all<BLACK, Type>(pos, moveList);
}

// Explicit template instantiations
template ExtMove* generate<CAPTURES>(const Position&, ExtMove*);
template ExtMove* generate<QUIETS>(const Position&, ExtMove*);
template ExtMove* generate<EVASIONS>(const Position&, ExtMove*);
template ExtMove* generate<QUIET_CHECKS>(const Position&, ExtMove*);
template ExtMove* generate<NON_EVASIONS>(const Position&, ExtMove*);


/// generate<LEGAL> generates all the legal moves in the given position

template<>
ExtMove* generate<LEGAL>(const Position& pos, ExtMove* moveList) {

  if (pos.is_immediate_game_end())
      return moveList;

  ExtMove* cur = moveList;

  moveList = pos.checkers() ? generate<EVASIONS    >(pos, moveList)
                            : generate<NON_EVASIONS>(pos, moveList);
  while (cur != moveList)
      if (!pos.legal(*cur))
          *cur = (--moveList)->move;
      else
          ++cur;

  return moveList;
}

} // namespace Stockfish

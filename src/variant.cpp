/*
  Fairy-Stockfish, a UCI chess variant playing engine derived from Stockfish
  Copyright (C) 2018-2022 Fabian Fichter

  Fairy-Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Fairy-Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cctype>
#include <cstring>
#include <string>

#include "piece.h"
#include "variant.h"

namespace Stockfish {

VariantMap variants; // Global object

namespace {

Variant* make_xiangqi_variant() {
    Variant* v = (new Variant())->init();

    v->maxRank = RANK_10;
    v->maxFile = FILE_I;

    v->reset_pieces();
    v->add_piece(ROOK, 'r');
    v->add_piece(HORSE, 'n', 'h');
    v->add_piece(KING, 'k');
    v->add_piece(CANNON, 'c');
    v->add_piece(SOLDIER, 'p');
    v->add_piece(ELEPHANT, 'b', 'e');
    v->add_piece(FERS, 'a');

    v->startFen = "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR w - - 0 1";

    v->mobilityRegion[WHITE][KING] = (Rank1BB | Rank2BB | Rank3BB) & (FileDBB | FileEBB | FileFBB);
    v->mobilityRegion[BLACK][KING] = (Rank8BB | Rank9BB | Rank10BB) & (FileDBB | FileEBB | FileFBB);
    v->mobilityRegion[WHITE][FERS] = v->mobilityRegion[WHITE][KING];
    v->mobilityRegion[BLACK][FERS] = v->mobilityRegion[BLACK][KING];
    v->mobilityRegion[WHITE][ELEPHANT] = Rank1BB | Rank2BB | Rank3BB | Rank4BB | Rank5BB;
    v->mobilityRegion[BLACK][ELEPHANT] = Rank6BB | Rank7BB | Rank8BB | Rank9BB | Rank10BB;

    v->kingType = WAZIR;
    v->stalemateValue = -VALUE_MATE;
    v->perpetualCheckIllegal = true;
    v->flyingGeneral = true;
    v->soldierPromotionRank = RANK_6;
    v->chasingRule = AXF_CHASING;

    return v;
}

} // namespace

const Variant* xiangqi_variant() {
    static const Variant* v = make_xiangqi_variant()->conclude();
    return v;
}


/// VariantMap::init() is called at startup to initialize all predefined variants

void VariantMap::init() {
    insert(std::pair<std::string, const Variant*>("xiangqi", xiangqi_variant()));
}


// Pre-calculate derived properties
Variant* Variant::conclude() {
    nnueKing = KING;
    connectDirections.clear();
    connectPieceTypesTrimmed = NO_PIECE_SET;
    nnueMaxPieces = 32;

    std::memset(pieceSquareIndex, 0, sizeof(pieceSquareIndex));
    std::memset(kingSquareIndex, 0, sizeof(kingSquareIndex));

    constexpr int nnueSquares = 90;
    constexpr int nnuePieceCount = 7;
    constexpr int nnuePieceIndices = (2 * nnuePieceCount - 1) * nnueSquares;

    int i = 0;
    for (PieceSet ps = pieceTypes; ps;)
    {
        PieceType pt = lsb(ps != piece_set(nnueKing) ? ps & ~piece_set(nnueKing) : ps);
        ps ^= pt;
        assert(pt != nnueKing || !ps);

        for (Color c : { WHITE, BLACK})
        {
            pieceSquareIndex[c][make_piece(c, pt)] = 2 * i * nnueSquares;
            pieceSquareIndex[c][make_piece(~c, pt)] = (2 * i + (pt != nnueKing)) * nnueSquares;
        }
        i++;
    }

    int nnueKingSquare = 0;
    for (Square s = SQ_A1; s < nnueSquares; ++s)
    {
        Square bitboardSquare = Square(s + s / 9 * (FILE_MAX - FILE_I));
        if (mobilityRegion[WHITE][KING] & make_bitboard(bitboardSquare))
            kingSquareIndex[s] = nnueKingSquare++ * nnuePieceIndices;
    }
    nnueDimensions = nnueKingSquare * nnuePieceIndices;

    return this;
}


void VariantMap::add(std::string s, Variant* v) {
  insert(std::pair<std::string, const Variant*>(s, v->conclude()));
}

void VariantMap::clear_all() {
  clear();
}

std::vector<std::string> VariantMap::get_keys() {
  std::vector<std::string> keys;
  for (auto const& element : *this)
      keys.push_back(element.first);
  return keys;
}

} // namespace Stockfish

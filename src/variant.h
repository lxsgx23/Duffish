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

#ifndef VARIANT_H_INCLUDED
#define VARIANT_H_INCLUDED

#include <set>
#include <map>
#include <vector>
#include <string>
#include <functional>

#include "types.h"
#include "bitboard.h"

namespace Stockfish {

/// Variant struct stores information needed to determine the rules of a variant.

struct Variant {
  Rank maxRank = RANK_8;
  File maxFile = FILE_H;
  int pieceValue[PHASE_NB][PIECE_TYPE_NB] = {};
  PieceSet pieceTypes = CHESS_PIECES;
  std::string pieceToChar =  " PNBRQ" + std::string(KING - QUEEN - 1, ' ') + "K" + std::string(PIECE_TYPE_NB - KING - 1, ' ')
                           + " pnbrq" + std::string(KING - QUEEN - 1, ' ') + "k" + std::string(PIECE_TYPE_NB - KING - 1, ' ');
  std::string pieceToCharSynonyms = std::string(PIECE_NB, ' ');
  std::string startFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  Bitboard mobilityRegion[COLOR_NB][PIECE_TYPE_NB] = {};
  PieceSet petrifyOnCaptureTypes = NO_PIECE_SET;
  bool petrifyBlastPieces = false;
  Bitboard enPassantRegion[COLOR_NB] = {AllSquares, AllSquares};
  PieceSet enPassantTypes[COLOR_NB] = {piece_set(PAWN), piece_set(PAWN)};
  File castlingKingsideFile = FILE_G;
  File castlingQueensideFile = FILE_C;
  Rank castlingRank = RANK_1;
  File castlingKingFile = FILE_E;
  PieceType castlingKingPiece[COLOR_NB] = {KING, KING};
  File castlingRookKingsideFile = FILE_MAX; // only has to match if rook is not in corner in non-960 variants
  File castlingRookQueensideFile = FILE_A; // only has to match if rook is not in corner in non-960 variants
  PieceSet castlingRookPieces[COLOR_NB] = {piece_set(ROOK), piece_set(ROOK)};
  bool oppositeCastling = false;
  PieceType kingType = KING;
  bool checking = true;
  bool mustCapture = false;
  PieceType mustDropType = ALL_PIECES;
  WallingRule wallingRule = NO_WALLING;
  Bitboard wallingRegion[COLOR_NB] = {AllSquares, AllSquares};
  bool wallOrMove = false;
  bool flyingGeneral = false;
  Rank soldierPromotionRank = RANK_1;

  // game end
  PieceSet nMoveRuleTypes[COLOR_NB] = {piece_set(PAWN), piece_set(PAWN)};
  int nMoveRule = 60;
  int nFoldRule = 3;
  Value nFoldValue = VALUE_DRAW;
  bool nFoldValueAbsolute = false;
  bool perpetualCheckIllegal = false;
  bool moveRepetitionIllegal = false;
  ChasingRule chasingRule = NO_CHASING;
  Value stalemateValue = VALUE_DRAW;
  bool stalematePieceCount = false; // multiply stalemate value by sign(count(~stm) - count(stm))
  Value checkmateValue = -VALUE_MATE;
  bool shogiPawnDropMateIllegal = false;
  bool shatarMateRule = false;
  bool bikjangRule = false;
  Value extinctionValue = VALUE_NONE;
  bool extinctionClaim = false;
  bool extinctionPseudoRoyal = false;
  bool dupleCheck = false;
  PieceSet extinctionPieceTypes = NO_PIECE_SET;
  int extinctionPieceCount = 0;
  int extinctionOpponentPieceCount = 0;
  PieceType flagPiece[COLOR_NB] = {ALL_PIECES, ALL_PIECES};
  Bitboard flagRegion[COLOR_NB] = {};
  int flagPieceCount = 1;
  bool flagPieceBlockedWin = false;
  bool flagMove = false;
  bool flagPieceSafe = false;
  bool checkCounting = false;
  int connectN = 0;
  PieceSet connectPieceTypes = ~NO_PIECE_SET;
  bool connectHorizontal = true;
  bool connectVertical = true;
  bool connectDiagonal = true;
  Bitboard connectRegion1[COLOR_NB] = {};
  Bitboard connectRegion2[COLOR_NB] = {};
  int connectNxN = 0;
  int collinearN = 0;
  Value connectValue = VALUE_MATE;
  MaterialCounting materialCounting = NO_MATERIAL_COUNTING;
  bool adjudicateFullBoard = false;
  CountingRule countingRule = NO_COUNTING;
  CastlingRights castlingWins = NO_CASTLING;

  // Derived properties
  std::string nnueAlias = "";
  PieceType nnueKing = KING;
  int nnueDimensions;
  int pieceSquareIndex[COLOR_NB][PIECE_NB];
  int kingSquareIndex[SQUARE_NB];
  int nnueMaxPieces;
  std::vector<Direction> connectDirections;
  PieceSet connectPieceTypesTrimmed = ~NO_PIECE_SET;
  void add_piece(PieceType pt, char c, std::string betza = "", char c2 = ' ') {
      // Avoid ambiguous definition by removing existing piece with same letter
      size_t idx;
      if ((idx = pieceToChar.find(toupper(c))) != std::string::npos)
          remove_piece(PieceType(idx));
      // Now add new piece
      pieceToChar[make_piece(WHITE, pt)] = toupper(c);
      pieceToChar[make_piece(BLACK, pt)] = tolower(c);
      pieceToCharSynonyms[make_piece(WHITE, pt)] = toupper(c2);
      pieceToCharSynonyms[make_piece(BLACK, pt)] = tolower(c2);
      pieceTypes |= pt;
      (void)betza;
  }

  void add_piece(PieceType pt, char c, char c2) {
      add_piece(pt, c, "", c2);
  }

  void remove_piece(PieceType pt) {
      pieceToChar[make_piece(WHITE, pt)] = ' ';
      pieceToChar[make_piece(BLACK, pt)] = ' ';
      pieceToCharSynonyms[make_piece(WHITE, pt)] = ' ';
      pieceToCharSynonyms[make_piece(BLACK, pt)] = ' ';
      pieceTypes &= ~piece_set(pt);
  }

  void reset_pieces() {
      pieceToChar = std::string(PIECE_NB, ' ');
      pieceToCharSynonyms = std::string(PIECE_NB, ' ');
      pieceTypes = NO_PIECE_SET;
  }

  // Reset values that always need to be redefined
  Variant* init() {
      nnueAlias = "";
      return this;
  }

  Variant* conclude();
};

class VariantMap : public std::map<std::string, const Variant*> {
public:
  void init();
  void clear_all();
  std::vector<std::string> get_keys();

private:
  void add(std::string s, Variant* v);
};

extern VariantMap variants;
const Variant* xiangqi_variant();

} // namespace Stockfish

#endif // #ifndef VARIANT_H_INCLUDED

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

#ifndef POSITION_H_INCLUDED
#define POSITION_H_INCLUDED

#include <cassert>
#include <deque>
#include <memory> // For std::unique_ptr
#include <string>
#include <functional>

#include "bitboard.h"
#include "evaluate.h"
#include "psqt.h"
#include "types.h"
#include "variant.h"
#include "movegen.h"

#include "nnue/nnue_accumulator.h"

namespace Stockfish {

/// StateInfo struct stores information needed to restore a Position object to
/// its previous state when we retract a move. Whenever a move is made on the
/// board (by calling Position::do_move), a StateInfo object must be passed.

struct StateInfo {

  // Copied when making a move
  Key    pawnKey;
  Key    minorPieceKey;
  Key    nonPawnKey[COLOR_NB];
  Key    materialKey;
  Value  nonPawnMaterial[COLOR_NB];
  int    castlingRights;
  int    rule50;
  int    pliesFromNull;
  int    countingPly;
  int    countingLimit;
  CheckCount checksRemaining[COLOR_NB];
  Bitboard epSquares;
  Square castlingKingSquare[COLOR_NB];
  Bitboard wallSquares;
  Bitboard gatesBB[COLOR_NB];

  // Not copied when making a move (will be recomputed anyhow)
  Key        key;
  Bitboard   checkersBB;
  Piece      unpromotedCapturedPiece;
  Piece      unpromotedBycatch[SQUARE_NB];
  Bitboard   promotedBycatch;
  Bitboard   demotedBycatch;
  StateInfo* previous;
  Bitboard   blockersForKing[COLOR_NB];
  Bitboard   pinners[COLOR_NB];
  Bitboard   checkSquares[PIECE_TYPE_NB];
  Piece      capturedPiece;
  Square     captureSquare; // when != to_sq, e.g., en passant
  Piece      promotionPawn;
  Bitboard   nonSlidingRiders;
  Bitboard   flippedPieces;
  Bitboard   pseudoRoyalCandidates;
  Bitboard   pseudoRoyals;
  OptBool    legalCapture;
  bool       capturedpromoted;
  bool       shak;
  bool       bikjang;
  Bitboard   chased;
  bool       pass;
  Move       move;
  int        repetition;

  // Used by NNUE
  Eval::NNUE::Accumulator accumulator;
  DirtyPiece dirtyPiece;
};


/// A list to keep track of the position states along the setup moves (from the
/// start position to the position just before the search starts). Needed by
/// 'draw by repetition' detection. Use a std::deque because pointers to
/// elements are not invalidated upon list resizing.
typedef std::unique_ptr<std::deque<StateInfo>> StateListPtr;


/// Position class stores information regarding the board representation as
/// pieces, side to move, hash keys, castling info, etc. Important methods are
/// do_move() and undo_move(), used by the search to update node info when
/// traversing the search tree.
class Thread;

class Position {
public:
  static void init();

  Position() = default;
  Position(const Position&) = delete;
  Position& operator=(const Position&) = delete;

  // FEN string input/output
  Position& set(const Variant* v, const std::string& fenStr, bool isChess960, StateInfo* si, Thread* th, bool sfen = false);
  std::string fen(bool sfen = false, bool showPromoted = false, int countStarted = 0, std::string holdings = "-", Bitboard fogArea = 0) const;

  // Variant rule properties
  const Variant* variant() const;
  Rank max_rank() const;
  File max_file() const;
  int ranks() const;
  int files() const;
  bool two_boards() const;
  Bitboard board_bb() const;
  Bitboard board_bb(Color c, PieceType pt) const;
  PieceSet piece_types() const;
  const std::string& piece_to_char() const;
  const std::string& piece_to_char_synonyms() const;
  Bitboard promotion_zone(Color c) const;
  Square promotion_square(Color c, Square s) const;
  PieceType main_promotion_pawn_type(Color c) const;
  PieceSet promotion_piece_types(Color c) const;
  bool sittuyin_promotion() const;
  int promotion_limit(PieceType pt) const;
  PieceType promoted_piece_type(PieceType pt) const;
  bool piece_promotion_on_capture() const;
  bool mandatory_pawn_promotion() const;
  bool mandatory_piece_promotion() const;
  bool piece_demotion() const;
  bool blast_on_capture() const;
  PieceSet blast_immune_types() const;
  PieceSet mutually_immune_types() const;
  Bitboard double_step_region(Color c) const;
  Bitboard triple_step_region(Color c) const;
  bool castling_enabled() const;
  bool castling_dropped_piece() const;
  File castling_kingside_file() const;
  File castling_queenside_file() const;
  Rank castling_rank(Color c) const;
  File castling_king_file() const;
  PieceType castling_king_piece(Color c) const;
  PieceSet castling_rook_pieces(Color c) const;
  PieceType king_type() const;
  PieceType nnue_king() const;
  Square nnue_king_square(Color c) const;
  bool nnue_use_pockets() const;
  bool nnue_applicable() const;
  int nnue_piece_square_index(Color perspective, Piece pc) const;
  int nnue_piece_hand_index(Color perspective, Piece pc) const;
  int nnue_king_square_index(Square ksq) const;
  bool free_drops() const;
  bool fast_attacks() const;
  bool fast_attacks2() const;
  bool checking_permitted() const;
  bool drop_checks() const;
  bool must_capture() const;
  bool has_capture() const;
  bool must_drop() const;
  bool piece_drops() const;
  bool drop_loop() const;
  bool captures_to_hand() const;
  bool first_rank_pawn_drops() const;
  bool can_drop(Color c, PieceType pt) const;
  EnclosingRule enclosing_drop() const;
  Bitboard drop_region(Color c) const;
  Bitboard drop_region(Color c, PieceType pt) const;
  bool sittuyin_rook_drop() const;
  bool drop_opposite_colored_bishop() const;
  bool drop_promoted() const;
  PieceType drop_no_doubled() const;
  PieceSet promotion_pawn_types(Color c) const;
  PieceSet en_passant_types(Color c) const;
  bool immobility_illegal() const;
  bool gating() const;
  bool walling() const;
  WallingRule walling_rule() const;
  bool wall_or_move() const;
  Bitboard walling_region(Color c) const;
  bool seirawan_gating() const;
  bool cambodian_moves() const;
  Bitboard diagonal_lines() const;
  bool pass(Color c) const;
  bool pass_on_stalemate(Color c) const;
  Bitboard promoted_soldiers(Color c) const;
  bool makpong() const;
  EnclosingRule flip_enclosed_pieces() const;
  // winning conditions
  int n_move_rule() const;
  int n_fold_rule() const;
  Value stalemate_value(int ply = 0) const;
  Value checkmate_value(int ply = 0) const;
  Value extinction_value(int ply = 0) const;
  bool extinction_claim() const;
  PieceSet extinction_piece_types() const;
  bool extinction_single_piece() const;
  int extinction_piece_count() const;
  int extinction_opponent_piece_count() const;
  bool extinction_pseudo_royal() const;
  PieceType flag_piece(Color c) const;
  Bitboard flag_region(Color c) const;
  bool flag_move() const;
  bool flag_reached(Color c) const;
  bool check_counting() const;
  int connect_n() const;
  PieceSet connect_piece_types() const;
  bool connect_horizontal() const;
  bool connect_vertical() const;
  bool connect_diagonal() const;
  const std::vector<Direction>& getConnectDirections() const;
  int connect_nxn() const;
  int collinear_n() const;

  CheckCount checks_remaining(Color c) const;
  MaterialCounting material_counting() const;
  CountingRule counting_rule() const;

  // Variant-specific properties
  int count_in_hand(PieceType pt) const;
  int count_in_hand(Color c, PieceType pt) const;
  int count_with_hand(Color c, PieceType pt) const;
  bool bikjang() const;
  bool allow_virtual_drop(Color c, PieceType pt) const;

  // Position representation
  Bitboard pieces(PieceType pt = ALL_PIECES) const;
  Bitboard pieces(PieceType pt1, PieceType pt2) const;
  Bitboard pieces(Color c) const;
  Bitboard pieces(Color c, PieceType pt) const;
  Bitboard pieces(Color c, PieceType pt1, PieceType pt2) const;
  Bitboard pieces(Color c, PieceType pt1, PieceType pt2, PieceType pt3) const;
  Bitboard major_pieces(Color c) const;
  Bitboard non_sliding_riders() const;
  Piece piece_on(Square s) const;
  Piece unpromoted_piece_on(Square s) const;
  Bitboard ep_squares() const;
  Square castling_king_square(Color c) const;
  Bitboard gates(Color c) const;
  bool empty(Square s) const;
  int count(Color c, PieceType pt) const;
  template<PieceType Pt> int count(Color c) const;
  template<PieceType Pt> int count() const;
  template<PieceType Pt> Square square(Color c) const;
  Square square(Color c, PieceType pt) const;
  bool is_on_semiopen_file(Color c, Square s) const;

  // Castling
  CastlingRights castling_rights(Color c) const;
  bool can_castle(CastlingRights cr) const;
  bool castling_impeded(CastlingRights cr) const;
  Square castling_rook_square(CastlingRights cr) const;

  // Checking
  Bitboard checkers() const;
  Bitboard blockers_for_king(Color c) const;
  Bitboard check_squares(PieceType pt) const;
  Bitboard pinners(Color c) const;
  Bitboard checked_pseudo_royals(Color c) const;

  // Attacks to/from a given square
  Bitboard attackers_to(Square s) const;
  Bitboard attackers_to(Square s, Color c) const;
  Bitboard attackers_to(Square s, Bitboard occupied) const;
  Bitboard attackers_to(Square s, Bitboard occupied, Color c) const;
  Bitboard attackers_to(Square s, Bitboard occupied, Color c, Bitboard janggiCannons) const;
  Bitboard attacks_from(Color c, PieceType pt, Square s) const;
  Bitboard moves_from(Color c, PieceType pt, Square s) const;
  Bitboard slider_blockers(Bitboard sliders, Square s, Bitboard& pinners, Color c) const;

  // Properties of moves
  bool legal(Move m) const;
  bool pseudo_legal(const Move m) const;
  bool virtual_drop(Move m) const;
  bool capture(Move m) const;
  bool capture_or_promotion(Move m) const;
  Square capture_square(Square to) const;
  bool gives_check(Move m) const;
  Piece moved_piece(Move m) const;
  Piece captured_piece() const;

  // Piece specific
  bool pawn_passed(Color c, Square s) const;
  bool opposite_bishops() const;
  bool is_promoted(Square s) const;
  int  pawns_on_same_color_squares(Color c, Square s) const;

  // Doing and undoing moves
  void do_move(Move m, StateInfo& newSt);
  void do_move(Move m, StateInfo& newSt, bool givesCheck);
  void undo_move(Move m);
  void do_null_move(StateInfo& newSt);
  void undo_null_move();

  // Static Exchange Evaluation
  Value blast_see(Move m) const;
  bool see_ge(Move m, Value threshold = VALUE_ZERO) const;

  // Accessing hash keys
  Key key() const;
  Key key_after(Move m) const;
  Key material_key() const;
  Key pawn_key() const;
  Key minor_piece_key() const;
  Key non_pawn_key(Color c) const;

  // Other properties of the position
  Color side_to_move() const;
  int game_ply() const;
  bool is_chess960() const;
  Thread* this_thread() const;
  bool is_immediate_game_end() const;
  bool is_immediate_game_end(Value& result, int ply = 0) const;
  bool is_optional_game_end() const;
  bool is_optional_game_end(Value& result, int ply = 0, int countStarted = 0) const;
  bool is_game_end(Value& result, int ply = 0) const;
  Value material_counting_result() const;
  bool is_draw(int ply) const;
  bool has_game_cycle(int ply) const;
  bool has_repeated() const;
  Bitboard chased() const;
  int count_limit(Color sideToCount) const;
  int board_honor_counting_ply(int countStarted) const;
  bool board_honor_counting_shorter(int countStarted) const;
  int counting_limit(int countStarted) const;
  int counting_ply(int countStarted) const;
  int rule50_count() const;
  Score psq_score() const;
  Value non_pawn_material(Color c) const;
  Value non_pawn_material() const;
  Bitboard fog_area() const;

  // Position consistency check, for debugging
  bool pos_is_ok() const;
  void flip();

  // Used by NNUE
  StateInfo* state() const;

  void put_piece(Piece pc, Square s, bool isPromoted = false, Piece unpromotedPc = NO_PIECE);
  void remove_piece(Square s);

private:
  // Initialization helpers (used while setting up a position)
  void set_castling_right(Color c, Square rfrom);
  void set_state(StateInfo* si) const;
  void set_check_info(StateInfo* si) const;

  // Other helpers
  void move_piece(Square from, Square to);
  template<bool Do>
  void do_castling(Color us, Square from, Square& to, Square& rfrom, Square& rto);

  // Data members
  Piece board[SQUARE_NB];
  Piece unpromotedBoard[SQUARE_NB];
  Bitboard byTypeBB[PIECE_TYPE_NB];
  Bitboard byColorBB[COLOR_NB];
  int pieceCount[PIECE_NB];
  int castlingRightsMask[SQUARE_NB];
  Square castlingRookSquare[CASTLING_RIGHT_NB];
  Bitboard castlingPath[CASTLING_RIGHT_NB];
  Thread* thisThread;
  StateInfo* st;
  int gamePly;
  Color sideToMove;
  Score psq;

  // variant-specific
  const Variant* var;
  bool tsumeMode;
  bool chess960;
  int pieceCountInHand[COLOR_NB][PIECE_TYPE_NB];
  int virtualPieces;
  Bitboard promotedPieces;
  void add_to_hand(Piece pc);
  void remove_from_hand(Piece pc);
  void drop_piece(Piece pc_hand, Piece pc_drop, Square s);
  void undrop_piece(Piece pc_hand, Square s);
  Bitboard find_drop_region(Direction dir, Square s, Bitboard occupied) const;
};

extern std::ostream& operator<<(std::ostream& os, const Position& pos);

inline const Variant* Position::variant() const {
  assert(var != nullptr);
  return var;
}

inline Rank Position::max_rank() const {
  assert(var != nullptr);
  return var->maxRank;
}

inline File Position::max_file() const {
  assert(var != nullptr);
  return var->maxFile;
}

inline int Position::ranks() const {
  assert(var != nullptr);
  return var->maxRank + 1;
}

inline int Position::files() const {
  assert(var != nullptr);
  return var->maxFile + 1;
}

inline bool Position::two_boards() const {
  return false;
}

inline Bitboard Position::board_bb() const {
  assert(var != nullptr);
  return board_size_bb(var->maxFile, var->maxRank);
}

inline Bitboard Position::board_bb(Color c, PieceType pt) const {
  assert(var != nullptr);
  return var->mobilityRegion[c][pt] ? var->mobilityRegion[c][pt] & board_bb() : board_bb();
}

inline PieceSet Position::piece_types() const {
  assert(var != nullptr);
  return var->pieceTypes;
}

inline const std::string& Position::piece_to_char() const {
  assert(var != nullptr);
  return var->pieceToChar;
}

inline const std::string& Position::piece_to_char_synonyms() const {
  assert(var != nullptr);
  return var->pieceToCharSynonyms;
}

inline Bitboard Position::promotion_zone(Color c) const {
  (void)c;
  return 0;
}

inline Square Position::promotion_square(Color c, Square s) const {
  (void)c;
  (void)s;
  return SQ_NONE;
}

inline PieceType Position::main_promotion_pawn_type(Color c) const {
  (void)c;
  return SOLDIER;
}

inline PieceSet Position::promotion_piece_types(Color c) const {
  (void)c;
  return NO_PIECE_SET;
}

inline bool Position::sittuyin_promotion() const {
  return false;
}

inline int Position::promotion_limit(PieceType pt) const {
  (void)pt;
  return 0;
}

inline PieceType Position::promoted_piece_type(PieceType pt) const {
  (void)pt;
  return NO_PIECE_TYPE;
}

inline bool Position::piece_promotion_on_capture() const {
  return false;
}

inline bool Position::mandatory_pawn_promotion() const {
  return false;
}

inline bool Position::mandatory_piece_promotion() const {
  return false;
}

inline bool Position::piece_demotion() const {
  return false;
}

inline bool Position::blast_on_capture() const {
  return false;
}

inline PieceSet Position::blast_immune_types() const {
  return NO_PIECE_SET;
}

inline PieceSet Position::mutually_immune_types() const {
  return NO_PIECE_SET;
}

inline Bitboard Position::double_step_region(Color c) const {
  (void)c;
  return 0;
}

inline Bitboard Position::triple_step_region(Color c) const {
  (void)c;
  return 0;
}

inline bool Position::castling_enabled() const {
  return false;
}

inline bool Position::castling_dropped_piece() const {
  return false;
}

inline File Position::castling_kingside_file() const {
  assert(var != nullptr);
  return var->castlingKingsideFile;
}

inline File Position::castling_queenside_file() const {
  assert(var != nullptr);
  return var->castlingQueensideFile;
}

inline Rank Position::castling_rank(Color c) const {
  assert(var != nullptr);
  return relative_rank(c, var->castlingRank, max_rank());
}

inline File Position::castling_king_file() const {
  assert(var != nullptr);
  return var->castlingKingFile;
}

inline PieceType Position::castling_king_piece(Color c) const {
  assert(var != nullptr);
  return var->castlingKingPiece[c];
}

inline PieceSet Position::castling_rook_pieces(Color c) const {
  assert(var != nullptr);
  return var->castlingRookPieces[c];
}

inline PieceType Position::king_type() const {
  assert(var != nullptr);
  return var->kingType;
}

inline PieceType Position::nnue_king() const {
  assert(var != nullptr);
  return var->nnueKing;
}

inline Square Position::nnue_king_square(Color c) const {
  return nnue_king() ? square(c, nnue_king()) : SQ_NONE;
}

inline bool Position::nnue_use_pockets() const {
  return false;
}

inline bool Position::nnue_applicable() const {
  return !virtualPieces
      && (!nnue_king() || (count(WHITE, nnue_king()) == 1 && count(BLACK, nnue_king()) == 1));
}

inline int Position::nnue_piece_square_index(Color perspective, Piece pc) const {
  assert(var != nullptr);
  return var->pieceSquareIndex[perspective][pc];
}

inline int Position::nnue_piece_hand_index(Color perspective, Piece pc) const {
  (void)perspective;
  (void)pc;
  return 0;
}

inline int Position::nnue_king_square_index(Square ksq) const {
  assert(var != nullptr);
  return var->kingSquareIndex[ksq];
}

inline bool Position::checking_permitted() const {
  assert(var != nullptr);
  return var->checking;
}

inline bool Position::free_drops() const {
  return false;
}

inline bool Position::fast_attacks() const {
  return false;
}

inline bool Position::fast_attacks2() const {
  return false;
}

inline bool Position::drop_checks() const {
  return false;
}

inline bool Position::must_capture() const {
  assert(var != nullptr);
  return var->mustCapture;
}

inline bool Position::has_capture() const {
  // Check for cached value
  if (st->legalCapture != NO_VALUE)
      return st->legalCapture == VALUE_TRUE;
  if (checkers())
  {
      for (const auto& mevasion : MoveList<EVASIONS>(*this))
          if (capture(mevasion) && legal(mevasion))
          {
              st->legalCapture = VALUE_TRUE;
              return true;
          }
  }
  else
  {
      for (const auto& mcap : MoveList<CAPTURES>(*this))
          if (capture(mcap) && legal(mcap))
          {
              st->legalCapture = VALUE_TRUE;
              return true;
          }
  }
  st->legalCapture = VALUE_FALSE;
  return false;
}

inline bool Position::must_drop() const {
  return false;
}

inline bool Position::piece_drops() const {
  return false;
}

inline bool Position::drop_loop() const {
  return false;
}

inline bool Position::captures_to_hand() const {
  return false;
}

inline bool Position::first_rank_pawn_drops() const {
  return false;
}

inline EnclosingRule Position::enclosing_drop() const {
  return NO_ENCLOSING;
}

inline Bitboard Position::drop_region(Color c) const {
  (void)c;
  return 0;
}

inline Bitboard Position::drop_region(Color c, PieceType pt) const {
  (void)c;
  (void)pt;
  return 0;
}

inline bool Position::sittuyin_rook_drop() const {
  return false;
}

inline bool Position::drop_opposite_colored_bishop() const {
  return false;
}

inline bool Position::drop_promoted() const {
  return false;
}

inline PieceType Position::drop_no_doubled() const {
  return NO_PIECE_TYPE;
}

inline PieceSet Position::promotion_pawn_types(Color c) const {
  (void)c;
  return NO_PIECE_SET;
}

inline PieceSet Position::en_passant_types(Color c) const {
  assert(var != nullptr);
  return var->enPassantTypes[c];
}

inline bool Position::immobility_illegal() const {
  return false;
}

inline bool Position::gating() const {
  return false;
}

inline bool Position::walling() const {
  return false;
}

inline WallingRule Position::walling_rule() const {
  return NO_WALLING;
}

inline bool Position::wall_or_move() const {
  return false;
}

inline Bitboard Position::walling_region(Color c) const {
  (void)c;
  return 0;
}

inline bool Position::seirawan_gating() const {
  return false;
}

inline bool Position::cambodian_moves() const {
  return false;
}

inline Bitboard Position::diagonal_lines() const {
  return 0;
}

inline bool Position::pass(Color c) const {
  (void)c;
  return false;
}

inline bool Position::pass_on_stalemate(Color c) const {
  (void)c;
  return false;
}

inline Bitboard Position::promoted_soldiers(Color c) const {
  assert(var != nullptr);
  return pieces(c, SOLDIER) & zone_bb(c, var->soldierPromotionRank, max_rank());
}

inline bool Position::makpong() const {
  return false;
}

inline int Position::n_move_rule() const {
  assert(var != nullptr);
  return var->nMoveRule;
}

inline int Position::n_fold_rule() const {
  assert(var != nullptr);
  return var->nFoldRule;
}

inline EnclosingRule Position::flip_enclosed_pieces() const {
  return NO_ENCLOSING;
}

inline Value Position::stalemate_value(int ply) const {
  assert(var != nullptr);
  // Check for checkmate of pseudo-royal pieces
  if (var->extinctionPseudoRoyal)
  {
      Bitboard pseudoRoyals = st->pseudoRoyals & pieces(sideToMove);
      Bitboard pseudoRoyalsTheirs = st->pseudoRoyals & pieces(~sideToMove);
      while (pseudoRoyals)
      {
          Square sr = pop_lsb(pseudoRoyals);
          if (  !(blast_on_capture() && (pseudoRoyalsTheirs & attacks_bb<KING>(sr)))
              && attackers_to(sr, ~sideToMove))
              return convert_mate_value(var->checkmateValue, ply);
      }
      // Look for duple check
      if (var->dupleCheck)
      {
          Bitboard pseudoRoyalCandidates = st->pseudoRoyalCandidates & pieces(sideToMove);
          bool allCheck = bool(pseudoRoyalCandidates);
          while (allCheck && pseudoRoyalCandidates)
          {
              Square sr = pop_lsb(pseudoRoyalCandidates);
              // Touching pseudo-royal pieces are immune
              if (!(  !(blast_on_capture() && (pseudoRoyalsTheirs & attacks_bb<KING>(sr)))
                    && attackers_to(sr, ~sideToMove)))
                  allCheck = false;
          }
          if (allCheck)
              return convert_mate_value(var->checkmateValue, ply);
      }
  }
  Value result = var->stalemateValue;
  // Is piece count used to determine stalemate result?
  if (var->stalematePieceCount)
  {
      int c = count<ALL_PIECES>(sideToMove) - count<ALL_PIECES>(~sideToMove);
      result = c == 0 ? VALUE_DRAW : c < 0 ? var->stalemateValue : -var->stalemateValue;
  }
  // Apply material counting
  if (result == VALUE_DRAW && var->materialCounting)
      result = material_counting_result();
  return convert_mate_value(result, ply);
}

inline Value Position::checkmate_value(int ply) const {
  assert(var != nullptr);
  // Check for illegal mate by shogi pawn drop
  if (    var->shogiPawnDropMateIllegal
      && !(checkers() & ~pieces(SHOGI_PAWN))
      && !st->capturedPiece
      &&  st->pliesFromNull > 0
      && (st->materialKey != st->previous->materialKey))
  {
      return mate_in(ply);
  }
  // Check for shatar mate rule
  if (var->shatarMateRule)
  {
      // Mate by knight is illegal
      if (!(checkers() & ~pieces(KNIGHT)))
          return mate_in(ply);

      StateInfo* stp = st;
      while (stp->checkersBB)
      {
          // Return mate score if there is at least one shak in series of checks
          if (stp->shak)
              return convert_mate_value(var->checkmateValue, ply);

          if (stp->pliesFromNull < 2)
              break;

          stp = stp->previous->previous;
      }
      // Niol
      return VALUE_DRAW;
  }
  // Checkmate using virtual pieces
  if (two_boards() && var->checkmateValue < VALUE_ZERO)
  {
      Value virtualMaterial = VALUE_ZERO;
      for (PieceSet ps = piece_types(); ps;)
      {
          PieceType pt = pop_lsb(ps);
          virtualMaterial += std::max(-count_in_hand(~sideToMove, pt), 0) * PieceValue[MG][pt];
      }

      if (virtualMaterial > 0)
          return -VALUE_VIRTUAL_MATE + virtualMaterial / 20 + ply;
  }
  // Return mate value
  return convert_mate_value(var->checkmateValue, ply);
}

inline Value Position::extinction_value(int ply) const {
  assert(var != nullptr);
  return convert_mate_value(var->extinctionValue, ply);
}

inline bool Position::extinction_claim() const {
  assert(var != nullptr);
  return var->extinctionClaim;
}

inline PieceSet Position::extinction_piece_types() const {
  assert(var != nullptr);
  return var->extinctionPieceTypes;
}

inline bool Position::extinction_single_piece() const {
  assert(var != nullptr);
  return   var->extinctionValue == -VALUE_MATE
        && (var->extinctionPieceTypes & ~piece_set(ALL_PIECES));
}

inline int Position::extinction_piece_count() const {
  assert(var != nullptr);
  return var->extinctionPieceCount;
}

inline int Position::extinction_opponent_piece_count() const {
  assert(var != nullptr);
  return var->extinctionOpponentPieceCount;
}

inline bool Position::extinction_pseudo_royal() const {
  assert(var != nullptr);
  return var->extinctionPseudoRoyal;
}

inline PieceType Position::flag_piece(Color c) const {
  assert(var != nullptr);
  return var->flagPiece[c];
}

inline Bitboard Position::flag_region(Color c) const {
  assert(var != nullptr);
  return var->flagRegion[c];
}

inline bool Position::flag_move() const {
  assert(var != nullptr);
  return var->flagMove;
}

inline bool Position::flag_reached(Color c) const {
  assert(var != nullptr);
  bool simpleResult = 
        (flag_region(c) & pieces(c, flag_piece(c)))
        && (   popcount(flag_region(c) & pieces(c, flag_piece(c))) >= var->flagPieceCount
            || (var->flagPieceBlockedWin && !(flag_region(c) & ~pieces())));
      
  if (simpleResult&&var->flagPieceSafe)
  {
      Bitboard piecesInFlagZone = flag_region(c) & pieces(c, flag_piece(c));
      int potentialPieces = (popcount(piecesInFlagZone));
      /*
      There isn't a variant that uses it, but in the hypothetical game where the rules say I need 3
      pieces in the flag zone and they need to be safe: If I have 3 pieces there, but one is under
      threat, I don't think I can declare victory. If I have 4 there, but one is under threat, I
      think that's victory.
      */      
      while (piecesInFlagZone)
      {
          Square sr = pop_lsb(piecesInFlagZone);
          Bitboard flagAttackers = attackers_to(sr, ~c);

          if ((potentialPieces < var->flagPieceCount) || (potentialPieces >= var->flagPieceCount + 1)) break;
          while (flagAttackers)
          {
              Square currentAttack = pop_lsb(flagAttackers);
              if (legal(make_move(currentAttack, sr)))
              {
                  potentialPieces--;
                  break;
              }
          }
      }
      return potentialPieces >= var->flagPieceCount;
  }
  return simpleResult;
}

inline bool Position::check_counting() const {
  assert(var != nullptr);
  return var->checkCounting;
}

inline int Position::connect_n() const {
  assert(var != nullptr);
  return var->connectN;
}

inline PieceSet Position::connect_piece_types() const {
  assert(var != nullptr);
  return var->connectPieceTypesTrimmed;
}

inline bool Position::connect_horizontal() const {
  assert(var != nullptr);
  return var->connectHorizontal;
}
inline bool Position::connect_vertical() const {
  assert(var != nullptr);
  return var->connectVertical;
}
inline bool Position::connect_diagonal() const {
  assert(var != nullptr);
  return var->connectDiagonal;
}

inline const std::vector<Direction>& Position::getConnectDirections() const {
    assert(var != nullptr);
    return var->connectDirections;
}

inline int Position::connect_nxn() const {
  assert(var != nullptr);
  return var->connectNxN;
}

inline int Position::collinear_n() const {
  assert(var != nullptr);
  return var->collinearN;
}

inline CheckCount Position::checks_remaining(Color c) const {
  return st->checksRemaining[c];
}

inline MaterialCounting Position::material_counting() const {
  assert(var != nullptr);
  return var->materialCounting;
}

inline CountingRule Position::counting_rule() const {
  assert(var != nullptr);
  return var->countingRule;
}

inline bool Position::is_immediate_game_end() const {
  Value result;
  return is_immediate_game_end(result);
}

inline bool Position::is_optional_game_end() const {
  Value result;
  return is_optional_game_end(result);
}

inline bool Position::is_draw(int ply) const {
  Value result;
  return is_optional_game_end(result, ply);
}

inline bool Position::is_game_end(Value& result, int ply) const {
  return is_immediate_game_end(result, ply) || is_optional_game_end(result, ply);
}

inline Color Position::side_to_move() const {
  return sideToMove;
}

inline Piece Position::piece_on(Square s) const {
  assert(is_ok(s));
  return board[s];
}

inline bool Position::empty(Square s) const {
  return piece_on(s) == NO_PIECE;
}

inline Piece Position::unpromoted_piece_on(Square s) const {
  return unpromotedBoard[s];
}

inline Piece Position::moved_piece(Move m) const {
  return piece_on(from_sq(m));
}

inline Bitboard Position::pieces(PieceType pt) const {
  return byTypeBB[pt];
}

inline Bitboard Position::pieces(PieceType pt1, PieceType pt2) const {
  return pieces(pt1) | pieces(pt2);
}

inline Bitboard Position::pieces(Color c) const {
  return byColorBB[c];
}

inline Bitboard Position::pieces(Color c, PieceType pt) const {
  return pieces(c) & pieces(pt);
}

inline Bitboard Position::pieces(Color c, PieceType pt1, PieceType pt2) const {
  return pieces(c) & (pieces(pt1) | pieces(pt2));
}

inline Bitboard Position::pieces(Color c, PieceType pt1, PieceType pt2, PieceType pt3) const {
  return pieces(c) & (pieces(pt1) | pieces(pt2) | pieces(pt3));
}

inline Bitboard Position::major_pieces(Color c) const {
  return pieces(c) & (pieces(QUEEN) | pieces(AIWOK) | pieces(ARCHBISHOP) | pieces(CHANCELLOR) | pieces(AMAZON));
}

inline Bitboard Position::non_sliding_riders() const {
  return st->nonSlidingRiders;
}

inline int Position::count(Color c, PieceType pt) const {
  return pieceCount[make_piece(c, pt)];
}

template<PieceType Pt> inline int Position::count(Color c) const {
  return pieceCount[make_piece(c, Pt)];
}

template<PieceType Pt> inline int Position::count() const {
  return count<Pt>(WHITE) + count<Pt>(BLACK);
}

template<PieceType Pt> inline Square Position::square(Color c) const {
  assert(count<Pt>(c) == 1);
  return lsb(pieces(c, Pt));
}

inline Square Position::square(Color c, PieceType pt) const {
  assert(count(c, pt) == 1);
  return lsb(pieces(c, pt));
}

inline Bitboard Position::ep_squares() const {
  return st->epSquares;
}

inline Square Position::castling_king_square(Color c) const {
  return st->castlingKingSquare[c];
}

inline Bitboard Position::gates(Color c) const {
  (void)c;
  return 0;
}

inline bool Position::is_on_semiopen_file(Color c, Square s) const {
  return !((pieces(c, PAWN) | pieces(c, SHOGI_PAWN, SOLDIER)) & file_bb(s));
}

inline bool Position::can_castle(CastlingRights cr) const {
  (void)cr;
  return false;
}

inline CastlingRights Position::castling_rights(Color c) const {
  (void)c;
  return NO_CASTLING;
}

inline bool Position::castling_impeded(CastlingRights cr) const {
  assert(cr == WHITE_OO || cr == WHITE_OOO || cr == BLACK_OO || cr == BLACK_OOO);

  return pieces() & castlingPath[cr];
}

inline Square Position::castling_rook_square(CastlingRights cr) const {
  assert(cr == WHITE_OO || cr == WHITE_OOO || cr == BLACK_OO || cr == BLACK_OOO);

  return castlingRookSquare[cr];
}

inline Bitboard Position::attacks_from(Color c, PieceType pt, Square s) const {
  if (fast_attacks() || fast_attacks2())
      return attacks_bb(c, pt, s, byTypeBB[ALL_PIECES]) & board_bb();

  PieceType movePt = pt == KING ? king_type() : pt;
  Bitboard b = attacks_bb(c, movePt, s, byTypeBB[ALL_PIECES]);
  // Xiangqi soldier
  if (pt == SOLDIER && !(promoted_soldiers(c) & s))
      b &= file_bb(file_of(s));
  // Janggi cannon restrictions
  if (pt == JANGGI_CANNON)
  {
      b &= ~pieces(pt);
      b &= attacks_bb(c, pt, s, pieces() ^ pieces(pt));
  }
  // Janggi palace moves
  if (diagonal_lines() & s)
  {
      PieceType diagType = movePt == WAZIR ? FERS : movePt == SOLDIER ? PAWN : movePt == ROOK ? BISHOP : NO_PIECE_TYPE;
      if (diagType)
          b |= attacks_bb(c, diagType, s, pieces()) & diagonal_lines();
      else if (movePt == JANGGI_CANNON)
          b |=  rider_attacks_bb<RIDER_CANNON_DIAG>(s, pieces())
              & rider_attacks_bb<RIDER_CANNON_DIAG>(s, pieces() ^ pieces(pt))
              & ~pieces(pt)
              & diagonal_lines();
  }
  return b & board_bb(c, pt);
}

inline Bitboard Position::moves_from(Color c, PieceType pt, Square s) const {
  if (fast_attacks() || fast_attacks2())
      return moves_bb(c, pt, s, byTypeBB[ALL_PIECES]) & board_bb();

  PieceType movePt = pt == KING ? king_type() : pt;
  Bitboard b = moves_bb(c, movePt, s, byTypeBB[ALL_PIECES]);
  // Add initial moves
  if (double_step_region(c) & s)
      b |= moves_bb<true>(c, movePt, s, byTypeBB[ALL_PIECES]);
  // Xiangqi soldier
  if (pt == SOLDIER && !(promoted_soldiers(c) & s))
      b &= file_bb(file_of(s));
  // Janggi cannon restrictions
  if (pt == JANGGI_CANNON)
  {
      b &= ~pieces(pt);
      b &= attacks_bb(c, pt, s, pieces() ^ pieces(pt));
  }
  // Janggi palace moves
  if (diagonal_lines() & s)
  {
      PieceType diagType = movePt == WAZIR ? FERS : movePt == SOLDIER ? PAWN : movePt == ROOK ? BISHOP : NO_PIECE_TYPE;
      if (diagType)
          b |= attacks_bb(c, diagType, s, pieces()) & diagonal_lines();
      else if (movePt == JANGGI_CANNON)
          b |=  rider_attacks_bb<RIDER_CANNON_DIAG>(s, pieces())
              & rider_attacks_bb<RIDER_CANNON_DIAG>(s, pieces() ^ pieces(pt))
              & ~pieces(pt)
              & diagonal_lines();
  }
  return b & board_bb(c, pt);
}

inline Bitboard Position::attackers_to(Square s) const {
  return attackers_to(s, pieces());
}

inline Bitboard Position::attackers_to(Square s, Color c) const {
  return attackers_to(s, byTypeBB[ALL_PIECES], c);
}

inline Bitboard Position::attackers_to(Square s, Bitboard occupied, Color c) const {
  return attackers_to(s, occupied, c, byTypeBB[JANGGI_CANNON]);
}

inline Bitboard Position::checkers() const {
  return st->checkersBB;
}

inline Bitboard Position::blockers_for_king(Color c) const {
  return st->blockersForKing[c];
}

inline Bitboard Position::pinners(Color c) const {
  return st->pinners[c];
}

inline Bitboard Position::check_squares(PieceType pt) const {
  return st->checkSquares[pt];
}

inline bool Position::pawn_passed(Color c, Square s) const {
  return !(pieces(~c, PAWN) & passed_pawn_span(c, s));
}

inline int Position::pawns_on_same_color_squares(Color c, Square s) const {
  return popcount(pieces(c, PAWN) & ((DarkSquares & s) ? DarkSquares : ~DarkSquares));
}

inline Key Position::key() const {
  return st->rule50 < 14 ? st->key
                         : st->key ^ make_key((st->rule50 - 14) / 8);
}

inline Key Position::pawn_key() const {
  return st->pawnKey;
}

inline Key Position::minor_piece_key() const {
  return st->minorPieceKey;
}

inline Key Position::non_pawn_key(Color c) const {
  return st->nonPawnKey[c];
}

inline Score Position::psq_score() const {
  return psq;
}

inline Value Position::non_pawn_material(Color c) const {
  return st->nonPawnMaterial[c];
}

inline Value Position::non_pawn_material() const {
  return non_pawn_material(WHITE) + non_pawn_material(BLACK);
}

inline int Position::game_ply() const {
  return gamePly;
}

inline int Position::board_honor_counting_ply(int countStarted) const {
  return countStarted == 0 ?
      st->countingPly :
      countStarted < 0 ? 0 : std::max(1 + gamePly - countStarted, 0);
}

inline bool Position::board_honor_counting_shorter(int countStarted) const {
  return counting_rule() == CAMBODIAN_COUNTING && 126 - board_honor_counting_ply(countStarted) < st->countingLimit - st->countingPly;
}

inline int Position::counting_limit(int countStarted) const {
  return board_honor_counting_shorter(countStarted) ? 126 : st->countingLimit;
}

inline int Position::counting_ply(int countStarted) const {
  return !count<PAWN>() && (count<ALL_PIECES>(WHITE) <= 1 || count<ALL_PIECES>(BLACK) <= 1) && !board_honor_counting_shorter(countStarted) ?
      st->countingPly :
      board_honor_counting_ply(countStarted);
}

inline int Position::rule50_count() const {
  return st->rule50;
}

inline bool Position::opposite_bishops() const {
  return   count<BISHOP>(WHITE) == 1
        && count<BISHOP>(BLACK) == 1
        && opposite_colors(square<BISHOP>(WHITE), square<BISHOP>(BLACK));
}

inline bool Position::is_promoted(Square s) const {
  return promotedPieces & s;
}

inline bool Position::is_chess960() const {
  return false;
}

inline bool Position::capture_or_promotion(Move m) const {
  assert(is_ok(m));
  return type_of(m) == PROMOTION || type_of(m) == EN_PASSANT || !empty(to_sq(m));
}

inline bool Position::capture(Move m) const {
  assert(is_ok(m));
  return (!empty(to_sq(m)) && from_sq(m) != to_sq(m)) || type_of(m) == EN_PASSANT;
}

inline Square Position::capture_square(Square to) const {
  assert(is_ok(to));
  // The capture square of en passant is either the marked ep piece or the closest piece behind the target square
  Bitboard customEp = ep_squares() & pieces();
  if (customEp)
  {
      // For longer custom en passant paths, we take the frontmost piece
      return sideToMove == WHITE ? lsb(customEp) : msb(customEp);
  }
  else
  {
      // The capture square of normal en passant is the closest piece behind the target square
      Bitboard epCandidates = pieces(~sideToMove) & forward_file_bb(~sideToMove, to);
      return sideToMove == WHITE ? msb(epCandidates) : lsb(epCandidates);
  }
}

inline bool Position::virtual_drop(Move m) const {
  assert(is_ok(m));
  (void)m;
  return false;
}

inline Piece Position::captured_piece() const {
  return st->capturedPiece;
}

inline Bitboard Position::fog_area() const {
  Bitboard b = board_bb();
  // Our own pieces are visible
  Bitboard visible = pieces(sideToMove);
  // Squares where we can move to are visible as well
  for (const auto& m : MoveList<LEGAL>(*this))
  {
    Square to = to_sq(m);
    visible |= to;
  }
  // Everything else is invisible
  return ~visible & b;
}

inline Thread* Position::this_thread() const {
  return thisThread;
}

inline void Position::put_piece(Piece pc, Square s, bool isPromoted, Piece unpromotedPc) {

  board[s] = pc;
  byTypeBB[ALL_PIECES] |= byTypeBB[type_of(pc)] |= s;
  byColorBB[color_of(pc)] |= s;
  pieceCount[pc]++;
  pieceCount[make_piece(color_of(pc), ALL_PIECES)]++;
  psq += PSQT::psq[pc][s];
  if (isPromoted)
      promotedPieces |= s;
  unpromotedBoard[s] = unpromotedPc;
}

inline void Position::remove_piece(Square s) {

  Piece pc = board[s];
  byTypeBB[ALL_PIECES] ^= s;
  byTypeBB[type_of(pc)] ^= s;
  byColorBB[color_of(pc)] ^= s;
  board[s] = NO_PIECE;
  pieceCount[pc]--;
  pieceCount[make_piece(color_of(pc), ALL_PIECES)]--;
  psq -= PSQT::psq[pc][s];
  promotedPieces -= s;
  unpromotedBoard[s] = NO_PIECE;
}

inline void Position::move_piece(Square from, Square to) {

  Piece pc = board[from];
  Bitboard fromTo = square_bb(from) ^ to; // from == to needs to cancel out
  byTypeBB[ALL_PIECES] ^= fromTo;
  byTypeBB[type_of(pc)] ^= fromTo;
  byColorBB[color_of(pc)] ^= fromTo;
  board[from] = NO_PIECE;
  board[to] = pc;
  psq += PSQT::psq[pc][to] - PSQT::psq[pc][from];
  if (is_promoted(from))
      promotedPieces ^= fromTo;
  unpromotedBoard[to] = unpromotedBoard[from];
  unpromotedBoard[from] = NO_PIECE;
}

inline void Position::do_move(Move m, StateInfo& newSt) {
  do_move(m, newSt, gives_check(m));
}

inline StateInfo* Position::state() const {

  return st;
}

// Variant-specific

inline int Position::count_in_hand(PieceType pt) const {
  (void)pt;
  return 0;
}

inline int Position::count_in_hand(Color c, PieceType pt) const {
  (void)c;
  (void)pt;
  return 0;
}

inline int Position::count_with_hand(Color c, PieceType pt) const {
  return pieceCount[make_piece(c, pt)];
}

inline bool Position::bikjang() const {
  return st->bikjang;
}

inline bool Position::allow_virtual_drop(Color c, PieceType pt) const {
  (void)c;
  (void)pt;
  return false;
}

inline Value Position::material_counting_result() const {
  auto weight_count = [this](PieceType pt, int v){ return v * (count(WHITE, pt) - count(BLACK, pt)); };
  int materialCount;
  Value result;
  switch (var->materialCounting)
  {
  case JANGGI_MATERIAL:
      materialCount =  weight_count(ROOK, 13)
                     + weight_count(JANGGI_CANNON, 7)
                     + weight_count(HORSE, 5)
                     + weight_count(JANGGI_ELEPHANT, 3)
                     + weight_count(WAZIR, 3)
                     + weight_count(SOLDIER, 2)
                     - 1;
      result = materialCount > 0 ? VALUE_MATE : -VALUE_MATE;
      break;
  case UNWEIGHTED_MATERIAL:
      result =  count(WHITE, ALL_PIECES) > count(BLACK, ALL_PIECES) ?  VALUE_MATE
              : count(WHITE, ALL_PIECES) < count(BLACK, ALL_PIECES) ? -VALUE_MATE
                                                                    :  VALUE_DRAW;
      break;
  case WHITE_DRAW_ODDS:
      result = VALUE_MATE;
      break;
  case BLACK_DRAW_ODDS:
      result = -VALUE_MATE;
      break;
  default:
      assert(false);
      result = VALUE_DRAW;
  }
  return sideToMove == WHITE ? result : -result;
}

inline void Position::add_to_hand(Piece pc) {
  if (free_drops()) return;
  pieceCountInHand[color_of(pc)][type_of(pc)]++;
  pieceCountInHand[color_of(pc)][ALL_PIECES]++;
  psq += PSQT::psq[pc][SQ_NONE];
}

inline void Position::remove_from_hand(Piece pc) {
  if (free_drops()) return;
  pieceCountInHand[color_of(pc)][type_of(pc)]--;
  pieceCountInHand[color_of(pc)][ALL_PIECES]--;
  psq -= PSQT::psq[pc][SQ_NONE];
}

inline void Position::drop_piece(Piece pc_hand, Piece pc_drop, Square s) {
  assert(can_drop(color_of(pc_hand), type_of(pc_hand)));
  put_piece(pc_drop, s, pc_drop != pc_hand, pc_drop != pc_hand ? pc_hand : NO_PIECE);
  remove_from_hand(pc_hand);
  virtualPieces += (pieceCountInHand[color_of(pc_hand)][type_of(pc_hand)] < 0);
}

inline void Position::undrop_piece(Piece pc_hand, Square s) {
  virtualPieces -= (pieceCountInHand[color_of(pc_hand)][type_of(pc_hand)] < 0);
  remove_piece(s);
  board[s] = NO_PIECE;
  add_to_hand(pc_hand);
  assert(can_drop(color_of(pc_hand), type_of(pc_hand)));
}

inline bool Position::can_drop(Color c, PieceType pt) const {
  (void)c;
  (void)pt;
  return false;
}

} // namespace Stockfish

#endif // #ifndef POSITION_H_INCLUDED

/*
  ffish.js, a JavaScript chess variant library derived from Fairy-Stockfish
  Copyright (C) 2022 Fabian Fichter, Johannes Czech

  ffish.js is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ffish.js is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <emscripten.h>
#include <emscripten/bind.h>
#include <vector>
#include <string>
#include <sstream>
#include <stdexcept>
#include<iostream>

#include "misc.h"
#include "types.h"
#include "bitboard.h"
#include "evaluate.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "piece.h"
#include "variant.h"
#include "movegen.h"
#include "apiutil.h"

using namespace emscripten;

using namespace Stockfish;

void initialize_stockfish() {
  pieceMap.init();
  variants.init();
  UCI::init(Options);
  Bitboards::init();
  Position::init();
}

#define DELIM " "

inline void save_pop_back(std::string& s) {
  if (s.size() != 0) {
    s.pop_back();
  }
}

const Variant* get_variant(const std::string& uciVariant) {
  std::string variant = (uciVariant.size() == 0 || uciVariant == "Standard" || uciVariant == "standard")
                      ? "xiangqi"
                      : uciVariant;
  if (variant != "xiangqi")
    throw std::invalid_argument("Unsupported variant '" + variant + "'. Duffish supports only xiangqi.");
  return xiangqi_variant();
}

inline bool is_move_none(Move move, const std::string& strMove, const Position& pos) {
  if (move == MOVE_NONE) {
    std::cerr << "The given uciMove '" << strMove
              << "' for position '" << pos.fen() << "' is invalid." << std::endl;
    return true;
  }
  return false;
}

class Board {
  // note: we can't use references for strings here due to conversion to JavaScript
private:
  const Variant* v;
  StateListPtr states;
  Position pos;
  Thread* thread;
  std::vector<Move> moveStack;

public:
  static bool sfInitialized;

  Board():
    Board("xiangqi", "") {
  }

  Board(std::string uciVariant):
    Board(uciVariant, "") {
  }

  Board(std::string uciVariant, std::string fen) {
    init(uciVariant, fen);
  }

  std::string legal_moves() {
    std::string moves;
    for (const ExtMove& move : MoveList<LEGAL>(this->pos)) {
      moves += UCI::move(this->pos, move);
      moves += DELIM;
    }
    save_pop_back(moves);
    return moves;
  }

  int number_legal_moves() const {
    return MoveList<LEGAL>(pos).size();
  }

  bool push(std::string uciMove) {
    const Move move = UCI::to_move(this->pos, uciMove);
    if (is_move_none(move, uciMove, pos))
      return false;
    do_move(move);
    return true;
  }

  void pop() {
    pos.undo_move(this->moveStack.back());
    moveStack.pop_back();
    states->pop_back();
  }

  void reset() {
    set_fen(v->startFen);
  }

  std::string fen() const {
    return this->pos.fen();
  }

  std::string fen(bool showPromoted) const {
    return this->pos.fen(false, showPromoted);
  }

  std::string fen(bool showPromoted, int countStarted) const {
    return this->pos.fen(false, showPromoted, countStarted);
  }

  void set_fen(std::string fen) {
    resetStates();
    moveStack.clear();
    pos.set(v, fen, false, &states->back(), thread);
  }

  // returns true for WHITE and false for BLACK
  bool turn() const {
    return !pos.side_to_move();
  }

  int fullmove_number() const {
    return pos.game_ply() / 2 + 1;
  }

  int halfmove_clock() const {
    return pos.rule50_count();
  }

  int game_ply() const {
    return pos.game_ply();
  }

  bool has_insufficient_material(bool turn) const {
    return Stockfish::has_insufficient_material(turn ? WHITE : BLACK, pos);
  }

  bool is_insufficient_material() const {
    return Stockfish::has_insufficient_material(WHITE, pos) && Stockfish::has_insufficient_material(BLACK, pos);
  }

  bool is_game_over() const {
    return is_game_over(false);
  }

  bool is_game_over(bool claim_draw) const {
    if (is_insufficient_material())
      return true;
    if (claim_draw && pos.is_optional_game_end())
      return true;
    return MoveList<LEGAL>(pos).size() == 0;
  }

  std::string result() const {
    return result(false);
  }

  std::string result(bool claim_draw) const {
    Value result;
    bool gameEnd = pos.is_immediate_game_end(result);
    if (!gameEnd) {
      if (is_insufficient_material()) {
        gameEnd = true;
        result = VALUE_DRAW;
      }
    }
    if (!gameEnd && MoveList<LEGAL>(pos).size() == 0) {
      gameEnd = true;
      result = pos.checkers() ? pos.checkmate_value() : pos.stalemate_value();
    }
    if (!gameEnd && claim_draw)
      gameEnd = pos.is_optional_game_end(result);

    if (!gameEnd)
      return "*";
    if (result == 0)
      return "1/2-1/2";
    if (pos.side_to_move() == BLACK)
      result = -result;
    if (result > 0)
      return "1-0";
    else
      return "0-1";
  }

  std::string checked_pieces() const {
    Bitboard checked = Stockfish::checked(pos);
    std::string squares;
    while (checked) {
      Square sr = pop_lsb(checked);
      squares += UCI::square(pos, sr);
      squares += DELIM;
    }
    save_pop_back(squares);
    return squares;
  }

  bool is_check() const {
    return Stockfish::checked(pos);
  }

  bool is_bikjang() const {
    return pos.bikjang();
  }

  bool is_capture(std::string uciMove) const {
    return pos.capture(UCI::to_move(pos, uciMove));
  }

  std::string move_stack() const {
    std::string moves;
    for(auto it = std::begin(moveStack); it != std::end(moveStack); ++it) {
      moves += UCI::move(pos, *it);
      moves += DELIM;
    }
    save_pop_back(moves);
    return moves;
  }

  void push_moves(std::string uciMoves) {
    std::stringstream ss(uciMoves);
    std::string uciMove;
    while (std::getline(ss, uciMove, ' ')) {
      push(uciMove);
    }
  }

  std::string to_string() {
    std::string stringBoard;
    for (Rank r = pos.max_rank(); r >= RANK_1; --r) {
      for (File f = FILE_A; f <= pos.max_file(); ++f) {
        if (f != FILE_A)
          stringBoard += " ";
        const Piece p = pos.piece_on(make_square(f, r));
        switch(p) {
        case NO_PIECE:
          stringBoard += '.';
          break;
        default:
          stringBoard += pos.piece_to_char()[p];
        }
      }
      if (r != RANK_1)
        stringBoard += "\n";
    }
    return stringBoard;
  }

  std::string to_verbose_string() {
    std::stringstream ss;
    operator<<(ss, pos);
    return ss.str();
  }

  std::string variant() {
    return "xiangqi";
  }

private:
  void resetStates() {
    this->states = StateListPtr(new std::deque<StateInfo>(1));
  }

  void do_move(Move move) {
    states->emplace_back();
    this->pos.do_move(move, states->back());
    this->moveStack.emplace_back(move);
  }

  void init(std::string uciVariant, std::string fen) {
    if (!Board::sfInitialized) {
      initialize_stockfish();
      Board::sfInitialized = true;
    }
    v = get_variant(uciVariant);
    UCI::init_variant(v);
    this->resetStates();
    if (fen == "")
      fen = v->startFen;
    this->pos.set(this->v, fen, false, &this->states->back(), this->thread);
  }
};

bool Board::sfInitialized = false;

namespace ffish {
  // returns the version of the Fairy-Stockfish binary
  std::string info() {
    return engine_info();
  }

  template <typename T>
  void set_option(std::string name, T value) {
    if (!Options.count(name))
      throw std::invalid_argument("No such option '" + name + "'.");
    Options[name] = value;
    Board::sfInitialized = false;
  }

  std::string available_variants() {
    std::string availableVariants;
    for (std::string variant : variants.get_keys()) {
      availableVariants += variant;
      availableVariants += DELIM;
    }
    save_pop_back(availableVariants);
    return availableVariants;
  }

  std::string starting_fen(std::string uciVariant) {
    const Variant* v = get_variant(uciVariant);
    return v->startFen;
  }

  int validate_fen(std::string fen, std::string uciVariant) {
    const Variant* v = get_variant(uciVariant);
    return FEN::validate_fen(fen, v, false);
  }

  int validate_fen(std::string fen) {
    return validate_fen(fen, "xiangqi");
  }
}

// binding code
EMSCRIPTEN_BINDINGS(ffish_js) {
  class_<Board>("Board")
    .constructor<>()
    .constructor<std::string>()
    .constructor<std::string, std::string>()
    .function("legalMoves", &Board::legal_moves)
    .function("numberLegalMoves", &Board::number_legal_moves)
    .function("push", &Board::push)
    .function("pop", &Board::pop)
    .function("reset", &Board::reset)
    .function("fen", select_overload<std::string()const>(&Board::fen))
    .function("fen", select_overload<std::string(bool)const>(&Board::fen))
    .function("fen", select_overload<std::string(bool, int)const>(&Board::fen))
    .function("setFen", &Board::set_fen)
    .function("turn", &Board::turn)
    .function("fullmoveNumber", &Board::fullmove_number)
    .function("halfmoveClock", &Board::halfmove_clock)
    .function("gamePly", &Board::game_ply)
    .function("hasInsufficientMaterial", &Board::has_insufficient_material)
    .function("isInsufficientMaterial", &Board::is_insufficient_material)
    .function("isGameOver", select_overload<bool() const>(&Board::is_game_over))
    .function("isGameOver", select_overload<bool(bool) const>(&Board::is_game_over))
    .function("result", select_overload<std::string() const>(&Board::result))
    .function("result", select_overload<std::string(bool) const>(&Board::result))
    .function("checkedPieces", &Board::checked_pieces)
    .function("isCheck", &Board::is_check)
    .function("isBikjang", &Board::is_bikjang)
    .function("isCapture", &Board::is_capture)
    .function("moveStack", &Board::move_stack)
    .function("pushMoves", &Board::push_moves)
    .function("toString", &Board::to_string)
    .function("toVerboseString", &Board::to_verbose_string)
    .function("variant", &Board::variant);
  // usage: e.g. ffish.Termination.CHECKMATE
  enum_<Termination>("Termination")
    .value("ONGOING", ONGOING)
    .value("CHECKMATE", CHECKMATE)
    .value("STALEMATE", STALEMATE)
    .value("INSUFFICIENT_MATERIAL", INSUFFICIENT_MATERIAL)
    .value("N_MOVE_RULE", N_MOVE_RULE)
    .value("N_FOLD_REPETITION", N_FOLD_REPETITION)
    .value("VARIANT_END", VARIANT_END);
  function("info", &ffish::info);
  function("setOption", &ffish::set_option<std::string>);
  function("setOptionInt", &ffish::set_option<int>);
  function("setOptionBool", &ffish::set_option<bool>);
  function("variants", &ffish::available_variants);
  function("startingFen", &ffish::starting_fen);
  function("validateFen", select_overload<int(std::string)>(&ffish::validate_fen));
  function("validateFen", select_overload<int(std::string, std::string)>(&ffish::validate_fen));
  // TODO: enable to string conversion method
  // .class_function("getStringFromInstance", &Board::get_string_from_instance);
}

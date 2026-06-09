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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#include <iostream>
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"

namespace Stockfish {

namespace Search {

  LimitsType Limits;
}

using std::string;
using Eval::evaluate;
using namespace Search;

namespace {

  // Different node types, used as a template parameter
  enum NodeType { NonPV, PV, Root };

  enum XiangqiMoveCategory {
    XMC_NONE                  = 0,
    XMC_DIRECT_CHECK          = 1 << 0,
    XMC_PALACE_BREAK_CAPTURE  = 1 << 1,
    XMC_KING_LINE_PRESSURE    = 1 << 2,
    XMC_PALACE_PRESSURE       = 1 << 3,
    XMC_PASSED_SOLDIER_PUSH   = 1 << 4,
    XMC_SACRIFICE_CONTINUATION= 1 << 5,
    XMC_CHECK_THREAT          = 1 << 6,
    XMC_GUARD_THREAT          = 1 << 7,
    XMC_UNANSWERABLE_THREAT   = 1 << 8,
    XMC_OVERLOAD_ATTACK       = 1 << 9,
    XMC_SIMPLIFICATION        = 1 << 10,
    XMC_SACRIFICE_OFFER       = 1 << 11,
    XMC_CANNON_BATTERY        = 1 << 12,
    XMC_ATTACK_MASK           = XMC_DIRECT_CHECK | XMC_PALACE_BREAK_CAPTURE
                              | XMC_KING_LINE_PRESSURE | XMC_PALACE_PRESSURE
                              | XMC_PASSED_SOLDIER_PUSH | XMC_SACRIFICE_CONTINUATION
                              | XMC_CHECK_THREAT | XMC_GUARD_THREAT
                              | XMC_UNANSWERABLE_THREAT | XMC_OVERLOAD_ATTACK,
    XMC_SEARCH_PROTECTION_MASK= XMC_DIRECT_CHECK | XMC_PALACE_BREAK_CAPTURE
                              | XMC_SACRIFICE_CONTINUATION | XMC_CHECK_THREAT
                              | XMC_UNANSWERABLE_THREAT | XMC_OVERLOAD_ATTACK
  };

  // Fixed-point LMR lookup table, initialized at startup.
  int PikaReductions[MAX_MOVES];
  constexpr uint64_t NODES_LIMIT_OUTPUT = 10000000;
  constexpr bool XiangqiHasMandatoryCapture = false;

  constexpr int LmrDivisor[16] = {3307, 2930, 2874, 2818, 3215, 3225, 3224, 2782,
                                  2858, 2919, 3088, 3275, 3180, 2868, 3006, 3599};

  bool xiangqi_attacker_type(PieceType pt);

  // History and stats update bonus, based on depth
  int stat_bonus(Depth d) {
    return d > 14 ? 73 : 6 * d * d + 229 * d - 215;
  }

  bool decisive(Value v) {
    return std::abs(int(v)) >= VALUE_KNOWN_WIN;
  }

  int xiangqi_material(const Position& pos, Color c) {
    return int(PieceValue[MG][make_piece(c, ROOK)])    * pos.count<ROOK>(c)
         + int(PieceValue[MG][make_piece(c, HORSE)])   * pos.count<HORSE>(c)
         + int(PieceValue[MG][make_piece(c, CANNON)])  * pos.count<CANNON>(c)
         + int(PieceValue[MG][make_piece(c, FERS)])    * pos.count<FERS>(c)
         + int(PieceValue[MG][make_piece(c, ELEPHANT)])* pos.count<ELEPHANT>(c)
         + int(PieceValue[MG][make_piece(c, SOLDIER)]) * pos.count<SOLDIER>(c);
  }

  int xiangqi_material_balance(const Position& pos, Color root) {
    return xiangqi_material(pos, root) - xiangqi_material(pos, ~root);
  }

  int xiangqi_total_material(const Position& pos) {
    return xiangqi_material(pos, WHITE) + xiangqi_material(pos, BLACK);
  }

  Bitboard xiangqi_palace_zone(const Position& pos, Color c) {
    if (!pos.count<KING>(c))
        return Bitboard(0);

    Square ksq = pos.square<KING>(c);
    int kFile = int(file_of(ksq));
    Bitboard zone = 0;

    for (int f = std::max(0, kFile - 1); f <= std::min(int(pos.max_file()), kFile + 1); ++f)
        for (int r = 0; r <= int(pos.max_rank()); ++r)
            if (int(relative_rank(c, Rank(r), pos.max_rank())) <= 2)
                zone |= make_square(File(f), Rank(r));

    return zone;
  }

  bool xiangqi_near_enemy_palace(const Position& pos, Color root, Square s) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return false;

    Square ksq = pos.square<KING>(them);
    return   (xiangqi_palace_zone(pos, them) & s)
          || (distance<File>(s, ksq) <= 1 && distance<Rank>(s, ksq) <= 3);
  }

  Bitboard xiangqi_attackers(const Position& pos, Color c) {
    return pos.pieces(c, ROOK) | pos.pieces(c, CANNON)
         | pos.pieces(c, HORSE) | pos.pieces(c, SOLDIER);
  }

  int xiangqi_guard_damage(const Position& pos, Color root) {
    Color them = ~root;
    return 40 * std::max(0, 2 - pos.count<FERS>(them))
         + 22 * std::max(0, 2 - pos.count<ELEPHANT>(them));
  }

  int xiangqi_attack_pressure(const Position& pos, Color root) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return 0;

    Square ksq = pos.square<KING>(them);
    Bitboard palace = xiangqi_palace_zone(pos, them);
    Bitboard attackers = xiangqi_attackers(pos, root);

    int pressure = xiangqi_guard_damage(pos, root);
    pressure += 42 * popcount(pos.attackers_to(ksq, root) & attackers);

    Bitboard zone = palace;
    while (zone)
    {
        Square s = pop_lsb(zone);
        Bitboard a = pos.attackers_to(s, root) & attackers;
        pressure += 10 * popcount(a);
    }

    Bitboard pieces = attackers;
    while (pieces)
    {
        Square s = pop_lsb(pieces);
        PieceType pt = type_of(pos.piece_on(s));
        int fd = distance<File>(s, ksq);
        int rd = distance<Rank>(s, ksq);

        if (fd <= 1 && rd <= 3)
            pressure += pt == ROOK ? 34 : pt == CANNON ? 38 : pt == HORSE ? 28 : 14;
        else if (pt == ROOK || pt == CANNON)
        {
            if (file_of(s) == file_of(ksq) || rank_of(s) == rank_of(ksq))
                pressure += 16;
        }
    }

    return std::min(360, pressure);
  }

  bool xiangqi_attack_shape(const Position& pos, Color root) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return false;

    Square ksq = pos.square<KING>(them);
    Bitboard pieces = xiangqi_attackers(pos, root);

    while (pieces)
    {
        Square s = pop_lsb(pieces);
        PieceType pt = type_of(pos.piece_on(s));

        if (xiangqi_near_enemy_palace(pos, root, s))
            return true;

        if ((pt == ROOK || pt == CANNON)
            && (file_of(s) == file_of(ksq) || rank_of(s) == rank_of(ksq)))
            return true;

        if (pt == SOLDIER
            && relative_rank(root, s, pos.max_rank()) >= 5
            && distance<File>(s, ksq) <= 2)
            return true;
    }

    return false;
  }

  int xiangqi_required_sacrifice_quality(int materialLoss) {
    return 70 + std::min(materialLoss, 1200) / 12;
  }

  int xiangqi_sacrifice_quality(const Position& pos, Color root, int materialLoss, int pressure) {
    if (materialLoss <= 0 || !pos.count<KING>(~root))
        return 0;

    Color them = ~root;
    Square ksq = pos.square<KING>(them);
    Bitboard attackers = xiangqi_attackers(pos, root);
    int guardDamage = xiangqi_guard_damage(pos, root);
    int directKing = popcount(pos.attackers_to(ksq, root) & attackers);
    int nearPalace = 0;
    int kingLines = 0;
    int advancedSoldiers = 0;
    int guardThreats = 0;

    Bitboard pieces = attackers;
    while (pieces)
    {
        Square s = pop_lsb(pieces);
        PieceType pt = type_of(pos.piece_on(s));

        if (xiangqi_near_enemy_palace(pos, root, s))
            nearPalace++;

        if ((pt == ROOK || pt == CANNON)
            && (file_of(s) == file_of(ksq) || rank_of(s) == rank_of(ksq)))
            kingLines++;

        if (pt == SOLDIER
            && relative_rank(root, s, pos.max_rank()) >= 5
            && distance<File>(s, ksq) <= 2)
            advancedSoldiers++;

        if (pos.attacks_from(root, pt, s) & (pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT)))
            guardThreats++;
    }

    int quality = pressure + guardDamage / 2
                + 34 * directKing + 18 * nearPalace + 18 * kingLines
                + 14 * advancedSoldiers + 16 * guardThreats;

    if (materialLoss >= 600 && directKing == 0 && nearPalace < 2 && pressure < 115)
        quality -= 70;

    if (pressure < 65 && guardDamage < 40 && directKing == 0)
        quality /= 3;

    return std::clamp(quality, 0, 512);
  }

  int xiangqi_defender_duties(const Position& pos, Color root, Square s, PieceType pt,
                              Bitboard palace, Bitboard rootAttackers) {
    Color them = ~root;
    Bitboard attacks = pos.attacks_from(them, pt, s);
    int duties = 0;

    if (pt == FERS || pt == ELEPHANT)
        duties += 2;

    if (pos.count<KING>(them) && (attacks & pos.square<KING>(them)))
        duties += 2;

    Bitboard guarded = attacks & (pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT));
    duties += popcount(guarded);

    Bitboard zone = palace;
    while (zone)
    {
        Square z = pop_lsb(zone);
        if ((attacks & z) && (pos.attackers_to(z, root) & rootAttackers))
            duties++;
    }

    return duties;
  }

  Bitboard xiangqi_overloaded_defenders(const Position& pos, Color root) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return Bitboard(0);

    Bitboard palace = xiangqi_palace_zone(pos, them);
    Bitboard rootAttackers = xiangqi_attackers(pos, root);
    Bitboard candidates = pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT)
                        | pos.pieces(them, ROOK) | pos.pieces(them, CANNON)
                        | pos.pieces(them, HORSE);
    Bitboard overloaded = 0;

    while (candidates)
    {
        Square s = pop_lsb(candidates);
        PieceType pt = type_of(pos.piece_on(s));
        int duties = xiangqi_defender_duties(pos, root, s, pt, palace, rootAttackers);
        int attackers = popcount(pos.attackers_to(s, root) & rootAttackers);
        int defenders = popcount(pos.attackers_to(s, them) & pos.pieces(them));

        if (   (duties >= 3 && attackers)
            || (duties >= 2 && attackers > defenders)
            || ((pt == FERS || pt == ELEPHANT) && attackers >= 2))
            overloaded |= s;
    }

    return overloaded;
  }

  int xiangqi_defender_overload(const Position& pos, Color root) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return 0;

    Bitboard palace = xiangqi_palace_zone(pos, them);
    Bitboard rootAttackers = xiangqi_attackers(pos, root);
    Bitboard overloaded = xiangqi_overloaded_defenders(pos, root);
    int score = 0;

    while (overloaded)
    {
        Square s = pop_lsb(overloaded);
        PieceType pt = type_of(pos.piece_on(s));
        int duties = xiangqi_defender_duties(pos, root, s, pt, palace, rootAttackers);
        int attackers = popcount(pos.attackers_to(s, root) & rootAttackers);
        int defenders = popcount(pos.attackers_to(s, them) & pos.pieces(them));

        score += 24 + 14 * duties + 12 * std::max(0, attackers - defenders);
        if (pt == FERS || pt == ELEPHANT)
            score += 18;
        if (xiangqi_near_enemy_palace(pos, root, s))
            score += 12;
    }

    return std::clamp(score, 0, 260);
  }

  int xiangqi_palace_threat(const Position& pos, Color root, int pressure) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return 0;

    Square ksq = pos.square<KING>(them);
    Bitboard palace = xiangqi_palace_zone(pos, them);
    Bitboard attackers = xiangqi_attackers(pos, root);
    Bitboard defenders = pos.pieces(them);
    int score = pressure / 3 + xiangqi_guard_damage(pos, root) / 2;
    int forcingPoints = 0;

    int direct = popcount(pos.attackers_to(ksq, root) & attackers);
    if (direct)
    {
        score += 46 * direct;
        forcingPoints += 1 + direct;
    }

    Bitboard zone = palace;
    while (zone)
    {
        Square s = pop_lsb(zone);
        int a = popcount(pos.attackers_to(s, root) & attackers);
        int d = popcount(pos.attackers_to(s, them) & defenders);

        if (!a)
            continue;

        score += 9 * a;
        if (a > d)
        {
            score += 18 * (a - d);
            forcingPoints++;
        }
        if (a >= 2)
            forcingPoints++;
    }

    Bitboard guards = pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT);
    while (guards)
    {
        Square s = pop_lsb(guards);
        int a = popcount(pos.attackers_to(s, root) & attackers);
        int d = popcount(pos.attackers_to(s, them) & defenders);

        if (!a)
            continue;

        score += 18 * a + 12 * std::max(0, a - d);
        if (a > d)
            forcingPoints++;
    }

    if (forcingPoints >= 4 && score >= 135)
        score += 70;
    else if (forcingPoints >= 3 && score >= 115)
        score += 40;
    else if (forcingPoints < 2)
        score /= 2;

    return std::clamp(score, 0, 320);
  }

  bool xiangqi_move_attacks_targets(const Position& pos, Color root, Move move, Bitboard targets) {
    if (!targets)
        return false;

    PieceType pt = type_of(pos.moved_piece(move));
    if (!xiangqi_attacker_type(pt))
        return false;

    Square to = to_sq(move);
    return (targets & to) || (pos.attacks_from(root, pt, to) & targets);
  }

  bool xiangqi_same_orthogonal_line(Square a, Square b) {
    return file_of(a) == file_of(b) || rank_of(a) == rank_of(b);
  }

  Bitboard xiangqi_between_without_target(Square from, Square target) {
    return between_bb(from, target) ^ square_bb(target);
  }

  int xiangqi_cannon_battery_score(const Position& pos, Color root, Move move,
                                   bool givesCheck, Bitboard overloadedDefenders) {
    if (root >= COLOR_NB || pos.side_to_move() != root)
        return 0;

    Color them = ~root;
    if (!pos.count<KING>(them))
        return 0;

    Square from = from_sq(move);
    Square to = to_sq(move);
    Square ksq = pos.square<KING>(them);
    Piece moved = pos.moved_piece(move);
    PieceType movedType = type_of(moved);
    Bitboard toBB = square_bb(to);
    Bitboard occupiedAfter = (pos.pieces() ^ square_bb(from)) | toBB;
    int score = 0;

    if (movedType == CANNON && xiangqi_same_orthogonal_line(to, ksq))
    {
        Bitboard screens = xiangqi_between_without_target(to, ksq) & occupiedAfter;
        if (popcount(screens) == 1)
        {
            Square screen = lsb(screens);
            score += givesCheck ? 130 : 92;
            if (overloadedDefenders & screen)
                score += 34;
            if (xiangqi_near_enemy_palace(pos, root, screen))
                score += 18;
        }
    }

    Bitboard cannons = pos.pieces(root, CANNON);
    while (cannons)
    {
        Square csq = pop_lsb(cannons);
        if (csq == from || !xiangqi_same_orthogonal_line(csq, ksq))
            continue;

        Bitboard screens = xiangqi_between_without_target(csq, ksq) & occupiedAfter;
        int screenCount = popcount(screens);

        if (screenCount == 1)
        {
            Square screen = lsb(screens);
            if (screen == to)
                score += givesCheck ? 112 : 82;
            else if (overloadedDefenders & screen)
                score += 42;
        }
        else if (screenCount == 2 && (screens & toBB))
            score += 30;
    }

    if (score)
    {
        if (xiangqi_near_enemy_palace(pos, root, to))
            score += 22;
        if (pos.capture(move))
        {
            Piece captured = pos.piece_on(to);
            if (type_of(captured) == FERS || type_of(captured) == ELEPHANT)
                score += 34;
        }
    }

    return std::clamp(score, 0, 280);
  }

  int xiangqi_sacrifice_offer_level(const Position& pos, Color root, Move move,
                                    int category, int pressure) {
    if (root >= COLOR_NB || pos.side_to_move() != root)
        return 0;

    PieceType pt = type_of(pos.moved_piece(move));
    if (!xiangqi_attacker_type(pt) && pt != FERS && pt != ELEPHANT)
        return 0;

    int tacticalHooks = category & (XMC_DIRECT_CHECK | XMC_PALACE_BREAK_CAPTURE
                                  | XMC_SACRIFICE_CONTINUATION | XMC_CHECK_THREAT
                                  | XMC_UNANSWERABLE_THREAT | XMC_OVERLOAD_ATTACK);
    if (!tacticalHooks)
        return 0;

    if (pos.see_ge(move, Value(-80)))
        return 0;

    int level = pos.see_ge(move, Value(-200)) ? 1
              : pos.see_ge(move, Value(-450)) ? 2 : 3;

    if (pressure < 120
        && !(category & (XMC_DIRECT_CHECK | XMC_PALACE_BREAK_CAPTURE
                       | XMC_UNANSWERABLE_THREAT | XMC_OVERLOAD_ATTACK)))
        return 0;

    if (level >= 3
        && (pressure < 155 || !(category & (XMC_DIRECT_CHECK | XMC_UNANSWERABLE_THREAT))))
        return 0;

    if (level >= 2
        && !(category & (XMC_DIRECT_CHECK | XMC_PALACE_BREAK_CAPTURE | XMC_UNANSWERABLE_THREAT)))
        return 0;

    return level;
  }

  int xiangqi_move_palace_threat_score(const Position& pos, Color root, Move move, bool givesCheck,
                                       Bitboard overloadedDefenders) {
    Color them = ~root;
    if (!pos.count<KING>(them))
        return 0;

    PieceType pt = type_of(pos.moved_piece(move));
    if (!xiangqi_attacker_type(pt))
        return givesCheck ? 80 : 0;

    Square to = to_sq(move);
    Square ksq = pos.square<KING>(them);
    Bitboard attacks = pos.attacks_from(root, pt, to);
    Bitboard guards = pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT);
    int score = givesCheck ? 90 : 0;

    if (attacks & ksq)
        score += 70;
    if (xiangqi_near_enemy_palace(pos, root, to))
        score += 35;
    if ((pt == ROOK || pt == CANNON)
        && (file_of(to) == file_of(ksq) || rank_of(to) == rank_of(ksq)))
        score += 45;
    if (attacks & guards)
        score += 34 + 14 * popcount(attacks & guards);
    if (attacks & overloadedDefenders)
        score += 50 + 16 * popcount(attacks & overloadedDefenders);
    if (pt == SOLDIER
        && relative_rank(root, to, pos.max_rank()) >= 5
        && distance<File>(to, ksq) <= 2)
        score += 24;

    return std::clamp(score, 0, 260);
  }

  int xiangqi_root_material_loss(Thread* thisThread, int materialBalance) {
    if (thisThread->rootColor >= COLOR_NB)
        return 0;

    return std::max(0, thisThread->rootMaterialBalance - materialBalance);
  }

  bool xiangqi_style_search_enabled(Thread* thisThread) {
    return thisThread->cachedAggressiveness > 100 || thisThread->cachedSacDetect > 0;
  }

  int xiangqi_next_material_balance(const Position& pos, Thread* thisThread, int materialBalance, Move move) {
    if (thisThread->rootColor >= COLOR_NB || !pos.capture(move))
        return materialBalance;

    Piece captured = pos.piece_on(to_sq(move));
    if (!captured)
        return materialBalance;

    int capturedValue = int(PieceValue[MG][captured]);
    return color_of(captured) == thisThread->rootColor ? materialBalance - capturedValue
                                                       : materialBalance + capturedValue;
  }

  bool xiangqi_attack_phase(const Position& pos, Thread* thisThread, int materialBalance, int pressure, int sacrificeTrace) {
    if (thisThread->rootColor >= COLOR_NB)
        return false;

    int sacLoss = xiangqi_root_material_loss(thisThread, materialBalance);

    return pressure >= 135
        || (sacLoss > 0 && pressure >= 75)
        || (xiangqi_guard_damage(pos, thisThread->rootColor) >= 62 && pressure >= 95)
        || sacrificeTrace >= 170;
  }

  int xiangqi_attack_bias(Thread* thisThread) {
    return std::max(0, thisThread->cachedAggressiveness - 100);
  }

  bool xiangqi_breaking_capture(Piece pc) {
    PieceType pt = type_of(pc);
    return pt == FERS || pt == ELEPHANT || pt == CANNON || pt == HORSE || pt == ROOK;
  }

  bool xiangqi_attacking_move(const Position& pos, Move move, bool givesCheck) {
    return givesCheck || (pos.capture(move) && xiangqi_breaking_capture(pos.piece_on(to_sq(move))));
  }

  bool xiangqi_sacrifice_continuation(const Position& pos, Thread* thisThread, Move move, bool givesCheck,
                                      int materialBalance) {
    if (thisThread->rootColor >= COLOR_NB
        || pos.side_to_move() != thisThread->rootColor
        || xiangqi_root_material_loss(thisThread, materialBalance) <= 0)
        return false;

    Piece moved = pos.moved_piece(move);
    PieceType pt = type_of(moved);
    if (pt != ROOK && pt != CANNON && pt != HORSE && pt != SOLDIER)
        return false;

    if (givesCheck)
        return true;

    Square to = to_sq(move);
    Color them = ~thisThread->rootColor;
    return   xiangqi_near_enemy_palace(pos, thisThread->rootColor, to)
          || (pos.count<KING>(them)
              && (pt == ROOK || pt == CANNON)
              && (file_of(to) == file_of(pos.square<KING>(them))
                  || rank_of(to) == rank_of(pos.square<KING>(them))));
  }

  int xiangqi_move_category(const Position& pos, Thread* thisThread, Move move, bool givesCheck, bool sacContinuation) {
    if (thisThread->rootColor >= COLOR_NB)
        return XMC_NONE;

    int category = givesCheck ? XMC_DIRECT_CHECK : XMC_NONE;

    if (pos.side_to_move() != thisThread->rootColor)
        return category;

    Color root = thisThread->rootColor;
    Color them = ~root;
    Piece moved = pos.moved_piece(move);
    PieceType movedType = type_of(moved);
    Square to = to_sq(move);

    if (pos.capture(move))
    {
        Piece captured = pos.piece_on(to);
        PieceType capturedType = type_of(captured);
        if (capturedType == FERS || capturedType == ELEPHANT)
            category |= XMC_PALACE_BREAK_CAPTURE;
    }

    if (pos.count<KING>(them))
    {
        Square ksq = pos.square<KING>(them);

        if ((movedType == ROOK || movedType == CANNON)
            && (file_of(to) == file_of(ksq) || rank_of(to) == rank_of(ksq)))
            category |= XMC_KING_LINE_PRESSURE;

        if (xiangqi_cannon_battery_score(pos, root, move, givesCheck, Bitboard(0)) >= 95)
            category |= XMC_CANNON_BATTERY;

        if ((movedType == ROOK || movedType == CANNON || movedType == HORSE || movedType == SOLDIER)
            && xiangqi_near_enemy_palace(pos, root, to))
            category |= XMC_PALACE_PRESSURE;

        if (movedType == SOLDIER
            && relative_rank(root, to, pos.max_rank()) >= 5
            && distance<File>(to, ksq) <= 2)
            category |= XMC_PASSED_SOLDIER_PUSH;

        if (movedType == ROOK || movedType == CANNON || movedType == HORSE || movedType == SOLDIER)
        {
            Bitboard attacks = pos.attacks_from(root, movedType, to);

            if (!givesCheck && (attacks & ksq))
                category |= XMC_CHECK_THREAT;

            if (attacks & (pos.pieces(them, FERS) | pos.pieces(them, ELEPHANT)))
                category |= XMC_GUARD_THREAT;
        }
    }

    if (sacContinuation)
        category |= XMC_SACRIFICE_CONTINUATION;

    if ((category & XMC_ATTACK_MASK) && !pos.see_ge(move, Value(-80)))
    {
        int pressure = xiangqi_attack_pressure(pos, root);
        if (xiangqi_sacrifice_offer_level(pos, root, move, category, pressure))
            category |= XMC_SACRIFICE_OFFER;
    }

    return category;
  }

  bool xiangqi_simplification_candidate(const Position& pos, Thread* thisThread, Move move, bool givesCheck,
                                        int category, Value staticEval, bool attackPhase,
                                        int attackPressure, int materialBalance) {
    if (thisThread->rootColor >= COLOR_NB
        || pos.side_to_move() != thisThread->rootColor
        || !pos.capture(move)
        || givesCheck
        || staticEval == VALUE_NONE
        || (category & (XMC_PALACE_BREAK_CAPTURE | XMC_KING_LINE_PRESSURE | XMC_PALACE_PRESSURE
                      | XMC_PASSED_SOLDIER_PUSH | XMC_SACRIFICE_CONTINUATION
                      | XMC_UNANSWERABLE_THREAT | XMC_OVERLOAD_ATTACK
                      | XMC_SACRIFICE_OFFER | XMC_CANNON_BATTERY)))
        return false;

    Piece moved = pos.moved_piece(move);
    Piece captured = pos.piece_on(to_sq(move));
    if (!captured)
        return false;
    PieceType movedType = type_of(moved);
    PieceType capturedType = type_of(captured);

    if (!(movedType == ROOK || movedType == CANNON || movedType == HORSE)
        || !(capturedType == ROOK || capturedType == CANNON || capturedType == HORSE))
        return false;

    if (int(staticEval) < 180 && materialBalance < 300)
        return false;

    if (!attackPhase && attackPressure < 110)
        return false;

    int movedValue = int(PieceValue[MG][moved]);
    int capturedValue = int(PieceValue[MG][captured]);
    return std::abs(movedValue - capturedValue) <= 350 || capturedValue >= HorseValueMg;
  }

  int attack_category_weight(int category) {
    int weight = 96;

    if (category & XMC_DIRECT_CHECK)
        weight += 48;
    if (category & XMC_PALACE_BREAK_CAPTURE)
        weight += 64;
    if (category & XMC_SACRIFICE_CONTINUATION)
        weight += 56;
    if (category & XMC_KING_LINE_PRESSURE)
        weight += 32;
    if (category & XMC_PALACE_PRESSURE)
        weight += 24;
    if (category & XMC_PASSED_SOLDIER_PUSH)
        weight += 16;
    if (category & XMC_CHECK_THREAT)
        weight += 32;
    if (category & XMC_GUARD_THREAT)
        weight += 28;
    if (category & XMC_UNANSWERABLE_THREAT)
        weight += 52;
    if (category & XMC_OVERLOAD_ATTACK)
        weight += 42;
    if ((category & XMC_SACRIFICE_OFFER) && (category & XMC_ATTACK_MASK))
        weight += 18;
    if ((category & XMC_CANNON_BATTERY) && (category & XMC_ATTACK_MASK))
        weight += 14;

    return weight;
  }

  int xiangqi_move_search_protection(int category, int attackBias) {
    if (!(category & XMC_SEARCH_PROTECTION_MASK))
        return 0;

    int protection = attack_category_weight(category & XMC_SEARCH_PROTECTION_MASK) / 3 + attackBias / 2;

    if (category & XMC_SACRIFICE_CONTINUATION)
        protection += 48;
    if (category & XMC_PALACE_BREAK_CAPTURE)
        protection += 40;
    if (category & XMC_DIRECT_CHECK)
        protection += 32;
    if (category & XMC_UNANSWERABLE_THREAT)
        protection += 56;
    if (category & XMC_OVERLOAD_ATTACK)
        protection += 42;

    return std::clamp(protection, 0, 192);
  }

  bool xiangqi_trade_piece(PieceType pt) {
    return pt == ROOK || pt == CANNON || pt == HORSE;
  }

  int xiangqi_simplification_penalty(const Position& pos, Thread* thisThread, Move move, int category,
                                     bool attackPhase, int pressure, int palaceThreat, int overloadScore,
                                     int materialBalance) {
    if (thisThread->rootColor >= COLOR_NB
        || pos.side_to_move() != thisThread->rootColor
        || !pos.capture(move)
        || pos.gives_check(move)
        || (category & (XMC_PALACE_BREAK_CAPTURE | XMC_SACRIFICE_CONTINUATION
                      | XMC_UNANSWERABLE_THREAT | XMC_OVERLOAD_ATTACK
                      | XMC_SACRIFICE_OFFER | XMC_CANNON_BATTERY)))
        return 0;

    Piece moved = pos.moved_piece(move);
    Piece captured = pos.piece_on(to_sq(move));
    if (!captured)
        return 0;

    PieceType movedType = type_of(moved);
    PieceType capturedType = type_of(captured);
    if (!xiangqi_trade_piece(movedType) || !xiangqi_trade_piece(capturedType))
        return 0;

    int movedValue = int(PieceValue[MG][moved]);
    int capturedValue = int(PieceValue[MG][captured]);
    if (capturedValue > movedValue + 180)
        return 0;

    bool attackingContext = attackPhase || pressure >= 105 || palaceThreat >= 115 || overloadScore >= 80;
    if (!attackingContext)
        return 0;

    int penalty = 144 + thisThread->cachedAggressiveness + palaceThreat / 3 + overloadScore / 4;
    if (materialBalance > 250)
        penalty += 80;
    if (xiangqi_root_material_loss(thisThread, materialBalance) > 0)
        penalty += 96;
    if (capturedValue < movedValue - 260)
        penalty += 64;
    if (category & (XMC_KING_LINE_PRESSURE | XMC_PALACE_PRESSURE | XMC_CHECK_THREAT | XMC_GUARD_THREAT))
        penalty /= 2;

    return std::clamp(penalty, 0, 640);
  }

  int xiangqi_node_attack_protection(const Position& pos, Thread* thisThread, int materialBalance,
                                     int pressure, int sacrificeTrace) {
    if (thisThread->rootColor >= COLOR_NB
        || pos.side_to_move() != thisThread->rootColor
        || !xiangqi_style_search_enabled(thisThread))
        return 0;

    int rootLoss = xiangqi_root_material_loss(thisThread, materialBalance);
    int palaceThreat = xiangqi_palace_threat(pos, thisThread->rootColor, pressure);
    int quality = rootLoss > 0 ? xiangqi_sacrifice_quality(pos, thisThread->rootColor, rootLoss, pressure)
                               : pressure + xiangqi_guard_damage(pos, thisThread->rootColor) / 2
                                          + palaceThreat / 3;
    int protection = 0;

    if (pressure >= 125)
        protection += 16 + (pressure - 125) / 4;
    if (palaceThreat >= 135)
        protection += 16 + (palaceThreat - 120) / 5;
    if (sacrificeTrace >= 150)
        protection += (sacrificeTrace - 120) / 5;
    if (rootLoss > 0)
    {
        int required = xiangqi_required_sacrifice_quality(rootLoss);
        if (quality >= required / 2)
            protection += 12 + quality / 12;
        if (quality >= required)
            protection += 16;
    }

    protection += thisThread->cachedDynamicComp * std::min(quality, 360) / 1800;
    return std::clamp(protection, 0, 96);
  }

  bool xiangqi_attacker_type(PieceType pt) {
    return pt == ROOK || pt == CANNON || pt == HORSE || pt == SOLDIER;
  }

  int xiangqi_position_sacrifice_trace(const Position& pos, Thread* thisThread, int materialBalance) {
    if (thisThread->rootColor >= COLOR_NB)
        return 0;

    int rootLoss = xiangqi_root_material_loss(thisThread, materialBalance);
    if (rootLoss <= 0)
        return 0;

    int pressure = xiangqi_attack_pressure(pos, thisThread->rootColor);
    int guardDamage = xiangqi_guard_damage(pos, thisThread->rootColor);
    int quality = xiangqi_sacrifice_quality(pos, thisThread->rootColor, rootLoss, pressure);
    int required = xiangqi_required_sacrifice_quality(rootLoss);
    int trace = std::min(160, rootLoss / 6) + quality / 2 + guardDamage / 3;

    if (pressure >= 75)
        trace += 45;
    if (pressure >= 135)
        trace += 55;
    if (quality >= required)
        trace += 55 + std::min(quality - required, 160) / 4;
    else if (quality < required / 2)
        trace = trace * std::max(quality, 1) / std::max(required / 2, 1);

    if (rootLoss >= 350 && quality >= required)
        trace += 45;

    if (pressure < 60 && guardDamage < 40)
        trace /= 3;

    return std::clamp(trace, 0, 512);
  }

  int xiangqi_next_sacrifice_trace(const Position& pos, Thread* thisThread, Stack* ss,
                                   Move move, bool givesCheck, int nextMaterialBalance) {
    if (thisThread->rootColor >= COLOR_NB)
        return 0;

    int trace = std::max(0, ss->sacrificeTrace - 6);
    trace = std::max(trace, xiangqi_position_sacrifice_trace(pos, thisThread, nextMaterialBalance));

    Color root = thisThread->rootColor;
    int oldLoss = xiangqi_root_material_loss(thisThread, ss->materialDiff);
    int newLoss = xiangqi_root_material_loss(thisThread, nextMaterialBalance);
    if (newLoss > 0)
    {
        int pressure = xiangqi_attack_pressure(pos, root);
        int quality = xiangqi_sacrifice_quality(pos, root, newLoss, pressure);
        int required = xiangqi_required_sacrifice_quality(newLoss);

        if (quality >= required)
            trace += std::min(70, (quality - required) / 3 + thisThread->cachedDynamicComp / 3);
        else if (quality < required / 2)
            trace = std::max(0, trace - 35);
    }

    if (pos.side_to_move() == root)
    {
        int category = xiangqi_move_category(pos, thisThread, move, givesCheck, false);
        if (category & XMC_ATTACK_MASK)
            trace += attack_category_weight(category) / 4;

        Piece moved = pos.moved_piece(move);
        PieceType movedType = type_of(moved);
        if (xiangqi_attacker_type(movedType)
            && (givesCheck || (category & XMC_ATTACK_MASK)))
        {
            if (pos.capture(move) && !pos.see_ge(move, Value(-120)))
                trace += 70;
            else if (!pos.capture(move) && xiangqi_near_enemy_palace(pos, root, to_sq(move)))
                trace += 35;
        }
    }
    else if (newLoss > oldLoss)
        trace += std::min(90, (newLoss - oldLoss) / 3);

    return std::clamp(trace, 0, 512);
  }

  int pikafish_reduction(Thread* thisThread, bool improving, Depth d, int moveNumber, int delta) {
    d = std::clamp(d, 1, MAX_MOVES - 1);
    moveNumber = std::clamp(moveNumber, 1, MAX_MOVES - 1);

    int reductionScale = PikaReductions[d] * PikaReductions[moveNumber];
    int rootDelta = std::max(1, int(thisThread->rootDelta));

    return reductionScale - delta * 1138 / rootDelta
         + (!improving) * reductionScale * 166 / 512
         + 1934;
  }

  Value to_corrected_static_eval(Value v, int correctionValue) {
    return Value(std::clamp(int(v) + correctionValue / 131072,
                            int(VALUE_MATED_IN_MAX_PLY + 1),
                            int(VALUE_MATE_IN_MAX_PLY - 1)));
  }

  int correction_value(Thread* thisThread, const Position& pos, Stack* ss) {
    const Color us = pos.side_to_move();
    const auto& history = thisThread->correctionHistory;
    constexpr int Mask = CORRECTION_HISTORY_SIZE - 1;

    int pawnCorrection = history[pos.pawn_key() & Mask][us][PawnCorrection];
    int materialCorrection = history[pos.minor_piece_key() & Mask][us][MaterialCorrection];
    int whiteNonPawnCorrection = history[pos.non_pawn_key(WHITE) & Mask][us][NonPawnWhiteCorrection];
    int blackNonPawnCorrection = history[pos.non_pawn_key(BLACK) & Mask][us][NonPawnBlackCorrection];
    Move previousMove = (ss - 1)->currentMove;
    int continuationCorrection = is_ok(previousMove)
        ? 8982 * ((*(ss - 2)->continuationCorrectionHistory)[history_slot(pos.piece_on(to_sq(previousMove)))][history_square(to_sq(previousMove))]
                + (*(ss - 4)->continuationCorrectionHistory)[history_slot(pos.piece_on(to_sq(previousMove)))][history_square(to_sq(previousMove))])
        : 71856;

    return 4547 * pawnCorrection
         + 3804 * materialCorrection
         + 8213 * (whiteNonPawnCorrection + blackNonPawnCorrection)
         + continuationCorrection;
  }

  void update_correction_history(const Position& pos, Stack* ss, Thread* thisThread, int bonus) {
    const Color us = pos.side_to_move();
    auto& history = thisThread->correctionHistory;
    constexpr int Mask = CORRECTION_HISTORY_SIZE - 1;

    history[pos.pawn_key() & Mask][us][PawnCorrection] << bonus;
    history[pos.minor_piece_key() & Mask][us][MaterialCorrection] << bonus * 145 / 128;
    history[pos.non_pawn_key(WHITE) & Mask][us][NonPawnWhiteCorrection] << bonus * 125 / 128;
    history[pos.non_pawn_key(BLACK) & Mask][us][NonPawnBlackCorrection] << bonus * 125 / 128;

    Move previousMove = (ss - 1)->currentMove;
    if (is_ok(previousMove))
    {
        Square to = to_sq(previousMove);
        Piece pc = pos.piece_on(to);
        (*(ss - 2)->continuationCorrectionHistory)[history_slot(pc)][history_square(to)] << bonus * 131 / 128;
        (*(ss - 4)->continuationCorrectionHistory)[history_slot(pc)][history_square(to)] << bonus * 63 / 128;
    }
  }

  bool is_shuffling(Move move, Stack* ss, const Position& pos) {
    if (pos.capture_or_promotion(move) || pos.rule50_count() < 10)
        return false;
    if (pos.state()->pliesFromNull < 6 || ss->ply < 20)
        return false;
    return from_sq(move) == to_sq((ss - 2)->currentMove)
        && from_sq((ss - 2)->currentMove) == to_sq((ss - 4)->currentMove);
  }

  // Add a small random component to draw evaluations to avoid 3-fold blindness
  Value value_draw(Thread* thisThread, Color us) {
    int rootDraw = thisThread->cachedDrawValue;

    Value contempt = thisThread->rootColor < COLOR_NB
                   ? Value(us == thisThread->rootColor ? rootDraw : -rootDraw)
                   : VALUE_ZERO;

    int drawMatBias = thisThread->cachedDrawMatBias;
    if (drawMatBias != 0 && thisThread->rootColor < COLOR_NB)
    {
        Position& rootPos = thisThread->rootPos;
        int matDiff = rootPos.non_pawn_material(WHITE) - rootPos.non_pawn_material(BLACK);
        int matImbalance = (thisThread->rootColor == WHITE) ? matDiff : -matDiff;
        int bias = drawMatBias * matImbalance / 500;
        contempt += Value(us == thisThread->rootColor ? bias : -bias);
    }

    return VALUE_DRAW + contempt + Value(2 * (thisThread->nodes & 1) - 1);
  }

  // Skill structure is used to implement strength limit
  struct Skill {
    explicit Skill(int l) : level(l) {}
    bool enabled() const { return level < 20; }
    bool time_to_pick(Depth depth) const { return depth == 1 + std::max(level, 0); }
    Move pick_best(size_t multiPV);

    int level;
    Move best = MOVE_NONE;
  };

  template <NodeType nodeType>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);

  template <NodeType nodeType>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = 0);

  Value value_to_tt(Value v, int ply);
  Value value_from_tt(Value v, int ply, int r50c);
  void update_pv(Move* pv, Move move, Move* childPv);
  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
  void update_attack_stats(const Position& pos, Move move, int bonus);
  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus, int depth);
  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth);

  // perft() is our utility to verify move generation. All the leaf nodes up
  // to the given depth are generated and counted, and the sum is returned.
  template<bool Root>
  uint64_t perft(Position& pos, Depth depth) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        assert(pos.pseudo_legal(m));
        if (Root && depth <= 1)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - 1);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << UCI::move(pos, m) << ": " << cnt << sync_endl;
    }
    return nodes;
  }

} // namespace


/// Search::init() is called at startup to initialize various lookup tables

void Search::init() {

  for (int i = 1; i < MAX_MOVES; ++i)
      PikaReductions[i] = int(17.40 * std::log(i));
}


/// Search::clear() resets search state to its initial value

void Search::clear() {

  Threads.main()->wait_for_search_finished();

  Time.clear();
  TT.clear();
  Threads.clear();
}


/// MainThread::search() is started when the program receives the UCI 'go'
/// command. It searches from the root position and outputs the "bestmove".

void MainThread::search() {

  if (Limits.perft)
  {
      nodes = perft<true>(rootPos, Limits.perft);
      sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
      return;
  }

  Color us = rootPos.side_to_move();
  Time.init(rootPos, Limits, us, rootPos.game_ply(), originalTimeAdjust);
  TT.new_search();

  Eval::NNUE::verify();

  if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      Value variantResult;
      Value result =  rootPos.is_game_end(variantResult) ? variantResult
                    : rootPos.checkers()                 ? rootPos.checkmate_value()
                                                         : rootPos.stalemate_value();
      sync_cout << "info depth 0 score "
                << UCI::value(result)
                << sync_endl;
  }
  else
  {
      Threads.start_searching(); // start non-main threads
      Thread::search();          // main thread start searching
  }

  // When we reach the maximum depth, we can arrive here without a raise of
  // Threads.stop. However, if we are pondering or in an infinite search,
  // the UCI protocol states that we shouldn't print the best move before the
  // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
  // until the GUI sends one of those commands.

  while (!Threads.stop && (ponder || Limits.infinite))
  {} // Busy wait for a stop or a ponder reset

  // Stop the threads if not already stopped (also raise the stop if
  // "ponderhit" just reset Threads.ponder).
  Threads.stop = true;

  // Wait until all threads have finished
  Threads.wait_for_search_finished();

  // When playing in 'nodes as time' mode, subtract the searched nodes from
  // the available ones before exiting.
  if (Limits.npmsec)
      Time.advance_nodes_time(Threads.nodes_searched() - Limits.inc[us]);

  bestThread = this;

  if (   int(Options["MultiPV"]) == 1
      && !Limits.depth
      && !(Skill(Options["Skill Level"]).enabled() || int(Options["UCI_LimitStrength"]))
      && rootMoves[0].pv[0] != MOVE_NONE)
      bestThread = Threads.get_best_thread();

  bestPreviousScore = bestThread->rootMoves[0].score;
  bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;

  // Send again PV info if we have a new best thread
  if (bestThread != this)
      sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;

  sync_cout << "bestmove " << UCI::move(rootPos, bestThread->rootMoves[0].pv[0]);

  if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
      std::cout << " ponder " << UCI::move(rootPos, bestThread->rootMoves[0].pv[1]);

  std::cout << sync_endl;
}


/// Thread::search() is the main iterative deepening loop. It calls search()
/// repeatedly with increasing depth until the allocated thinking time has been
/// consumed, the user stops the search, or the maximum search depth is reached.

void Thread::search() {

  // To allow access to (ss-7) up to (ss+2), the stack must be oversized.
  // The former is needed to allow update_continuation_histories(ss-1, ...),
  // which accesses its argument at ss-6, also near the root.
  // The latter is needed for statScore and killer initialization.
  Stack stack[MAX_PLY+10], *ss = stack+7;
  Move  pv[MAX_PLY+1];
  Value bestValue, alpha, beta;
  int delta;
  Depth lastBestMoveDepth = 0;
  std::vector<Move> lastIterationPV;
  Value lastIterationScore = -VALUE_INFINITE;
  MainThread* mainThread = (this == Threads.main() ? Threads.main() : nullptr);
  double timeReduction = 1, totBestMoveChanges = 0;
  Color us = rootPos.side_to_move();
  rootColor = us;
  int iterIdx = 0;

  cachedAggressiveness = int(Options["Aggressiveness"]);
  cachedDrawValue = int(Options["DrawValue"]);
  cachedDynamicComp = int(Options["DynamicComp"]);
  cachedSacBonus = int(Options["SacBonus"]);
  cachedAdvisorBreakBonus = int(Options["AdvisorBreakBonus"]);
  cachedBishopBreakBonus = int(Options["BishopBreakBonus"]);
  cachedMatScale = int(Options["MatScale"]);
  cachedSacDetect = int(Options["SacDetect"]);
  cachedDrawMatBias = int(Options["DrawMatBias"]);
  cachedEvalDecay = int(Options["EvalDecay"]);
  rootMaterialBalance = xiangqi_material_balance(rootPos, us);
  rootDelta = VALUE_INFINITE;

  std::memset(ss-7, 0, 10 * sizeof(Stack));
  for (int i = 7; i > 0; i--)
  {
      (ss-i)->continuationHistory = &this->continuationHistory[0][0][history_slot(NO_PIECE)][0]; // Use as a sentinel
      (ss-i)->continuationCorrectionHistory = &this->continuationCorrectionHistory[history_slot(NO_PIECE)][0];
  }

  for (int i = 0; i <= MAX_PLY + 2; ++i)
      (ss+i)->ply = i;

  ss->materialDiff = rootMaterialBalance;
  ss->sacrificeTrace = 0;

  ss->pv = pv;

  bestValue = alpha = -VALUE_INFINITE;
  delta = 0;
  beta = VALUE_INFINITE;

  if (mainThread)
  {
      if (mainThread->bestPreviousScore == VALUE_INFINITE)
      {
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = VALUE_ZERO;
          mainThread->bestPreviousAverageScore = VALUE_ZERO;
      }
      else
      {
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = mainThread->bestPreviousScore;
          if (mainThread->bestPreviousAverageScore == VALUE_INFINITE)
              mainThread->bestPreviousAverageScore = mainThread->bestPreviousScore;
      }
  }

  std::copy(&lowPlyHistory[2][0], &lowPlyHistory.back().back() + 1, &lowPlyHistory[0][0]);
  std::fill(&lowPlyHistory[MAX_LPH - 2][0], &lowPlyHistory.back().back() + 1, 0);

  for (Color c : { WHITE, BLACK })
      for (int i = 0; i < XIANGQI_HISTORY_MOVE_NB; ++i)
          mainHistory[c][i] = (int(mainHistory[c][i]) + 5) * 768 / 1024;

  size_t multiPV = size_t(Options["MultiPV"]);

  // Pick integer skill levels, but non-deterministically round up or down
  // such that the average integer skill corresponds to the input floating point one.
  // UCI_Elo is converted to a suitable fractional skill level, using anchoring
  // to CCRL Elo (goldfish 1.13 = 2000) and a fit through Ordo derived Elo
  // for match (TC 60+0.6) results spanning a wide range of k values.
  PRNG rng(now());
  double shiftedElo = Options["UCI_Elo"] - 1346.6;
  double floatLevel = Options["UCI_LimitStrength"] ?
                      std::clamp(shiftedElo > 0 ? std::pow(shiftedElo / 143.4, 1 / 0.806)
                                                : shiftedElo / 143.4 + std::pow(shiftedElo / 500, 5),
                                 -20.0, 20.0) :
                        double(Options["Skill Level"]);
  int intLevel = int(floatLevel) +
                 ((floatLevel - int(floatLevel)) * 1024 > rng.rand<unsigned>() % 1024  ? 1 : 0);
  Skill skill(intLevel);

  // When playing with strength handicap enable MultiPV search that we will
  // use behind the scenes to retrieve a set of possible moves.
  if (skill.enabled())
      multiPV = std::max(multiPV, (size_t)4);

  multiPV = std::min(multiPV, rootMoves.size());
  trend = SCORE_ZERO;

  int searchAgainCounter = 0;

  // Iterative deepening loop until requested to stop or the target depth is reached
  while (   ++rootDepth < MAX_PLY
         && !Threads.stop
         && !(Limits.depth && mainThread && rootDepth > Limits.depth))
  {
      // Age out PV variability metric
      if (mainThread)
          totBestMoveChanges /= 2;

      // Save the last iteration's scores before first PV line is searched and
      // all the move scores except the (new) PV are set to -VALUE_INFINITE.
      for (RootMove& rm : rootMoves)
          rm.previousScore = rm.score;

      size_t pvFirst = 0;
      pvLast = 0;

      if (!Threads.increaseDepth)
         searchAgainCounter++;

      // MultiPV loop. We perform a full root search for each PV line
      for (pvIdx = 0; pvIdx < multiPV && !Threads.stop; ++pvIdx)
      {
          if (pvIdx == pvLast)
          {
              pvFirst = pvLast;
              for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                  if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                      break;
          }

          // Reset UCI info selDepth for each depth and each PV line
          selDepth = 0;

          // Reset aspiration window starting size
          if (rootDepth >= 4 && rootMoves[pvIdx].averageScore != -VALUE_INFINITE)
          {
              Value avg = rootMoves[pvIdx].averageScore;
              int variance = rootMoves[pvIdx].meanSquaredScore == -int(VALUE_INFINITE) * int(VALUE_INFINITE)
                           ? 0
                           : std::abs(rootMoves[pvIdx].meanSquaredScore) / 39605;
              delta = 10 + int(id() % 8) + variance;
              alpha = std::max(Value(avg - delta), -VALUE_INFINITE);
              beta  = std::min(Value(avg + delta),  VALUE_INFINITE);

              // Adjust trend based on root move's average score (dynamic contempt)
              int dynamicComp = cachedDynamicComp;
              int tr = dynamicComp * avg / (std::abs(int(avg)) + 147);

              trend = (us == WHITE ?  make_score(tr, tr / 2)
                                   : -make_score(tr, tr / 2));
          }
          else
          {
              alpha = -VALUE_INFINITE;
              beta = VALUE_INFINITE;
              delta = 17;
          }

          // Start with a small aspiration window and, in the case of a fail
          // high/low, re-search with a bigger window until we don't fail
          // high/low anymore.
          int failedHighCnt = 0;
          while (true)
          {
              Depth adjustedDepth = std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
              rootDelta = Value(std::max(1, int(beta - alpha)));
              bestValue = Stockfish::search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);

              // Bring the best move to the front. It is critical that sorting
              // is done with a stable algorithm because all the values but the
              // first and eventually the new best one are set to -VALUE_INFINITE
              // and we want to keep the same order for all the moves except the
              // new PV that goes to the front. Note that in case of MultiPV
              // search the already searched PV lines are preserved.
              std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

              // If search has been stopped, we break immediately. Sorting is
              // safe because RootMoves is still valid, although it refers to
              // the previous iteration.
              if (Threads.stop)
                  break;

              // When failing high/low give some update (without cluttering
              // the UI) before a re-search.
              if (   mainThread
                  && multiPV == 1
                  && (bestValue <= alpha || bestValue >= beta)
                  && Time.elapsed() > 3000)
                  sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;

              // In case of failing low/high increase aspiration window and
              // re-search, otherwise exit the loop.
              if (bestValue <= alpha)
              {
                  beta = alpha;
                  alpha = std::max(Value(bestValue - delta), -VALUE_INFINITE);

                  failedHighCnt = 0;
                  if (mainThread)
                      mainThread->stopOnPonderhit = false;
              }
              else if (bestValue >= beta)
              {
                  alpha = std::max(Value(beta - delta), alpha);
                  beta = std::min(Value(bestValue + delta), VALUE_INFINITE);
                  ++failedHighCnt;
              }
              else
                  break;

              delta += 44 * delta / 128;

              assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
          }

          // Sort the PV lines searched so far and update the GUI
          std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

          if (    mainThread
              && (Threads.stop || pvIdx + 1 == multiPV || Time.elapsed() > 3000))
              sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;
      }

      const bool forgottenMate = lastIterationScore != -VALUE_INFINITE
                              && decisive(lastIterationScore)
                              && (std::abs(int(rootMoves[0].score)) < std::abs(int(lastIterationScore))
                                  || rootMoves[0].score_is_bound());

      if (!Threads.stop)
      {
          completedDepth = rootDepth;

          if (lastIterationPV.empty() || rootMoves[0].pv[0] != lastIterationPV[0])
          {
              lastBestMoveDepth = rootDepth;
          }

          if (!forgottenMate)
          {
              lastIterationPV = rootMoves[0].pv;
              lastIterationScore = rootMoves[0].score;
          }
      }

      const bool abortedLossSearch = Threads.stop
                                  && !pvIdx
                                  && rootMoves[0].score != -VALUE_INFINITE
                                  && rootMoves[0].score <= VALUE_TB_LOSS_IN_MAX_PLY
                                  && !rootMoves[0].score_is_bound();

      if (abortedLossSearch || (rootMoves[0].score != -VALUE_INFINITE && forgottenMate))
      {
          if (!lastIterationPV.empty())
          {
              auto it = std::find(rootMoves.begin(), rootMoves.end(), lastIterationPV[0]);
              if (it != rootMoves.end())
                  std::rotate(rootMoves.begin(), it, it + 1);

              rootMoves[0].pv = lastIterationPV;
              rootMoves[0].score = rootMoves[0].uciScore = lastIterationScore;
              rootMoves[0].unset_bound_flags();
          }
          else if (abortedLossSearch)
              rootMoves[0].scoreLowerbound = true;
      }

      // Have we found a "mate in x"?
      if (   Limits.mate
          && !Threads.stop
          && decisive(rootMoves[0].score)
          && VALUE_MATE - std::abs(int(rootMoves[0].score)) <= 2 * Limits.mate)
          Threads.stop = true;

      if (!mainThread)
          continue;

      // If skill level is enabled and time is up, pick a sub-optimal best move
      if (skill.enabled() && skill.time_to_pick(rootDepth))
          skill.pick_best(multiPV);

      // Do we have time for the next iteration? Can we stop searching now?
      if (    Limits.use_time_management()
          && !Threads.stop
          && !mainThread->stopOnPonderhit)
      {
          uint64_t nodesEffort = rootMoves[0].effort * 100000
                               / std::max(uint64_t(1), Threads.nodes_searched());

          const int previousAverageDrop = int(mainThread->bestPreviousAverageScore) - int(bestValue);
          const int previousIterDrop    = int(mainThread->iterValue[iterIdx]) - int(bestValue);
          double fallingEval = (16.93 + 2.73 * previousAverageDrop
                                      + 0.80 * previousIterDrop) / 100.0;
          fallingEval = std::clamp(fallingEval, 0.610, 1.860);

          // If the bestMove is stable over several iterations, reduce time accordingly
          double stabilityDepth = double(rootDepth - lastBestMoveDepth);
          timeReduction = std::clamp(0.67 + (stabilityDepth - 8.0) * (1.44 - 0.67) / (17.0 - 8.0),
                                     0.67, 1.44);
          double reduction = (2.10 + mainThread->previousTimeReduction) / (2.480 * timeReduction);

          // Use part of the gained time from a previous stable move for the current move
          for (Thread* th : Threads)
          {
              totBestMoveChanges += th->bestMoveChanges;
              th->bestMoveChanges = 0;
          }
          double bestMoveInstability = 0.960 + 1.63 * totBestMoveChanges / Threads.size();
          double highBestMoveEffort = std::clamp(0.960 + (double(nodesEffort) - 78000.0)
                                                        * (0.740 - 0.960) / (94000.0 - 78000.0),
                                                 0.740, 0.960);
          double totalTime = Time.optimum() * fallingEval * reduction
                           * bestMoveInstability * highBestMoveEffort;

          // Cap used time in case of a single legal move for a better viewer experience in tournaments
          // yielding correct scores and sufficiently fast moves.
          if (rootMoves.size() == 1)
              totalTime = std::min(500.0, totalTime);

          TimePoint elapsed = Time.elapsed();

          if (rootMoves.size() == 1)
              Threads.stop = true;

          // Stop the search if we have exceeded the soft budget or hard maximum.
          if (!Threads.stop && elapsed > std::min(totalTime, double(Time.maximum())))
          {
              // If we are allowed to ponder do not stop the search now but
              // keep pondering until the GUI sends "ponderhit" or "stop".
              if (mainThread->ponder)
                  mainThread->stopOnPonderhit = true;
              else
                  Threads.stop = true;
          }
          else if (   Threads.increaseDepth
                   && !mainThread->ponder
                   && elapsed > totalTime * 0.58)
                   Threads.increaseDepth = false;
          else
                   Threads.increaseDepth = true;
      }

      mainThread->iterValue[iterIdx] = bestValue;
      iterIdx = (iterIdx + 1) & 3;
  }

  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;

  // If skill level is enabled, swap best PV line with the sub-optimal one
  if (skill.enabled())
      std::swap(rootMoves[0], *std::find(rootMoves.begin(), rootMoves.end(),
                skill.best ? skill.best : skill.pick_best(multiPV)));
}


namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType nodeType>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const bool allNode = !(PvNode || cutNode);
    const Depth maxNextDepth = rootNode ? depth : depth + 1;

    // Check if we have an upcoming move which draws by repetition, or
    // if the opponent had an alternative move earlier to this position.
    if (   !rootNode
        && pos.rule50_count() >= 3
        && alpha < VALUE_DRAW
        && pos.has_game_cycle(ss->ply))
    {
        alpha = value_draw(pos.this_thread(), pos.side_to_move());
        if (alpha >= beta)
            return alpha;
    }

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, excludedMove, bestMove;
    Depth extension, newDepth;
    Value bestValue, value, ttValue, eval, rawStaticEval, maxValue, probCutBeta;
    bool givesCheck, improving, opponentWorsening, didLMR, priorCapture, attackPhase, attackPhaseKnown, useAttackStyle;
    bool attackContext, attackContextKnown, overloadedDefendersKnown;
    bool captureOrPromotion, doFullDepthSearch, moveCountPruning,
         ttCapture, xiangqiAttack, sacContinuation, protectedAttack, simplificationCandidate;
    Piece movedPiece;
    int moveCount, captureCount, quietCount, priorReduction, moveCategory, attackPressure;
    int nodeAttackProtection, moveProtection, palaceThreat, overloadScore, simplificationPenalty;
    int cannonBatteryScore, sacrificeOfferLevel;
    Bitboard overloadedDefenders;

    // Step 1. Initialize node
    Thread* thisThread = pos.this_thread();
    useAttackStyle     = xiangqi_style_search_enabled(thisThread);
    attackPhase        = attackPhaseKnown = false;
    attackContext      = attackContextKnown = false;
    attackPressure     = -1;
    nodeAttackProtection = -1;
    palaceThreat      = overloadScore = -1;
    overloadedDefenders = Bitboard(0);
    overloadedDefendersKnown = false;
    ss->inCheck        = pos.checkers();
    priorCapture       = pos.captured_piece();
    Color us           = pos.side_to_move();
    moveCount          = captureCount = quietCount = ss->moveCount = 0;
    bestValue          = -VALUE_INFINITE;
    maxValue           = VALUE_INFINITE;

    auto getAttackPressure = [&]() {
        if (attackPressure < 0)
            attackPressure = xiangqi_attack_pressure(pos, thisThread->rootColor);
        return attackPressure;
    };

    auto hasAttackContext = [&]() {
        if (!attackContextKnown)
        {
            attackContext = useAttackStyle
                         && thisThread->rootColor < COLOR_NB
                         && pos.side_to_move() == thisThread->rootColor
                         && (   ss->sacrificeTrace >= 80
                             || xiangqi_root_material_loss(thisThread, ss->materialDiff) > 0
                             || xiangqi_guard_damage(pos, thisThread->rootColor) >= 40
                             || xiangqi_attack_shape(pos, thisThread->rootColor));
            attackContextKnown = true;
        }
        return attackContext;
    };

    auto getAttackPhase = [&]() {
        if (!attackPhaseKnown)
        {
            attackPhase = hasAttackContext()
                       && xiangqi_attack_phase(pos, thisThread, ss->materialDiff,
                                              getAttackPressure(), ss->sacrificeTrace);
            attackPhaseKnown = true;
        }
        return attackPhase;
    };

    auto getNodeAttackProtection = [&]() {
        if (nodeAttackProtection < 0)
            nodeAttackProtection = hasAttackContext()
                                 ? xiangqi_node_attack_protection(pos, thisThread, ss->materialDiff,
                                                                  getAttackPressure(), ss->sacrificeTrace)
                                 : 0;
        return nodeAttackProtection;
    };

    auto getPalaceThreat = [&]() {
        if (palaceThreat < 0)
            palaceThreat = hasAttackContext()
                         ? xiangqi_palace_threat(pos, thisThread->rootColor, getAttackPressure())
                         : 0;
        return palaceThreat;
    };

    auto getOverloadScore = [&]() {
        if (overloadScore < 0)
            overloadScore = depth >= 4 && hasAttackContext() && getAttackPressure() >= 80
                          ? xiangqi_defender_overload(pos, thisThread->rootColor)
                          : 0;
        return overloadScore;
    };

    auto getOverloadedDefenders = [&]() {
        if (!overloadedDefendersKnown)
        {
            overloadedDefenders = depth >= 4 && hasAttackContext() && getAttackPressure() >= 80
                                ? xiangqi_overloaded_defenders(pos, thisThread->rootColor)
                                : Bitboard(0);
            overloadedDefendersKnown = true;
        }
        return overloadedDefenders;
    };

    if (useAttackStyle)
        ss->sacrificeTrace = std::max(ss->sacrificeTrace,
                                      xiangqi_position_sacrifice_trace(pos, thisThread, ss->materialDiff));

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    if (!rootNode)
    {
        Value variantResult;
        if (pos.is_game_end(variantResult, ss->ply))
            return variantResult;

        // Step 2. Check for aborted search and immediate draw
        if (   Threads.stop.load(std::memory_order_relaxed)
            || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos)
                                                        : value_draw(pos.this_thread(), us);

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply+1), beta);
        if (alpha >= beta)
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->ttPv         = false;
    (ss+1)->excludedMove = bestMove = MOVE_NONE;
    (ss+2)->killers[0]   = (ss+2)->killers[1] = MOVE_NONE;
    ss->doubleExtensions = (ss-1)->doubleExtensions;
    Square prevSq        = to_sq((ss-1)->currentMove);
    priorReduction       = (ss-1)->reduction;
    (ss-1)->reduction    = 0;
    ss->statScore        = 0;
    (ss+2)->cutoffCnt    = 0;

    // Initialize statScore to zero for the grandchildren of the current position.
    // So statScore is shared between all grandchildren and only the first grandchild
    // starts with statScore = 0. Later grandchildren start with the last calculated
    // statScore of the previous grandchild. This influences the reduction rules in
    // LMR which are based on the statScore of parent position.
    if (!rootNode)
        (ss+2)->statScore = 0;

    // Step 4. Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = excludedMove == MOVE_NONE ? pos.key() : pos.key() ^ make_key(excludedMove);
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
            : ss->ttHit    ? tte->move() : MOVE_NONE;
    if (!excludedMove)
        ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());
    ttCapture = ttMove && pos.capture_or_promotion(ttMove);

    int correctionValue = correction_value(thisThread, pos, ss);

    // Update low ply history for previous move if we are near root and position is or has been in PV
    if (   ss->ttPv
        && depth > 12
        && ss->ply - 1 < MAX_LPH
        && !priorCapture
        && is_ok((ss-1)->currentMove))
        thisThread->lowPlyHistory[ss->ply - 1][history_move((ss-1)->currentMove)] << stat_bonus(depth - 5);

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ss->ttHit
        && tte->depth() > depth - (ttValue <= beta)
        && ttValue != VALUE_NONE // Possible in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER))
        && (cutNode == (ttValue >= beta) || depth > 5))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                // Bonus for a quiet ttMove that fails high
                if (!pos.capture_or_promotion(ttMove))
                    update_quiet_stats(pos, ss, ttMove, stat_bonus(depth), depth);

                // Extra penalty for early quiet moves of the previous ply
                if ((ss-1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            }
            // Penalty for a quiet ttMove that fails low
            else if (!pos.capture_or_promotion(ttMove))
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory[us][history_move(ttMove)] << penalty;
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }

        // Partial workaround for the graph history interaction problem.
        // For high n-move rule counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 90)
        {
            if (depth >= 10
                && ttMove
                && pos.pseudo_legal(ttMove)
                && pos.legal(ttMove)
                && !decisive(ttValue))
            {
                pos.do_move(ttMove, st);
                bool ttHitNext;
                TTEntry* tteNext = TT.probe(pos.key(), ttHitNext);
                Value ttValueNext = ttHitNext ? value_from_tt(tteNext->value(), ss->ply + 1,
                                                              pos.rule50_count())
                                               : VALUE_NONE;
                pos.undo_move(ttMove);

                if (ttValueNext == VALUE_NONE)
                    return ttValue;

                if ((ttValue >= beta) == (-ttValueNext >= beta))
                    return ttValue;
            }
            else
                return ttValue;
        }
    }
    else if (  !PvNode
            && ss->ttHit
            && tte->depth() > depth - (ttValue <= beta)
            && ttValue != VALUE_NONE
            && tte->bound() != BOUND_EXACT
            && (tte->bound() & (ttValue >= beta ? BOUND_UPPER : BOUND_LOWER))
            && depth > 5)
        tte->penalize(1);

    CapturePieceToHistory& captureHistory = thisThread->captureHistory;

    // Step 5. Static evaluation of the position
    rawStaticEval = VALUE_NONE;
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = VALUE_NONE;
        improving = false;
        goto moves_loop;
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        rawStaticEval = tte->eval();
        if (rawStaticEval == VALUE_NONE)
            rawStaticEval = evaluate(pos);

        ss->staticEval = eval = to_corrected_static_eval(rawStaticEval, correctionValue);

        // Randomize draw evaluation
        if (eval == VALUE_DRAW)
            eval = value_draw(thisThread, us);

        // Can ttValue be used as a better position evaluation?
        if (    ttValue != VALUE_NONE
            && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {
        // In case of null move search use previous static eval with a different sign
        // and addition of two tempos
        if ((ss-1)->currentMove != MOVE_NULL)
            rawStaticEval = evaluate(pos);
        else
            rawStaticEval = -(ss-1)->staticEval;

        ss->staticEval = eval = to_corrected_static_eval(rawStaticEval, correctionValue);

        // Save static evaluation into transposition table
        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, rawStaticEval);
    }

    // Use static evaluation difference to improve quiet move ordering
    if (is_ok((ss-1)->currentMove) && !(ss-1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-depth * 4 * int((ss-1)->staticEval + ss->staticEval), -1000, 1000);
        thisThread->mainHistory[~us][history_move((ss-1)->currentMove)] << bonus;
    }

    // Set up improving flag that is used in various pruning heuristics
    // We define position as improving if static evaluation of position is better
    // Than the previous static evaluation at our turn
    // In case of us being in check at our previous move we look at move prior to it
    improving =  (ss-2)->staticEval == VALUE_NONE
               ? ss->staticEval > (ss-4)->staticEval || (ss-4)->staticEval == VALUE_NONE
               : ss->staticEval > (ss-2)->staticEval;
    opponentWorsening = (ss-1)->staticEval != VALUE_NONE
                     && ss->staticEval > -(ss-1)->staticEval;

    if (priorReduction >= 3 && !opponentWorsening)
        depth++;

    if (priorReduction >= 2
        && depth >= 2
        && (ss-1)->staticEval != VALUE_NONE
        && ss->staticEval + (ss-1)->staticEval > 193)
        depth--;

    // Patricia-style sacrifice detection from the original root side's perspective.
    {
        int sacDetect = thisThread->cachedSacDetect;
        if (sacDetect > 0 && !ss->inCheck && ss->ply >= 4 && eval != VALUE_NONE)
        {
            int totalMat = xiangqi_total_material(pos);
            int rootMaterialLoss = std::max(0, thisThread->rootMaterialBalance - ss->materialDiff);
            int sacrificeTrace = ss->sacrificeTrace;
            if (totalMat > 2000 && rootMaterialLoss > 0 && sacrificeTrace >= 70)
            {
                int pressure = getAttackPressure();
                int quality = xiangqi_sacrifice_quality(pos, thisThread->rootColor, rootMaterialLoss, pressure);
                int required = xiangqi_required_sacrifice_quality(rootMaterialLoss);
                int bonus = sacDetect * std::min(rootMaterialLoss, 1200) / 260;
                bonus += sacDetect * std::min(sacrificeTrace, 360) / 520;
                bonus += thisThread->cachedDynamicComp * std::min(quality, 420) / 900;

                if (quality < required)
                {
                    bonus = bonus * std::max(quality, 1) / required;
                    if (quality < required / 2)
                        bonus /= 2;
                }
                else
                    bonus += sacDetect * std::min(quality - required, 240) / 420;

                int rootEval = us == thisThread->rootColor ? int(eval) : -int(eval);

                if (rootEval > 500)
                    bonus *= 2;
                else if (rootEval < -250)
                    bonus /= 2;

                eval = Value(std::clamp(int(eval) + (us == thisThread->rootColor ? bonus : -bonus),
                                        int(VALUE_TB_LOSS_IN_MAX_PLY + 1),
                                        int(VALUE_TB_WIN_IN_MAX_PLY - 1)));
            }
        }
    }

    // Skip early pruning in case of mandatory capture
    if (XiangqiHasMandatoryCapture && pos.must_capture() && pos.has_capture())
        goto moves_loop;

    // Pikafish-style razoring: very low static eval falls back to qsearch.
    if (   !PvNode
        &&  eval < alpha - 1370 - 244 * depth * depth - 4 * getNodeAttackProtection())
        return qsearch<NonPV>(pos, ss, alpha, beta);

    // Step 7. Futility pruning: child node (~50 Elo)
    if (   !ss->ttPv
        &&  depth < 15
        &&  eval >= beta
        && (!ttMove || ttCapture)
        && !decisive(beta)
        && !decisive(eval))
    {
        Value futilityMult = Value(40 + 89 * std::min(int(depth), 10) / 10);
        futilityMult -= 33 * !ss->ttHit;

        Value futilityMargin = futilityMult * depth
                             - (2512 * improving + 340 * opponentWorsening) * futilityMult / 1024
                             + std::abs(correctionValue) / 132109;

        if (eval - futilityMargin >= beta)
            return Value((716 * beta + 308 * eval) / 1024);
    }

    // Step 8. Null move search with verification search (~40 Elo)
    if (   !PvNode
        && (ss-1)->currentMove != MOVE_NULL
        && (ss-1)->statScore < 23767
        &&  eval >= beta
        &&  eval >= ss->staticEval
        &&  ss->staticEval >= beta - 20 * depth - 22 * improving + 168 * ss->ttPv + 159
        && !excludedMove
        &&  pos.non_pawn_material(us)
        &&  pos.count<ALL_PIECES>(~us) != pos.count<PAWN>(~us)
        &&  getNodeAttackProtection() < 160
        && (ss->ply >= thisThread->nmpMinPly || us != thisThread->nmpColor))
    {
        assert(eval - beta >= 0);

        // Null move dynamic reduction based on depth and value
        int attackProtection = getNodeAttackProtection();
        Depth R = 8 + depth / 3;
        if (attackProtection)
            R = std::max(3, R - attackProtection / 96);

        ss->currentMove = MOVE_NULL;
        ss->continuationHistory = &thisThread->continuationHistory[0][0][history_slot(NO_PIECE)][0];
        ss->continuationCorrectionHistory = &thisThread->continuationCorrectionHistory[history_slot(NO_PIECE)][0];

        pos.do_null_move(st);

        (ss+1)->materialDiff = ss->materialDiff;
        (ss+1)->sacrificeTrace = std::max(0, ss->sacrificeTrace - 6);

        Value nullValue = -search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode);

        pos.undo_null_move();

        if (nullValue >= beta)
        {
            // Do not return unproven mate or TB scores
            if (nullValue >= VALUE_TB_WIN_IN_MAX_PLY)
                nullValue = beta;

            if (thisThread->nmpMinPly || (abs(beta) < VALUE_KNOWN_WIN && depth < 14))
                return nullValue;

            assert(!thisThread->nmpMinPly); // Recursive verification is not allowed

            // Do verification search at high depths, with null move pruning disabled
            // for us, until ply exceeds nmpMinPly.
            thisThread->nmpMinPly = ss->ply + 3 * (depth-R) / 4;
            thisThread->nmpColor = us;

            Value v = search<NonPV>(pos, ss, beta-1, beta, depth-R, false);

            thisThread->nmpMinPly = 0;

            if (v >= beta)
                return nullValue;
        }
    }

    probCutBeta = beta + 209 - 44 * improving + getNodeAttackProtection() / 4;

    // Step 9. ProbCut (~4 Elo)
    // If we have a good enough capture and a reduced search returns a value
    // much above beta, we can (almost) safely prune the previous move.
    if (   !PvNode
        &&  depth > 4
        &&  abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        // if value from transposition table is lower than probCutBeta, don't attempt probCut
        // there and in further interactions with transposition table cutoff depth is set to depth - 3
        // because probCut search has depth set to depth - 4 but we also do a move before it
        // so effective depth is equal to depth - 3
        && !(   ss->ttHit
             && tte->depth() >= depth - 3
             && ttValue != VALUE_NONE
             && ttValue < probCutBeta))
    {
        assert(probCutBeta < VALUE_INFINITE);

        MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);
        int probCutCount = 0;
        bool ttPv = ss->ttPv;
        ss->ttPv = false;

        while (   (move = mp.next_move()) != MOVE_NONE
               && probCutCount < 2 + 2 * cutNode)
            if (move != excludedMove && pos.legal(move))
            {
                assert(pos.capture_or_promotion(move));
                assert(depth >= 5);

                captureOrPromotion = true;
                probCutCount++;

                ss->currentMove = move;
                ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                          [captureOrPromotion]
                                                                          [history_slot(pos.moved_piece(move))]
                                                                          [history_square(to_sq(move))];
                ss->continuationCorrectionHistory =
                    &thisThread->continuationCorrectionHistory[history_slot(pos.moved_piece(move))][history_square(to_sq(move))];

                bool probCutGivesCheck = pos.gives_check(move);
                int nextMaterialBalance = xiangqi_next_material_balance(pos, thisThread, ss->materialDiff, move);
                (ss+1)->materialDiff = nextMaterialBalance;
                (ss+1)->sacrificeTrace = xiangqi_next_sacrifice_trace(pos, thisThread, ss, move,
                                                                       probCutGivesCheck, nextMaterialBalance);

                pos.do_move(move, st);

                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1);

                // If the qsearch held, perform the regular search
                if (value >= probCutBeta)
                    value = -search<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1, depth - 4, !cutNode);

                pos.undo_move(move);

                if (value >= probCutBeta)
                {
                    // if transposition table doesn't have equal or more deep info write probCut data into it
                    if ( !(ss->ttHit
                       && tte->depth() >= depth - 3
                       && ttValue != VALUE_NONE))
                         tte->save(posKey, value_to_tt(value, ss->ply), ttPv,
                             BOUND_LOWER,
                             depth - 3, move, rawStaticEval);
                    return !decisive(value) ? Value(value - (probCutBeta - beta))
                                            : value;
                }
            }
         ss->ttPv = ttPv;
    }

    // Step 10. If the position is not in TT, decrease depth by 2
    if (   depth >= 6
        && !ttMove
        && (PvNode || cutNode))
        depth -= 1;

moves_loop: // When in check, search starts from here

    ttCapture = ttMove && pos.capture_or_promotion(ttMove);

    // Step 11. A small Probcut idea, when we are in check
    probCutBeta = beta + 409;
    if (   ss->inCheck
        && !PvNode
        && depth >= 4
        && ttCapture
        && (tte->bound() & BOUND_LOWER)
        && tte->depth() >= depth - 3
        && ttValue >= probCutBeta
        && abs(ttValue) <= VALUE_KNOWN_WIN
        && abs(beta) <= VALUE_KNOWN_WIN
       )
        return probCutBeta;


    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    Move countermove = thisThread->counterMoves[history_slot(pos.piece_on(prevSq))][history_square(prevSq)];
    int attackBias = xiangqi_attack_bias(thisThread);
    int sideAttackBias = thisThread->rootColor < COLOR_NB && us == thisThread->rootColor ? attackBias : 0;

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->lowPlyHistory,
                                      &captureHistory,
                                      contHist,
                                      countermove,
                                      ss->killers,
                                      ss->ply,
                                      &thisThread->pawnHistory,
                                      attackBias ? &thisThread->attackHistory : nullptr,
                                      attackBias,
                                      thisThread->rootColor);

    value = bestValue;
    moveCountPruning = false;

    // Step 12. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched and those
      // of lower "TB rank" if we are in a TB root position.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                                  thisThread->rootMoves.begin() + thisThread->pvLast, move))
          continue;

      // Check for legality
      if (!rootNode && !pos.legal(move))
          continue;

      ss->moveCount = ++moveCount;

      if (rootNode
          && thisThread == Threads.main()
          && thisThread->nodes.load(std::memory_order_relaxed) > NODES_LIMIT_OUTPUT)
          sync_cout << "info depth " << depth
                    << " currmove " << UCI::move(pos, move)
                    << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;
      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = 0;
      captureOrPromotion = pos.capture_or_promotion(move);
      movedPiece = pos.moved_piece(move);
      givesCheck = pos.gives_check(move);
      xiangqiAttack = sacContinuation = protectedAttack = simplificationCandidate = false;
      moveCategory = XMC_NONE;
      moveProtection = 0;
      simplificationPenalty = 0;
      cannonBatteryScore = sacrificeOfferLevel = 0;

      if (useAttackStyle)
      {
          bool rootMove = pos.side_to_move() == thisThread->rootColor;
          xiangqiAttack = rootMove ? xiangqi_attacking_move(pos, move, givesCheck)
                                   : givesCheck;
          sacContinuation = xiangqi_sacrifice_continuation(pos, thisThread, move, givesCheck,
                                                           ss->materialDiff);
          moveCategory = xiangqi_move_category(pos, thisThread, move, givesCheck, sacContinuation);
          if (rootMove)
          {
              Bitboard overloaded = Bitboard(0);
              int moveThreat = xiangqi_move_palace_threat_score(pos, thisThread->rootColor, move,
                                                                givesCheck, overloaded);
              cannonBatteryScore = xiangqi_cannon_battery_score(pos, thisThread->rootColor, move,
                                                                givesCheck, overloaded);
              if (cannonBatteryScore >= 95)
                  moveCategory |= XMC_CANNON_BATTERY;
              moveThreat = std::max(moveThreat, cannonBatteryScore);

              if (moveThreat >= 75 || sacContinuation || (moveCategory & XMC_ATTACK_MASK))
              {
                  overloaded = getOverloadedDefenders();
                  if (overloaded)
                  {
                      moveThreat = xiangqi_move_palace_threat_score(pos, thisThread->rootColor, move,
                                                                    givesCheck, overloaded);
                      cannonBatteryScore = xiangqi_cannon_battery_score(pos, thisThread->rootColor, move,
                                                                        givesCheck, overloaded);
                      if (cannonBatteryScore >= 95)
                          moveCategory |= XMC_CANNON_BATTERY;
                      moveThreat = std::max(moveThreat, cannonBatteryScore);
                      if (xiangqi_move_attacks_targets(pos, thisThread->rootColor, move, overloaded))
                          moveCategory |= XMC_OVERLOAD_ATTACK;
                  }
              }

              if (moveThreat >= 120 || (moveThreat >= 85 && moveThreat + getPalaceThreat() / 3 >= 120))
                  moveCategory |= XMC_UNANSWERABLE_THREAT;

              sacrificeOfferLevel = xiangqi_sacrifice_offer_level(pos, thisThread->rootColor, move,
                                                                  moveCategory, getAttackPressure());
              if (sacrificeOfferLevel)
                  moveCategory |= XMC_SACRIFICE_OFFER;
          }
          moveProtection = rootMove ? xiangqi_move_search_protection(moveCategory, attackBias) : 0;
          bool currentAttackPhase = getAttackPhase();
          if (captureOrPromotion)
          {
              int pressure = getAttackPressure();
              simplificationCandidate = xiangqi_simplification_candidate(pos, thisThread, move, givesCheck,
                                                                         moveCategory, ss->staticEval,
                                                                         currentAttackPhase, pressure,
                                                                         ss->materialDiff);

              Piece captured = pos.piece_on(to_sq(move));
              if (captured && xiangqi_trade_piece(type_of(movedPiece)) && xiangqi_trade_piece(type_of(captured)))
                  simplificationPenalty = xiangqi_simplification_penalty(pos, thisThread, move, moveCategory,
                                                                         currentAttackPhase, pressure,
                                                                         getPalaceThreat(), getOverloadScore(),
                                                                         ss->materialDiff);
          }
          if (simplificationCandidate)
              moveCategory |= XMC_SIMPLIFICATION;
          if (simplificationPenalty > 0)
              simplificationCandidate = true;
          protectedAttack = rootMove ? (xiangqiAttack || moveProtection) : givesCheck;
      }

      // Calculate new depth for this move
      newDepth = depth - 1;

      // Step 13. Pruning at shallow depth (~200 Elo)
      if (  !rootNode
          && (pos.non_pawn_material(us) || pos.count<ALL_PIECES>(us) == pos.count<PAWN>(us))
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
      {
          if (moveCount >= (3 + depth * depth) / (2 - improving)
                         + 2 * protectedAttack + std::min(4, moveProtection / 90) + getAttackPhase())
              moveCountPruning = true;

          int r1024 = pikafish_reduction(thisThread, improving, depth, moveCount, beta - alpha);
          int lmrDepth = newDepth - r1024 / 1024;

          if (captureOrPromotion || givesCheck)
          {
              Piece captured = pos.piece_on(to_sq(move));
              int captHist = captureHistory[history_slot(movedPiece)][history_square(to_sq(move))][capture_history_slot(type_of(captured))];

              if (!givesCheck && !protectedAttack && lmrDepth < 19)
              {
                  Value futilityValue = ss->staticEval + 322 + 336 * lmrDepth
                                      + PieceValue[MG][captured] + 229 * captHist / 1024;

                  if (futilityValue <= alpha)
                      continue;
              }

              int margin = std::max(256 * depth + captHist * 34 / 1024, 0);
              if (protectedAttack)
                  margin += 128 + 2 * sideAttackBias + 64 * sacContinuation + moveProtection;
              else if (simplificationCandidate)
                  margin = std::max(0, margin - 192 - simplificationPenalty / 2);
              if (!pos.see_ge(move, Value(-margin)))
                  continue;
          }
          else
          {
              int history = (*contHist[0])[history_slot(movedPiece)][history_square(to_sq(move))]
                          + (*contHist[1])[history_slot(movedPiece)][history_square(to_sq(move))]
                          + thisThread->pawnHistory[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)][history_slot(movedPiece)][history_square(to_sq(move))];

              if (!protectedAttack && history < -2995 * depth)
                  continue;

              history += 73 * thisThread->mainHistory[us][history_move(move)] / 32;

              int dIndex = std::min(int(depth), 16) - 1;
              lmrDepth += history / LmrDivisor[dIndex];

              Value futilityValue = ss->staticEval + 47 + 272 * !bestMove + 129 * lmrDepth
                                  + 112 * (ss->staticEval > alpha);

              if (!ss->inCheck && !protectedAttack && lmrDepth < 10 && futilityValue <= alpha)
              {
                  if (bestValue <= futilityValue && !decisive(bestValue) && futilityValue < VALUE_KNOWN_WIN)
                      bestValue = futilityValue;
                  continue;
              }

              lmrDepth = std::max(lmrDepth, 0);

              int seeMargin = 35 * lmrDepth * lmrDepth
                            + (protectedAttack ? 96 + 64 * sacContinuation + moveProtection / 3 : 0);
              if (!pos.see_ge(move, Value(-seeMargin)))
                  continue;
          }
      }

      // Step 14. Extensions (~75 Elo)

      // Singular extension search (~70 Elo). If all moves but one fail low on a
      // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
      // then that move is singular and should be extended. To verify this we do
      // a reduced search on all the other moves but the ttMove and if the
      // result is lower than ttValue minus a margin, then we will extend the ttMove.
      if (   !rootNode
          &&  move == ttMove
          && !excludedMove // Avoid recursive singular search
       /* &&  ttValue != VALUE_NONE Already implicit in the next condition */
          &&  abs(ttValue) < VALUE_KNOWN_WIN
          && (tte->bound() & BOUND_LOWER)
          &&  tte->depth() >= depth - 3
          &&  depth >= 5 + ss->ttPv
          && !is_shuffling(move, ss, pos))
      {
          Value singularBeta = ttValue - (44 + 72 * (ss->ttPv && !PvNode)) * depth / 69;
          Depth singularDepth = newDepth / 2;

          ss->excludedMove = move;
          value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
          ss->excludedMove = MOVE_NONE;

          if (value < singularBeta)
          {
              int corrValAdj = std::abs(correctionValue) / 265845;
              int doubleMargin = -4 + 234 * PvNode - 172 * !ttCapture - corrValAdj
                               - 1085 * int(thisThread->ttMoveHistory) / 133615
                               - (ss->ply > thisThread->rootDepth) * 43;
              int tripleMargin = 106 + 299 * PvNode - 263 * !ttCapture + 93 * ss->ttPv
                               - corrValAdj - (ss->ply > thisThread->rootDepth) * 60;

              extension = 1 + (value < singularBeta - doubleMargin)
                            + (value < singularBeta - tripleMargin);

              if (extension >= 2 && ss->doubleExtensions >= 3)
                  extension = 1;

          }

          // Multi-cut pruning
          // Our ttMove is assumed to fail high, and now we failed high also on a reduced
          // search without the ttMove. So we assume this expected Cut-node is not singular,
          // that multiple moves fail high, and we can prune the whole subtree by returning
          // a soft bound.
          else if (value >= beta && !decisive(value))
          {
              thisThread->ttMoveHistory << -397 - 103 * depth;
              return value;
          }

          // If the eval of ttMove is greater than beta we try also if there is another
          // move that pushes it over beta, if so also produce a cutoff.
          else if (ttValue >= beta)
              extension = -3;
          else if (cutNode)
              extension = -2;
      }
      else if (   givesCheck
               && depth > 6 - std::min(2, sideAttackBias / 80)
                           - getAttackPhase()
               && abs(ss->staticEval) > Value(100))
          extension = 1;
      else if (   protectedAttack
               && sacContinuation
               && getAttackPhase()
               && depth >= 5
               && attack_category_weight(moveCategory) >= 168)
          extension = 1;

      // Losing chess capture extension
      else if (    XiangqiHasMandatoryCapture
               &&  pos.must_capture()
               &&  pos.capture(move)
               &&  (ss->inCheck || MoveList<CAPTURES>(pos).size() == 1))
          extension = 1;

      // Add extension to new depth
      newDepth += extension;
      ss->doubleExtensions = (ss-1)->doubleExtensions + (extension >= 2);

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [captureOrPromotion]
                                                                [history_slot(movedPiece)]
                                                                [history_square(to_sq(move))];
      ss->continuationCorrectionHistory =
          &thisThread->continuationCorrectionHistory[history_slot(movedPiece)][history_square(to_sq(move))];

      uint64_t nodeCount = rootNode ? thisThread->nodes.load(std::memory_order_relaxed) : 0;

      // Step 15. Make the move
      int nextMaterialBalance = xiangqi_next_material_balance(pos, thisThread, ss->materialDiff, move);
      (ss+1)->materialDiff = nextMaterialBalance;
      (ss+1)->sacrificeTrace = xiangqi_next_sacrifice_trace(pos, thisThread, ss, move,
                                                            givesCheck, nextMaterialBalance);
      pos.do_move(move, st, givesCheck);

      // Step 16. Late moves reduction / extension (LMR, ~200 Elo)
      // We use various heuristics for the sons of a node after the first son has
      // been searched. In general we would like to reduce them, but there are many
      // cases where we extend a son if it has good chances to be "interesting".
      if (    depth >= 3
          &&  moveCount > 1 + 2 * rootNode
          && !(XiangqiHasMandatoryCapture && pos.must_capture() && pos.has_capture())
          && (  !captureOrPromotion
              || (cutNode && (ss-1)->moveCount > 1)
              || !ss->ttPv)
          && (!PvNode || ss->ply > 1 || thisThread->id() % 4 != 3))
      {
          int r = pikafish_reduction(thisThread, improving, depth, moveCount, beta - alpha);

          if (ss->ttPv)
              r -= 2363 + PvNode * 963 + (ttValue > alpha) * 1121
                 + (tte->depth() >= depth) * (1137 + cutNode * 922);

          r += 855;
          r -= moveCount * 64;
          r -= std::abs(correctionValue) / 30558;

          int aggressiveness = (us == thisThread->rootColor) ? thisThread->cachedAggressiveness : 100;
          if (protectedAttack)
              r -= 192 + 512 * sideAttackBias / 200 + 2 * attack_category_weight(moveCategory) + moveProtection / 2;
          else if (aggressiveness > 100 && (givesCheck || captureOrPromotion))
              r -= 512 * (aggressiveness - 100) / 100;

          if (simplificationCandidate)
              r += 768 + 256 * getAttackPhase() + 2 * simplificationPenalty;

          if (cutNode)
              r += 3251 + 1048 * !ttMove;

          if (ttCapture)
              r += 1571;

          if ((ss+1)->cutoffCnt > 1)
              r += 256 + 1024 * ((ss+1)->cutoffCnt > 2) + 1024 * allNode;
          else if (move == ttMove)
              r = std::max(-10, r - 2730 + 150 * cutNode);

          if (captureOrPromotion)
              ss->statScore = 953 * int(PieceValue[MG][pos.captured_piece()]) / 128
                            + captureHistory[history_slot(movedPiece)][history_square(to_sq(move))][capture_history_slot(type_of(pos.captured_piece()))];
          else
              ss->statScore = 2 * thisThread->mainHistory[us][history_move(move)]
                            + (*contHist[0])[history_slot(movedPiece)][history_square(to_sq(move))]
                            + (*contHist[1])[history_slot(movedPiece)][history_square(to_sq(move))]
                            + thisThread->pawnHistory[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)][history_slot(movedPiece)][history_square(to_sq(move))];

          r -= ss->statScore * 946 / 8192;

          if (allNode)
              r += r * 256 / (256 * depth + 256);

          Depth d = std::max(1, std::min(newDepth - r / 1024, newDepth + 2)) + PvNode;

          ss->reduction = newDepth - d;
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true);
          ss->reduction = 0;

          if (value > alpha)
          {
              const bool doDeeperSearch = d < newDepth && value > bestValue + 60;
              const bool doShallowerSearch = value < bestValue + 9;

              newDepth += doDeeperSearch - doShallowerSearch;

              if (newDepth > d)
                  value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

              if (!captureOrPromotion)
                  update_continuation_histories(ss, movedPiece, to_sq(move), 1528);
          }

          doFullDepthSearch = false;
          didLMR = true;
      }
      else
      {
          doFullDepthSearch = !PvNode || moveCount > 1;
          didLMR = false;
      }

      // Step 17. Full depth search when LMR is skipped or fails high
      if (doFullDepthSearch)
      {
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

          // If the move passed LMR update its stats
          if (didLMR && !captureOrPromotion)
          {
              int bonus = value > alpha ?  stat_bonus(newDepth)
                                        : -stat_bonus(newDepth);

              update_continuation_histories(ss, movedPiece, to_sq(move), bonus);
          }
      }

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;

          value = -search<PV>(pos, ss+1, -beta, -alpha,
                              std::min(maxNextDepth, newDepth), false);
      }

      // Step 18. Undo move
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 19. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
          return VALUE_ZERO;

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);

          rm.effort += thisThread->nodes.load(std::memory_order_relaxed) - nodeCount;
          rm.averageScore = rm.averageScore != -VALUE_INFINITE ? Value((int(value) + int(rm.averageScore)) / 2)
                                                               : value;
          rm.meanSquaredScore = rm.meanSquaredScore != -int(VALUE_INFINITE) * int(VALUE_INFINITE)
                              ? (int(value) * std::abs(int(value)) + rm.meanSquaredScore) / 2
                              : int(value) * std::abs(int(value));

          // PV move or new best move?
          if (moveCount == 1 || value > alpha)
          {
              rm.score = rm.uciScore = value;
              rm.unset_bound_flags();

              if (value >= beta)
              {
                  rm.scoreLowerbound = true;
                  rm.uciScore = beta;
              }
              else if (value <= alpha)
              {
                  rm.scoreUpperbound = true;
                  rm.uciScore = alpha;
              }

              rm.selDepth = thisThread->selDepth;
              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each
              // iteration. This information is used for time management and LMR
              if (moveCount > 1 && !thisThread->pvIdx)
                  ++thisThread->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
              // is not a problem when sorting because the sort is stable and the
              // move position in the list is preserved - just the PV is pushed up.
              rm.score = -VALUE_INFINITE;
      }

      int inc = rootNode
             && value == bestValue
             && ss->ply + 2 >= thisThread->rootDepth
             && (thisThread->nodes.load(std::memory_order_relaxed) & 15) == 0
             && !decisive(Value(std::abs(int(value)) + 1));

      if (value + inc > bestValue)
      {
          bestValue = value;

          if (value + inc > alpha)
          {
              bestMove = move;

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha! Always alpha < beta
              {
                  if (depth > 2 && depth < 11 && !decisive(value))
                      depth -= 2;

                  alpha = value;
              }
              else
              {
                  ss->cutoffCnt += (extension < 2) || PvNode;

                  assert(value >= beta); // Fail high
                  break;
              }
          }
      }

      // If the move is worse than some previously searched move, remember it to update its stats later
      if (move != bestMove)
      {
          if (captureOrPromotion && captureCount < 32)
              capturesSearched[captureCount++] = move;

          else if (!captureOrPromotion && quietCount < 64)
              quietsSearched[quietCount++] = move;
      }
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 20. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (bestValue >= beta && !decisive(bestValue) && !decisive(alpha))
        bestValue = Value((bestValue * depth + beta) / (depth + 1));

    if (!moveCount)
        bestValue = excludedMove ? alpha :
                    ss->inCheck  ? pos.checkmate_value(ss->ply)
                                 : pos.stalemate_value(ss->ply);

    // If there is a move which produces search value greater than alpha we update stats of searched moves
    else if (bestMove)
    {
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq,
                         quietsSearched, quietCount, capturesSearched, captureCount, depth);

        if (!PvNode)
            thisThread->ttMoveHistory << (bestMove == ttMove ? 796 : -855);
    }

    // Bonus for prior quiet countermove that caused the fail low
    else if (!priorCapture && is_ok((ss-1)->currentMove))
    {
        int bonusScale = -231;
        bonusScale -= (ss-1)->statScore / 73;
        bonusScale += std::min(62 * depth, 512);
        bonusScale += 152 * ((ss-1)->moveCount > 13);
        bonusScale += 76 * (!ss->inCheck && bestValue <= ss->staticEval - 166);
        bonusScale += 163 * (!(ss-1)->inCheck && bestValue <= -(ss-1)->staticEval - 109);
        bonusScale = std::max(bonusScale, 0);

        const int scaledBonus = std::min(148 * depth - 86, 2188) * bonusScale;
        Piece prevPc = pos.piece_on(prevSq);

        update_continuation_histories(ss-1, prevPc, prevSq, scaledBonus * 192 / 16384);
        thisThread->mainHistory[~us][history_move((ss-1)->currentMove)] << scaledBonus * 216 / 32768;

        if (type_of(prevPc) != PAWN && type_of(prevPc) != SOLDIER)
            thisThread->pawnHistory[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)][history_slot(prevPc)][history_square(prevSq)]
                << scaledBonus * 244 / 8192;
    }

    // Bonus for prior capture countermove that caused the fail low
    else if (priorCapture && is_ok((ss-1)->currentMove))
    {
        Piece capturedPiece = pos.captured_piece();
        if (capturedPiece)
            captureHistory[history_slot(pos.piece_on(prevSq))][history_square(prevSq)][capture_history_slot(type_of(capturedPiece))] << 983;
    }

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss-1)->ttPv && depth > 3);
    // Otherwise, a counter move has been found and if the position is the last leaf
    // in the search tree, remove the position from the search tree.
    else if (depth > 3)
        ss->ttPv = ss->ttPv && (ss+1)->ttPv;

    // Write gathered information in transposition table
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, rawStaticEval);

    if (   !ss->inCheck
        && !(bestMove && pos.capture_or_promotion(bestMove))
        && (bestValue > ss->staticEval) == bool(bestMove))
    {
        int bonus = std::clamp(int(bestValue - ss->staticEval) * depth * (bestMove ? 12 : 17) / 128,
                               -CORRECTION_HISTORY_LIMIT / 4,
                                CORRECTION_HISTORY_LIMIT / 4);
        update_correction_history(pos, ss, thisThread, 1069 * bonus / 1024);
    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main search
  // function with zero depth, or recursively with further decreasing depth per call.
  template <NodeType nodeType>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= 0);

    Move pv[MAX_PLY+1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Depth ttDepth;
    Value bestValue, value, ttValue, rawStaticEval, futilityValue, futilityBase, oldAlpha;
    bool pvHit, givesCheck, captureOrPromotion, useAttackStyle, protectedAttack, sacContinuation;
    int moveCount, moveCategory;

    if (PvNode)
    {
        oldAlpha = alpha; // To flag BOUND_EXACT when eval above alpha and no available moves
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Thread* thisThread = pos.this_thread();
    bestMove = MOVE_NONE;
    ss->inCheck = pos.checkers();
    moveCount = 0;
    useAttackStyle = xiangqi_style_search_enabled(thisThread);

    Value gameResult;
    if (pos.is_game_end(gameResult, ss->ply))
        return gameResult;

    // Check for maximum ply reached
    if (ss->ply >= MAX_PLY)
        return !ss->inCheck ? evaluate(pos) : VALUE_DRAW;

    // Safeguard against too deep recursions in quiescence search
    if (depth < DEPTH_QS_MAX && !ss->inCheck)
        return evaluate(pos);

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                  : DEPTH_QS_NO_CHECKS;
    // Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove = ss->ttHit ? tte->move() : MOVE_NONE;
    pvHit = ss->ttHit && tte->is_pv();

    if (  !PvNode
        && ss->ttHit
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
        return ttValue;

    // Evaluate the position statically
    rawStaticEval = VALUE_NONE;
    if (ss->inCheck)
    {
        ss->staticEval = VALUE_NONE;
        bestValue = futilityBase = -VALUE_INFINITE;
    }
    else
    {
        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            rawStaticEval = tte->eval();
            if (rawStaticEval == VALUE_NONE)
                rawStaticEval = evaluate(pos);

            ss->staticEval = bestValue = to_corrected_static_eval(rawStaticEval, correction_value(thisThread, pos, ss));

            // Can ttValue be used as a better position evaluation?
            if (    ttValue != VALUE_NONE
                && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        }
        else
        {
            // In case of null move search use previous static eval with a different sign
            // and addition of two tempos
            ss->staticEval = bestValue =
            rawStaticEval =
            (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                             : -(ss-1)->staticEval;

            ss->staticEval = bestValue = to_corrected_static_eval(rawStaticEval, correction_value(thisThread, pos, ss));
        }

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!decisive(bestValue))
                bestValue = Value((467 * bestValue + 557 * beta) / 1024);

            // Save gathered info in transposition table
            if (!ss->ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, rawStaticEval);

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = ss->staticEval + 220;
    }

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };
    Square prevSq = to_sq((ss-1)->currentMove);
    int attackBias = xiangqi_attack_bias(thisThread);
    Color us = pos.side_to_move();
    int sideAttackBias = thisThread->rootColor < COLOR_NB && us == thisThread->rootColor ? attackBias : 0;

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions, and other checks (only if depth >= DEPTH_QS_CHECKS)
    // will be generated.
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      prevSq,
                                      &thisThread->pawnHistory,
                                      attackBias ? &thisThread->attackHistory : nullptr,
                                      attackBias,
                                      thisThread->rootColor);

    // Loop through the moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move()) != MOVE_NONE)
    {
      assert(is_ok(move));

      givesCheck = pos.gives_check(move);
      captureOrPromotion = pos.capture_or_promotion(move);
      protectedAttack = sacContinuation = false;
      moveCategory = XMC_NONE;

      if (useAttackStyle)
      {
          bool rootMove = pos.side_to_move() == thisThread->rootColor;
          sacContinuation = xiangqi_sacrifice_continuation(pos, thisThread, move, givesCheck,
                                                           ss->materialDiff);
          moveCategory = xiangqi_move_category(pos, thisThread, move, givesCheck, sacContinuation);
          if (rootMove)
          {
              Bitboard overloaded = Bitboard(0);
              int moveThreat = xiangqi_move_palace_threat_score(pos, thisThread->rootColor, move,
                                                                givesCheck, overloaded);
              int batteryScore = xiangqi_cannon_battery_score(pos, thisThread->rootColor, move,
                                                              givesCheck, overloaded);
              if (batteryScore >= 95)
                  moveCategory |= XMC_CANNON_BATTERY;
              moveThreat = std::max(moveThreat, batteryScore);

              if (moveThreat >= 75 || sacContinuation || (moveCategory & XMC_ATTACK_MASK))
              {
                  int pressure = xiangqi_attack_pressure(pos, thisThread->rootColor);
                  if (moveThreat >= 85
                      && moveThreat + xiangqi_palace_threat(pos, thisThread->rootColor, pressure) / 3 >= 120)
                      moveCategory |= XMC_UNANSWERABLE_THREAT;

                  if (xiangqi_sacrifice_offer_level(pos, thisThread->rootColor, move, moveCategory, pressure))
                      moveCategory |= XMC_SACRIFICE_OFFER;
              }

              if (moveThreat >= 120)
                  moveCategory |= XMC_UNANSWERABLE_THREAT;
          }
          protectedAttack = rootMove ? (givesCheck || (moveCategory & XMC_ATTACK_MASK))
                                     : givesCheck;
      }

      moveCount++;

      if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
      {
          if (!givesCheck && !protectedAttack && to_sq(move) != prevSq && futilityBase > -VALUE_KNOWN_WIN)
          {
              if (moveCount > 2)
                  continue;

              futilityValue = futilityBase + PieceValue[MG][pos.piece_on(to_sq(move))];

              if (futilityValue <= alpha)
              {
                  bestValue = std::max(bestValue, futilityValue);
                  continue;
              }

              if (!pos.see_ge(move, alpha - futilityBase))
              {
                  bestValue = std::max(bestValue, std::min(alpha, futilityBase));
                  continue;
              }
          }

          if (!captureOrPromotion)
          {
              if (!protectedAttack || depth < DEPTH_QS_CHECKS)
                  continue;

              if (!pos.see_ge(move, Value(-60 - sideAttackBias / 3)))
                  continue;
          }

          if (captureOrPromotion && !pos.see_ge(move, Value(protectedAttack ? -160 : -106)))
              continue;
      }

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Check for legality just before making the move
      if (!pos.legal(move))
      {
          moveCount--;
          continue;
      }

      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [captureOrPromotion]
                                                                [history_slot(pos.moved_piece(move))]
                                                                [history_square(to_sq(move))];
      ss->continuationCorrectionHistory =
          &thisThread->continuationCorrectionHistory[history_slot(pos.moved_piece(move))][history_square(to_sq(move))];

      // Continuation history based pruning
      if (  !captureOrPromotion
          && !protectedAttack
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && (*contHist[0])[history_slot(pos.moved_piece(move))][history_square(to_sq(move))] < CounterMovePruneThreshold
          && (*contHist[1])[history_slot(pos.moved_piece(move))][history_square(to_sq(move))] < CounterMovePruneThreshold)
          continue;

      // Make and search the move
      int nextMaterialBalance = xiangqi_next_material_balance(pos, thisThread, ss->materialDiff, move);
      (ss+1)->materialDiff = nextMaterialBalance;
      (ss+1)->sacrificeTrace = xiangqi_next_sacrifice_trace(pos, thisThread, ss, move,
                                                            givesCheck, nextMaterialBalance);
      pos.do_move(move, st, givesCheck);
      value = -qsearch<nodeType>(pos, ss+1, -beta, -alpha, depth - 1);
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Check for a new best move
      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha here!
                  alpha = value;
              else
                  break; // Fail high
          }
       }
    }

    // All legal moves have been searched. A special case: if we're in check
    // and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());

        return pos.checkmate_value(ss->ply); // Plies to mate from the root
    }

    if (!decisive(bestValue) && bestValue > beta)
        bestValue = Value((481 * bestValue + 543 * beta) / 1024);

    // Save gathered info in transposition table
    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER :
              PvNode && bestValue > oldAlpha  ? BOUND_EXACT : BOUND_UPPER,
              ttDepth, bestMove, rawStaticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate or TB score from "plies to mate from the root" to
  // "plies to mate from the current position". Standard scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v >= VALUE_TB_WIN_IN_MAX_PLY  ? v + ply
          : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): it adjusts a mate or TB score
  // from the transposition table (which refers to the plies to mate/be mated from
  // current position) to "plies to mate/be mated (TB win/loss) from the root". However,
  // for mate scores, to avoid potentially false mate scores related to the n-move rule
  // and the graph history interaction, we return an optimal TB score instead.

  Value value_from_tt(Value v, int ply, int r50c) {

    if (v == VALUE_NONE)
        return VALUE_NONE;

    if (v >= VALUE_TB_WIN_IN_MAX_PLY)  // TB win or better
    {
        if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 99 - r50c)
            return VALUE_MATE_IN_MAX_PLY - 1; // do not return a potentially false mate score

        return v - ply;
    }

    if (v <= VALUE_TB_LOSS_IN_MAX_PLY) // TB loss or worse
    {
        if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 99 - r50c)
            return VALUE_MATED_IN_MAX_PLY + 1; // do not return a potentially false mate score

        return v + ply;
    }

    return v;
  }


  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }

  void update_attack_stats(const Position& pos, Move move, int bonus) {

    Thread* thisThread = pos.this_thread();
    if (thisThread->rootColor >= COLOR_NB
        || pos.side_to_move() != thisThread->rootColor
        || !is_ok(move)
        || xiangqi_attack_bias(thisThread) <= 0)
        return;

    bool givesCheck = pos.gives_check(move);
    int materialBalance = xiangqi_material_balance(pos, thisThread->rootColor);
    bool sacContinuation = xiangqi_sacrifice_continuation(pos, thisThread, move, givesCheck,
                                                          materialBalance);
    int category = xiangqi_move_category(pos, thisThread, move, givesCheck, sacContinuation);
    Bitboard overloaded = Bitboard(0);
    int moveThreat = xiangqi_move_palace_threat_score(pos, thisThread->rootColor, move,
                                                      givesCheck, overloaded);
    int batteryScore = xiangqi_cannon_battery_score(pos, thisThread->rootColor, move,
                                                    givesCheck, overloaded);
    if (batteryScore >= 95)
        category |= XMC_CANNON_BATTERY;
    moveThreat = std::max(moveThreat, batteryScore);

    if (moveThreat >= 75 || sacContinuation || (category & XMC_ATTACK_MASK))
    {
        int pressure = xiangqi_attack_pressure(pos, thisThread->rootColor);
        if (moveThreat >= 85
            && moveThreat + xiangqi_palace_threat(pos, thisThread->rootColor, pressure) / 3 >= 120)
            category |= XMC_UNANSWERABLE_THREAT;

        if (xiangqi_sacrifice_offer_level(pos, thisThread->rootColor, move, category, pressure))
            category |= XMC_SACRIFICE_OFFER;
    }

    if (moveThreat >= 120)
        category |= XMC_UNANSWERABLE_THREAT;

    if (!(category & XMC_ATTACK_MASK))
        return;

    int weightedBonus = std::clamp(bonus * attack_category_weight(category) / 128, -16384, 16384);
    if (weightedBonus)
        thisThread->attackHistory[history_slot(pos.moved_piece(move))][history_square(to_sq(move))] << weightedBonus;
  }


  // update_all_stats() updates stats at the end of search() when a bestMove is found

  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth) {

    int bonus1, bonus2;
    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    CapturePieceToHistory& captureHistory = thisThread->captureHistory;
    Piece moved_piece = pos.moved_piece(bestMove);
    PieceType captured = type_of(pos.piece_on(to_sq(bestMove)));

    bonus1 = stat_bonus(depth + 1);
    bonus2 = bestValue > beta + PawnValueMg ? bonus1                                 // larger bonus
                                            : std::min(bonus1, stat_bonus(depth));   // smaller bonus

    if (!pos.capture_or_promotion(bestMove))
    {
        // Increase stats for the best move in case it was a quiet move
        update_quiet_stats(pos, ss, bestMove, bonus2, depth);

        // Decrease stats for all non-best quiet moves
        for (int i = 0; i < quietCount; ++i)
        {
            thisThread->mainHistory[us][history_move(quietsSearched[i])] << -bonus2;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), to_sq(quietsSearched[i]), -bonus2);
            update_attack_stats(pos, quietsSearched[i], -bonus2 / 2);
        }
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        captureHistory[history_slot(moved_piece)][history_square(to_sq(bestMove))][capture_history_slot(captured)] << bonus1;
        update_attack_stats(pos, bestMove, bonus1);
    }

    // Extra penalty for a quiet early move that was not a TT move or
    // main killer move in previous ply when it gets refuted.
    if (   ((ss-1)->moveCount == 1 + (ss-1)->ttHit || ((ss-1)->currentMove == (ss-1)->killers[0]))
        && !pos.captured_piece())
            update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -bonus1);

    // Decrease stats for all non-best capture moves
    for (int i = 0; i < captureCount; ++i)
    {
        moved_piece = pos.moved_piece(capturesSearched[i]);
        captured = type_of(pos.piece_on(to_sq(capturesSearched[i])));
        captureHistory[history_slot(moved_piece)][history_square(to_sq(capturesSearched[i]))][capture_history_slot(captured)] << -bonus1;
        update_attack_stats(pos, capturesSearched[i], -bonus1 / 2);
    }
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, -4, and -6 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    constexpr int indices[6] = {1, 2, 3, 4, 5, 6};
    constexpr int weights[6] = {1076, 639, 293, 523, 129, 445};
    constexpr int multipliers[7] = {96, 100, 100, 100, 115, 118, 129};
    int positiveCount = 0;

    for (int n = 0; n < 6; ++n)
    {
        int i = indices[n];

        // Only update first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;

        if (is_ok((ss-i)->currentMove))
        {
            auto& historyEntry = (*(ss-i)->continuationHistory)[history_slot(pc)][history_square(to)];
            if (historyEntry > 0)
                positiveCount++;

            int multiplier = multipliers[positiveCount];
            historyEntry << (bonus * weights[n] * multiplier / 131072) + 83 * (i < 2);
        }
    }
  }


  // update_quiet_stats() updates move sorting heuristics

  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus, int depth) {

    // Update killers
    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory[us][history_move(move)] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    thisThread->pawnHistory[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)][history_slot(pos.moved_piece(move))][history_square(to_sq(move))]
        << bonus * (bonus > -7 ? 913 : 553) / 1024;

    update_attack_stats(pos, move, bonus / 2);

    // Penalty for reversed move in case of moved piece not being a pawn
    if (type_of(pos.moved_piece(move)) != PAWN && type_of(move) != DROP)
        thisThread->mainHistory[us][history_move(reverse_move(move))] << -bonus;

    // Update countermove history
    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[history_slot(pos.piece_on(prevSq))][history_square(prevSq)] = move;
    }

    // Update low ply history
    if (depth > 11 && ss->ply < MAX_LPH)
        thisThread->lowPlyHistory[ss->ply][history_move(move)] << stat_bonus(depth - 7);
  }

  // When playing with strength handicap, choose best move among a set of RootMoves
  // using a statistical rule dependent on 'level'. Idea by Heinz van Saanen.

  Move Skill::pick_best(size_t multiPV) {

    const RootMoves& rootMoves = Threads.main()->rootMoves;
    static PRNG rng(now()); // PRNG sequence should be non-deterministic

    // RootMoves are already sorted by score in descending order
    Value topScore = rootMoves[0].score;
    int delta = std::min(topScore - rootMoves[multiPV - 1].score, PawnValueMg);
    int weakness = 120 - 2 * level;
    int maxScore = -VALUE_INFINITE;

    // Choose best move. For each move score we add two terms, both dependent on
    // weakness. One is deterministic and bigger for weaker levels, and one is
    // random. Then we choose the move with the resulting highest score.
    for (size_t i = 0; i < multiPV; ++i)
    {
        // This is our magic formula
        int push = (  weakness * int(topScore - rootMoves[i].score)
                    + delta * (rng.rand<unsigned>() % weakness)) / 128;

        if (rootMoves[i].score + push >= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best = rootMoves[i].pv[0];
        }
    }

    return best;
  }

} // namespace


/// MainThread::check_time() is used to print debug info and, more importantly,
/// to detect when we are out of available time and thus stop the search.

void MainThread::check_time() {

  if (--callsCnt > 0)
      return;

  // When using nodes, ensure checking rate is not lower than 0.1% of nodes
  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  static TimePoint lastInfoTime = now();

  TimePoint elapsed = Time.elapsed();
  TimePoint tick = Limits.startTime + elapsed;

  if (tick - lastInfoTime >= 1000)
  {
      lastInfoTime = tick;
      dbg_print();
  }

  // We should not stop pondering until told so by the GUI
  if (ponder)
      return;

  if (   (Limits.use_time_management() && (elapsed > Time.maximum() - 10 || stopOnPonderhit))
      || (Limits.movetime && elapsed >= Limits.movetime)
      || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
      Threads.stop = true;
}


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth, Value alpha, Value beta) {

  std::stringstream ss;
  TimePoint elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t pvIdx = pos.this_thread()->pvIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = rootMoves[i].score != -VALUE_INFINITE;

      if (depth == 1 && !updated && i > 0)
          continue;

      Depth d = updated ? depth : std::max(1, depth - 1);
      Value v = updated ? rootMoves[i].uciScore : rootMoves[i].previousScore;

      if (v == -VALUE_INFINITE)
          v = updated ? rootMoves[i].score : VALUE_ZERO;
      if (v == -VALUE_INFINITE)
          v = VALUE_ZERO;

      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";

      ss << "info"
         << " depth "    << d
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v);

      if (Options["UCI_ShowWDL"])
          ss << UCI::wdl(v, pos.game_ply());

      if (updated && rootMoves[i].scoreLowerbound)
          ss << " lowerbound";
      else if (updated && rootMoves[i].scoreUpperbound)
          ss << " upperbound";
      else if (i == pvIdx)
          ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed;

      if (elapsed > 1000) // Earlier makes little sense
          ss << " hashfull " << TT.hashfull();

      ss << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(pos, m);
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    bool ttHit;

    assert(pv.size() == 1);

    if (pv[0] == MOVE_NONE)
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

} // namespace Stockfish

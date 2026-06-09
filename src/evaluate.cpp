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
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "evaluate.h"
#include "misc.h"
#include "position.h"
#include "thread.h"
#include "uci.h"
#include "incbin/incbin.h"

// Macro to embed the default efficiently updatable neural network (NNUE) file
// data in the engine binary (using incbin.h, by Dale Weiler).
#if !defined(_MSC_VER) && !defined(NNUE_EMBEDDING_OFF)
  INCBIN(EmbeddedNNUE, EvalFileDefaultName);
#else
  const unsigned char        gEmbeddedNNUEData[1] = {0x0};
  [[maybe_unused]]
  const unsigned char *const gEmbeddedNNUEEnd = &gEmbeddedNNUEData[1];
  const unsigned int         gEmbeddedNNUESize = 1;
#endif

namespace Stockfish {

const Variant* currentNnueVariant;

namespace Eval {

bool useNNUE = true;
std::string eval_file_loaded = "None";

namespace {

class MemoryBuffer : public std::basic_streambuf<char> {
public:
    MemoryBuffer(char* p, size_t n) {
        setg(p, p, p + n);
        setp(p, p + n);
    }
};

double to_cp(Value v) {
    return double(v) / PawnValueEg;
}

Value adjusted_nnue(const Position& pos) {
    if (!pos.nnue_applicable())
        return VALUE_ZERO;

    int scale = 903
              + 32 * pos.count<SOLDIER>()
              + 32 * pos.non_pawn_material() / 1024;

    return NNUE::evaluate(pos, true) * scale / 1024;
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
        pressure += 10 * popcount(pos.attackers_to(s, root) & attackers);
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
        else if ((pt == ROOK || pt == CANNON)
              && (file_of(s) == file_of(ksq) || rank_of(s) == rank_of(ksq)))
            pressure += 16;
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

Value apply_duffish_eval_shaping(const Position& pos, Value v) {
    Thread* thisThread = pos.this_thread();

    if (pos.n_move_rule())
        v = v * (2 * pos.n_move_rule() - pos.rule50_count()) / (2 * pos.n_move_rule());

    int evalDecay = thisThread->cachedEvalDecay;
    if (evalDecay > 0 && v != VALUE_ZERO)
    {
        int halfmoves = pos.rule50_count();
        if (halfmoves > 0)
        {
            int decayFactor = 1024 - evalDecay * halfmoves / 50;
            decayFactor = std::max(decayFactor, 512);
            v = Value(int(v) * decayFactor / 1024);
        }
    }

    int aggressiveness = thisThread->cachedAggressiveness;
    if (aggressiveness == 100
        && thisThread->cachedSacBonus == 0
        && thisThread->cachedSacDetect == 0)
        return v;

    Color root = thisThread->rootColor < COLOR_NB ? thisThread->rootColor : pos.side_to_move();
    int rootSign = pos.side_to_move() == root ? 1 : -1;
    int rootEval = rootSign * int(v);
    int styleScale = std::clamp(aggressiveness, 0, 300);
    auto style_bonus = [styleScale](int bonus) { return bonus * styleScale / 100; };

    int currentBalance = xiangqi_material_balance(pos, root);
    int startBalance = thisThread->rootColor < COLOR_NB ? thisThread->rootMaterialBalance
                                                        : currentBalance;
    int rootMaterialLoss = std::max(0, startBalance - currentBalance);
    int attackPressure = -1;
    int palaceThreat = -1;
    bool attackContextKnown = false;
    bool attackContext = false;
    auto getAttackPressure = [&]() {
        if (attackPressure < 0)
            attackPressure = xiangqi_attack_pressure(pos, root);
        return attackPressure;
    };
    auto hasAttackContext = [&]() {
        if (!attackContextKnown)
        {
            attackContext = rootMaterialLoss > 0
                         || xiangqi_guard_damage(pos, root) >= 40
                         || xiangqi_attack_shape(pos, root);
            attackContextKnown = true;
        }
        return attackContext;
    };
    auto getPalaceThreat = [&]() {
        if (palaceThreat < 0)
            palaceThreat = hasAttackContext() && getAttackPressure() >= 80
                         ? xiangqi_palace_threat(pos, root, getAttackPressure())
                         : 0;
        return palaceThreat;
    };

    int sacBonus = thisThread->cachedSacBonus;
    if (sacBonus > 0 && rootMaterialLoss > 0)
    {
        int cappedLoss = std::min(rootMaterialLoss, 1200);
        int pressure = getAttackPressure();
        int quality = xiangqi_sacrifice_quality(pos, root, rootMaterialLoss, pressure);
        int required = xiangqi_required_sacrifice_quality(rootMaterialLoss);
        int bonus = sacBonus * cappedLoss / 260;
        if (quality < required)
        {
            bonus = bonus * std::max(quality, 1) / required;
            if (quality < required / 2)
                bonus /= 2;
        }
        else
        {
            bonus += sacBonus * std::min(quality - required, 240) / 380;
            bonus += thisThread->cachedDynamicComp * std::min(quality, 360) / 760;
            if (quality >= required + 90)
                bonus += sacBonus / 3;
        }
        if (rootEval > 500)
            bonus = bonus * 3 / 2;
        else if (rootEval < -250)
            bonus /= 2;
        rootEval += style_bonus(bonus);
    }

    if (aggressiveness > 100)
    {
        int pressure = getAttackPressure();
        if (pressure >= (rootMaterialLoss > 0 ? 70 : 95))
        {
            int pressureBonus = std::min(165, (pressure - 45) / 2);
            if (rootMaterialLoss > 0)
                pressureBonus += std::min(55, xiangqi_sacrifice_quality(pos, root, rootMaterialLoss, pressure) / 9);
            if (rootEval < -350)
                pressureBonus /= 2;
            rootEval += style_bonus(pressureBonus);
        }

        if (pressure >= 80)
        {
            int threat = getPalaceThreat();
            if (threat >= 105)
                rootEval += style_bonus(std::min(100, 12 + threat / 5));
        }
    }

    int materialOutperformance = rootEval - currentBalance;
    if (rootEval > 0 && materialOutperformance > 180)
        rootEval += style_bonus(std::min(90, 15 + (materialOutperformance - 180) / 12));

    int matScale = thisThread->cachedMatScale;
    if (matScale > 0 && rootEval > 0)
    {
        int totalMat = xiangqi_total_material(pos);
        int multiplier = 1024 + matScale * std::min(totalMat, 6400) / 4800;
        if (rootMaterialLoss > 0)
            multiplier += matScale * std::min(rootMaterialLoss, 800) / 400;
        rootEval = rootEval * multiplier / 1024;
    }

    if (aggressiveness != 100)
        rootEval += (aggressiveness - 100) * rootEval / 500;

    {
        Color them = ~root;
        int advisorBreakBonus = thisThread->cachedAdvisorBreakBonus;
        int bishopBreakBonus = thisThread->cachedBishopBreakBonus;

        if (advisorBreakBonus > 0)
            rootEval += style_bonus(advisorBreakBonus * std::max(0, 2 - pos.count<FERS>(them)));

        if (bishopBreakBonus > 0)
            rootEval += style_bonus(bishopBreakBonus * std::max(0, 2 - pos.count<ELEPHANT>(them)));
    }

    rootEval = std::clamp(rootEval, int(VALUE_TB_LOSS_IN_MAX_PLY + 1),
                                    int(VALUE_TB_WIN_IN_MAX_PLY - 1));
    return Value(rootSign * rootEval);
}

} // namespace

void NNUE::init() {
    useNNUE = true;

    std::string eval_file = std::string(Options["EvalFile"]);
    std::stringstream ss(eval_file);
    const Variant* variant = xiangqi_variant();

    useNNUE = false;
    while (std::getline(ss, eval_file, UCI::SepChar))
    {
        std::string basename = eval_file.substr(eval_file.find_last_of("\\/") + 1);
        std::string nnueAlias = variant->nnueAlias;
        if (basename.rfind("xiangqi", 0) != std::string::npos
            || (!nnueAlias.empty() && basename.rfind(nnueAlias, 0) != std::string::npos))
        {
            useNNUE = true;
            break;
        }
    }

    if (!useNNUE)
        return;

    currentNnueVariant = variant;

#if defined(DEFAULT_NNUE_DIRECTORY)
    #define stringify2(x) #x
    #define stringify(x) stringify2(x)
    std::vector<std::string> dirs = { "<internal>", "", CommandLine::binaryDirectory, stringify(DEFAULT_NNUE_DIRECTORY) };
#else
    std::vector<std::string> dirs = { "<internal>", "", CommandLine::binaryDirectory };
#endif

    for (const std::string& directory : dirs)
        if (eval_file_loaded != eval_file)
        {
            if (directory != "<internal>")
            {
                std::ifstream stream(directory + eval_file, std::ios::binary);
                if (load_eval(eval_file, stream))
                    eval_file_loaded = eval_file;
            }

            if (directory == "<internal>" && eval_file == EvalFileDefaultName)
            {
                MemoryBuffer buffer(const_cast<char*>(reinterpret_cast<const char*>(gEmbeddedNNUEData)),
                                    size_t(gEmbeddedNNUESize));
                std::istream stream(&buffer);
                if (load_eval(eval_file, stream))
                    eval_file_loaded = eval_file;
            }
        }
}

void NNUE::verify() {
    std::string eval_file = std::string(Options["EvalFile"]);

    if (!useNNUE || eval_file.find(eval_file_loaded) == std::string::npos)
    {
        UCI::OptionsMap defaults;
        UCI::init(defaults);

        std::string msg1 = "Duffish 2.2 is NNUE-only; xiangqi network parameters must be available.";
        std::string msg2 = "The network file " + eval_file + " was not loaded successfully.";
        std::string msg3 = "The UCI option EvalFile might need to specify the full path, including the directory name, to the network file.";
        std::string msg4 = "Expected default net: " + std::string(defaults["EvalFile"]);
        std::string msg5 = "The engine will be terminated now.";

        sync_cout << "info string ERROR: " << msg1 << sync_endl;
        sync_cout << "info string ERROR: " << msg2 << sync_endl;
        sync_cout << "info string ERROR: " << msg3 << sync_endl;
        sync_cout << "info string ERROR: " << msg4 << sync_endl;
        sync_cout << "info string ERROR: " << msg5 << sync_endl;

        std::exit(EXIT_FAILURE);
    }

    sync_cout << "info string NNUE-only evaluation using " << eval_file_loaded << " enabled" << sync_endl;
}

Value evaluate(const Position& pos) {
    return apply_duffish_eval_shaping(pos, adjusted_nnue(pos));
}

std::string trace(Position& pos) {
    if (pos.checkers())
        return "Final evaluation: none (in check)";

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);

    if (useNNUE && pos.nnue_applicable())
        ss << NNUE::trace(pos) << '\n';
    else
        ss << "NNUE evaluation is not applicable to this position.\n";

    Value v = evaluate(pos);
    v = pos.side_to_move() == WHITE ? v : -v;

    ss << std::showpos << "Final evaluation       " << to_cp(v)
       << " (white side) [NNUE only]\n";

    return ss.str();
}

} // namespace Eval

} // namespace Stockfish

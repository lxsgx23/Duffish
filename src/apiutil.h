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

#ifndef APIUTIL_H_INCLUDED
#define APIUTIL_H_INCLUDED

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "position.h"
#include "types.h"
#include "variant.h"

namespace Stockfish {

enum Termination {
    ONGOING,
    CHECKMATE,
    STALEMATE,
    INSUFFICIENT_MATERIAL,
    N_MOVE_RULE,
    N_FOLD_REPETITION,
    VARIANT_END,
};

inline bool has_insufficient_material(Color, const Position&) {
    // Xiangqi does not use the western insufficient-material draw shortcut.
    return false;
}

inline Bitboard checked(const Position& pos) {
    return pos.checkers() ? square_bb(pos.square<KING>(pos.side_to_move())) : Bitboard(0);
}

namespace FEN {

enum FenValidation : int {
    FEN_INVALID_COUNTING_RULE = -14,
    FEN_INVALID_CHECK_COUNT = -13,
    FEN_INVALID_PROMOTED_PIECE = -12,
    FEN_INVALID_NB_PARTS = -11,
    FEN_INVALID_CHAR = -10,
    FEN_TOUCHING_KINGS = -9,
    FEN_INVALID_BOARD_GEOMETRY = -8,
    FEN_INVALID_POCKET_INFO = -7,
    FEN_INVALID_SIDE_TO_MOVE = -6,
    FEN_INVALID_CASTLING_INFO = -5,
    FEN_INVALID_EN_PASSANT_SQ = -4,
    FEN_INVALID_NUMBER_OF_KINGS = -3,
    FEN_INVALID_HALF_MOVE_COUNTER = -2,
    FEN_INVALID_MOVE_COUNTER = -1,
    FEN_EMPTY = 0,
    FEN_OK = 1
};

inline std::vector<std::string> split(const std::string& text, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;

    while (true)
    {
        size_t end = text.find(delim, start);
        if (end == std::string::npos)
        {
            if (start < text.size())
                parts.emplace_back(text.substr(start));
            break;
        }

        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }

    return parts;
}

inline bool unsigned_number_or_dash(const std::string& field) {
    if (field == "-")
        return true;
    return !field.empty() && std::all_of(field.begin(), field.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

inline PieceType xiangqi_piece_type(char c) {
    switch (std::tolower(static_cast<unsigned char>(c)))
    {
    case 'r': return ROOK;
    case 'n':
    case 'h': return HORSE;
    case 'b':
    case 'e': return ELEPHANT;
    case 'a': return FERS;
    case 'k': return KING;
    case 'c': return CANNON;
    case 'p': return SOLDIER;
    default:  return NO_PIECE_TYPE;
    }
}

inline bool in_palace(Color c, int file, int rank) {
    return file >= 3 && file <= 5 && ((c == WHITE && rank <= 2) || (c == BLACK && rank >= 7));
}

inline bool elephant_on_own_side(Color c, int rank) {
    return (c == WHITE && rank <= 4) || (c == BLACK && rank >= 5);
}

inline FenValidation validate_fen(const std::string& fen, const Variant* v, bool chess960 = false) {
    (void)chess960;

    if (fen.empty())
    {
        std::cerr << "Fen is empty." << std::endl;
        return FEN_EMPTY;
    }

    std::vector<std::string> fenParts = split(fen, ' ');
    if (!(fenParts.size() == 1 || fenParts.size() == 2 || fenParts.size() == 4 || fenParts.size() == 6))
    {
        std::cerr << "Invalid number of xiangqi fen parts. Expected 1, 2, 4, or 6. Actual: "
                  << fenParts.size() << std::endl;
        return FEN_INVALID_NB_PARTS;
    }

    if ((v->maxRank + 1) != 10 || (v->maxFile + 1) != 9)
    {
        std::cerr << "Duffish supports only 9x10 xiangqi FEN." << std::endl;
        return FEN_INVALID_BOARD_GEOMETRY;
    }

    std::vector<std::string> ranks = split(fenParts[0], '/');
    if (ranks.size() != 10)
    {
        std::cerr << "Invalid xiangqi board geometry. Expected 10 ranks. Actual: "
                  << ranks.size() << std::endl;
        return FEN_INVALID_BOARD_GEOMETRY;
    }

    int pieceCount[COLOR_NB][PIECE_TYPE_NB] = {};
    int whiteKingFile = -1, blackKingFile = -1;
    int whiteKingRank = -1, blackKingRank = -1;

    for (int row = 0; row < 10; ++row)
    {
        int file = 0;
        int rank = 9 - row;

        for (char token : ranks[row])
        {
            if (std::isdigit(static_cast<unsigned char>(token)))
            {
                int empty = token - '0';
                if (empty <= 0)
                {
                    std::cerr << "Invalid xiangqi board geometry." << std::endl;
                    return FEN_INVALID_BOARD_GEOMETRY;
                }
                file += empty;
                if (file > 9)
                {
                    std::cerr << "Invalid xiangqi board geometry. Rank " << row + 1
                              << " exceeds 9 files." << std::endl;
                    return FEN_INVALID_BOARD_GEOMETRY;
                }
                continue;
            }

            PieceType pt = xiangqi_piece_type(token);
            if (pt == NO_PIECE_TYPE || file >= 9)
            {
                std::cerr << "Invalid xiangqi FEN character: '" << token << "'." << std::endl;
                return FEN_INVALID_CHAR;
            }

            Color c = std::isupper(static_cast<unsigned char>(token)) ? WHITE : BLACK;
            pieceCount[c][pt]++;

            if (pt == KING)
            {
                if (c == WHITE)
                {
                    whiteKingFile = file;
                    whiteKingRank = rank;
                }
                else
                {
                    blackKingFile = file;
                    blackKingRank = rank;
                }
            }

            if ((pt == KING || pt == FERS) && !in_palace(c, file, rank))
            {
                std::cerr << "Xiangqi king/advisor outside palace." << std::endl;
                return FEN_INVALID_BOARD_GEOMETRY;
            }

            if (pt == ELEPHANT && !elephant_on_own_side(c, rank))
            {
                std::cerr << "Xiangqi elephant outside own side." << std::endl;
                return FEN_INVALID_BOARD_GEOMETRY;
            }

            file++;
        }

        if (file != 9)
        {
            std::cerr << "Invalid xiangqi board geometry. Rank " << row + 1
                      << " has " << file << " files." << std::endl;
            return FEN_INVALID_BOARD_GEOMETRY;
        }
    }

    if (pieceCount[WHITE][KING] != 1 || pieceCount[BLACK][KING] != 1)
    {
        std::cerr << "Invalid number of xiangqi kings." << std::endl;
        return FEN_INVALID_NUMBER_OF_KINGS;
    }

    auto exceeds_start_count = [&](Color c) {
        return pieceCount[c][ROOK] > 2
            || pieceCount[c][HORSE] > 2
            || pieceCount[c][ELEPHANT] > 2
            || pieceCount[c][FERS] > 2
            || pieceCount[c][CANNON] > 2
            || pieceCount[c][SOLDIER] > 5;
    };

    if (exceeds_start_count(WHITE) || exceeds_start_count(BLACK))
    {
        std::cerr << "Too many xiangqi pieces in FEN." << std::endl;
        return FEN_INVALID_BOARD_GEOMETRY;
    }

    if (whiteKingFile == blackKingFile)
    {
        bool pieceBetweenKings = false;
        for (int row = 9 - std::max(whiteKingRank, blackKingRank) + 1;
             row < 9 - std::min(whiteKingRank, blackKingRank);
             ++row)
        {
            int file = 0;
            for (char token : ranks[row])
            {
                if (std::isdigit(static_cast<unsigned char>(token)))
                    file += token - '0';
                else
                {
                    if (file == whiteKingFile)
                    {
                        pieceBetweenKings = true;
                        break;
                    }
                    file++;
                }
            }
            if (pieceBetweenKings)
                break;
        }

        if (!pieceBetweenKings)
        {
            std::cerr << "Invalid xiangqi FEN: kings face each other." << std::endl;
            return FEN_TOUCHING_KINGS;
        }
    }

    if (fenParts.size() >= 2 && (fenParts[1].size() != 1 || (fenParts[1][0] != 'w' && fenParts[1][0] != 'b')))
    {
        std::cerr << "Invalid side to move specification: '" << fenParts[1] << "'." << std::endl;
        return FEN_INVALID_SIDE_TO_MOVE;
    }

    if (fenParts.size() >= 4)
    {
        if (fenParts[2] != "-")
        {
            std::cerr << "Xiangqi FEN must not contain castling rights." << std::endl;
            return FEN_INVALID_CASTLING_INFO;
        }
        if (fenParts[3] != "-")
        {
            std::cerr << "Xiangqi FEN must not contain en-passant squares." << std::endl;
            return FEN_INVALID_EN_PASSANT_SQ;
        }
    }

    if (fenParts.size() == 6)
    {
        if (!unsigned_number_or_dash(fenParts[4]))
            return FEN_INVALID_HALF_MOVE_COUNTER;
        if (!unsigned_number_or_dash(fenParts[5]))
            return FEN_INVALID_MOVE_COUNTER;
    }

    return FEN_OK;
}

} // namespace FEN

} // namespace Stockfish

#endif // #ifndef APIUTIL_H_INCLUDED

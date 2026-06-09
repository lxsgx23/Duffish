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
#include <cctype>
#include <fstream>
#include <iostream>
#include <istream>
#include <string>
#include <vector>

#include "position.h"
#include "uci.h"

using namespace std;

namespace Stockfish {

namespace {

bool is_bench_variant_alias(const string& token) {
  return token == "xiangqi" || token == "standard";
}

bool is_unsigned_number(const string& token) {
  return !token.empty()
      && all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); });
}

}

/// setup_bench() builds a list of UCI commands to be run by bench. There
/// are five parameters: TT size in MB, number of search threads that
/// should be used, the limit value spent for each position, a file name
/// where to look for positions in FEN format, and the type of the limit:
/// depth, perft, nodes and movetime (in millisecs). A historical evaluation
/// type argument is accepted for compatibility but ignored; Duffish is NNUE-only.
///
/// bench -> search the Xiangqi start position up to depth 13
/// bench 64 1 15 -> search the Xiangqi start position up to depth 15 (TT = 64MB)
/// bench 64 4 5000 current movetime -> search current position with 4 threads for 5 sec
/// bench 64 1 100000 default nodes -> search the Xiangqi start position for 100K nodes
/// bench 16 1 5 default perft -> run a perft 5 on the Xiangqi start position

vector<string> setup_bench(const Position& current, istream& is) {

  vector<string> fens, list;
  string go, token;

  streampos args = is.tellg();
  // Accept historical "bench xiangqi ..." and "bench standard ..." forms as no-ops.
  if (is >> token)
  {
      if (is_bench_variant_alias(token))
          args = is.tellg();
      else
      {
          if (!is_unsigned_number(token))
          {
              cerr << "Unsupported bench variant '" << token
                   << "'. Duffish is xiangqi-only." << endl;
              exit(EXIT_FAILURE);
          }

          is.seekg(args);
      }
  }
  else
      is.clear();

  const Variant* variant = xiangqi_variant();

  // Assign default values to missing arguments
  string ttSize    = (is >> token) ? token : "16";
  string threads   = (is >> token) ? token : "1";
  string limit     = (is >> token) ? token : "13";
  string fenFile   = (is >> token) ? token : "default";
  string limitType = (is >> token) ? token : "depth";
  // Historical "classical/mixed/NNUE" argument. There is no HCE path in Duffish.
  (void)(is >> token);

  go = limitType == "eval" ? "eval" : "go " + limitType + " " + limit;

  if (fenFile == "default")
      fens.push_back(variant->startFen);

  else if (fenFile == "current")
      fens.push_back(current.fen());

  else
  {
      string fen;
      ifstream file(fenFile);

      if (!file.is_open())
      {
          cerr << "Unable to open file " << fenFile << endl;
          exit(EXIT_FAILURE);
      }

      while (getline(file, fen))
          if (!fen.empty())
              fens.push_back(fen);

      file.close();
  }

  list.emplace_back("setoption name Threads value " + threads);
  list.emplace_back("setoption name Hash value " + ttSize);
  list.emplace_back("ucinewgame");

  for (const string& fen : fens)
      if (fen.find("setoption") != string::npos)
          list.emplace_back(fen);
      else
      {
          list.emplace_back("position fen " + fen);
          list.emplace_back(go);
      }

  return list;
}

} // namespace Stockfish

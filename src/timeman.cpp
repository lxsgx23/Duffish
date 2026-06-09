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
#include <cfloat>
#include <cmath>
#include <cstdint>

#include "search.h"
#include "timeman.h"
#include "uci.h"

namespace Stockfish {

TimeManagement Time; // Our global time management object


/// TimeManagement::init() is called at the beginning of the search and calculates
/// the bounds of time allowed for the current game ply. We currently support:
//      1) x basetime (+ z increment)
//      2) x moves in y seconds (+ z increment)

void TimeManagement::clear() {
  availableNodes = -1;
}

void TimeManagement::advance_nodes_time(int64_t nodes) {
  availableNodes = std::max(int64_t(0), availableNodes - nodes);
}

void TimeManagement::init(const Position&, Search::LimitsType& limits, Color us, int ply, double& originalTimeAdjust) {

  TimePoint moveOverhead    = TimePoint(Options["Move Overhead"]);
  TimePoint npmsec          = TimePoint(Options["nodestime"]);

  startTime = limits.startTime;

  if (!limits.time[us])
      return;

  // optScale is a percentage of available time to use for the current move.
  // maxScale is a multiplier applied to optimumTime.
  double optScale, maxScale;

  // If we have to play in 'nodes as time' mode, then convert from time
  // to nodes, and use resulting values in time management formulas.
  // WARNING: to avoid time losses, the given npmsec (nodes per millisecond)
  // must be much lower than the real engine speed.
  if (npmsec)
  {
      if (availableNodes == -1) // Only once at game start
          availableNodes = npmsec * limits.time[us]; // Time is in msec

      // Convert from milliseconds to nodes
      limits.time[us] = TimePoint(availableNodes);
      limits.inc[us] *= npmsec;
      limits.npmsec = npmsec;
      moveOverhead *= npmsec;
  }

  const int64_t scaleFactor = npmsec ? int64_t(npmsec) : 1;
  const TimePoint scaledTime = limits.time[us] / scaleFactor;

  // Maximum move horizon
  int mtg = limits.movestogo ? std::min(limits.movestogo, 50) : 50;

  // If less than one second is left, spend less time modeling future moves.
  if (scaledTime < 1000)
      mtg = std::max(1, int(scaledTime * 0.05));

  // Make sure timeLeft is > 0 since we may use it as a divisor
  TimePoint timeLeft =  std::max(TimePoint(1),
      limits.time[us] + limits.inc[us] * (mtg - 1) - moveOverhead * (2 + mtg));

  // A user may scale time usage by setting UCI option "Slow Mover"
  // Default is 100 and changing this value will probably lose elo.
  timeLeft = TimePoint(Options["Slow Mover"]) * timeLeft / 100;

  // x basetime (+ z increment)
  // If there is a healthy increment, timeLeft can exceed actual available
  // game time for the current move, so also cap to a percentage of available game time.
  if (limits.movestogo == 0)
  {
      if (originalTimeAdjust < 0)
          originalTimeAdjust = 0.3356 * std::log10(timeLeft) - 0.4903;

      double logTimeInSec = std::log10(std::max(1.0, double(scaledTime) / 1000.0));
      double optConstant  = std::min(0.0034013 + 0.00020657 * logTimeInSec, 0.004536);
      double maxConstant  = std::max(3.7803 + 2.8003 * logTimeInSec, 2.5470);

      optScale = std::min(0.017244 + std::pow(ply + 2.71111, 0.43433) * optConstant,
                           0.20577 * limits.time[us] / double(timeLeft))
               * originalTimeAdjust;
      maxScale = std::min(7.002, maxConstant + ply / 13.184);
  }

  // x moves in y seconds (+ z increment)
  else
  {
      optScale = std::min((0.88 + ply / 116.4) / mtg,
                            0.88 * limits.time[us] / double(timeLeft));
      maxScale = 1.3 + 0.11 * mtg;
  }

  // Never use more than most of the available time for this move.
  optimumTime = TimePoint(std::max(1.0, optScale * timeLeft));
  maximumTime = TimePoint(std::max(double(optimumTime),
                    std::min(0.8237 * limits.time[us] - moveOverhead, maxScale * optimumTime)));

  if (Options["Ponder"])
      optimumTime += optimumTime / 4;
}

} // namespace Stockfish

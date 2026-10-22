// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "FLARM/Traffic.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OGN {

/**
 * One aircraft position received from the OGN APRS-IS network.
 */
struct AprsReport {
  /** 24 bit device address */
  uint32_t address;

  /** Seconds of the UTC day of the beacon, if known */
  std::optional<unsigned> beacon_time;

  /** The report was relayed from SafeSky (tocall "OGNSKY") */
  bool safesky_relay;

  /**
   * The decoded target with absolute location and GPS altitude (MSL).
   * #FlarmTraffic::valid is not set.
   */
  FlarmTraffic traffic;
};

/**
 * Parse one OGN APRS aircraft position line.  Returns std::nullopt for
 * server comments, receiver beacons, aircraft which requested no
 * tracking and malformed lines.
 */
[[gnu::pure]]
std::optional<AprsReport>
ParseAprsPosition(std::string_view line) noexcept;

/**
 * Was #beacon sent before #reference?  Several receivers relay the
 * same beacon, sometimes late.  Times are seconds of the UTC day;
 * midnight wrap-around is handled.
 */
[[gnu::const]]
bool
IsOlderBeacon(unsigned beacon, unsigned reference) noexcept;

/**
 * Maximum length of an APRS-IS login callsign including the SSID.
 */
static constexpr std::size_t MAX_CALLSIGN_LENGTH = 9;

/**
 * Build the APRS-IS login callsign from the glider registration:
 * upper case, '-' replaced by '0', other non-alphanumeric characters
 * removed, followed by "-" and an SSID derived from #random (0..15,
 * or 0..9 if the base is too long).  The result is never longer than
 * #MAX_CALLSIGN_LENGTH.  Without a registration, "XCS" and four
 * random digits are used as base.
 *
 * Example: "F-CGCN" -> "F0CGCN-7"
 */
std::string
MakeAprsCallsign(std::string_view registration, unsigned random);

/**
 * Build the APRS-IS range filter ("r/LAT/LON/RADIUS").
 */
std::string
MakeAprsRangeFilter(double latitude, double longitude, unsigned radius_km);

} // namespace OGN

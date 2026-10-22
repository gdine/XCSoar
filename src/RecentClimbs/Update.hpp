// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Engine/Waypoint/Ptr.hpp"
#include "Engine/Waypoint/Waypoint.hpp"

#include <string>
#include <string_view>
#include <vector>

class Waypoints;
class RasterTerrain;

namespace RecentClimbs {

/**
 * Convert a Google Drive share link ("…/file/d/ID/view",
 * "…/open?id=ID", "…/uc?id=ID") to a direct download URL.  Any other
 * URL is returned unchanged.
 */
[[gnu::pure]]
std::string
ResolveDownloadUrl(std::string_view url) noexcept;

/**
 * Parse a downloaded CUP file.  Waypoints of the default type are
 * shown as thermal hotspots.  Missing elevations are not filled in
 * here, so the result can be compared with a later download.
 *
 * Throws on error, including when the response is an HTML page
 * (e.g. a Google Drive login page for a file that is not shared).
 */
std::vector<Waypoint>
Parse(std::string_view body);

struct Selection {
  std::vector<Waypoint> climbs;
  unsigned unknown_age = 0;
  unsigned unknown_rate = 0;
  std::chrono::system_clock::time_point next_expiry =
    std::chrono::system_clock::time_point::max();
};

/**
 * Select observations no older than max_age, excluding unknown and future
 * times, and optionally filter by minimum average climb rate (m/s).
 * The next expiry allows offline ageing without re-downloading.
 */
Selection
Select(const std::vector<Waypoint> &downloaded,
       std::chrono::system_clock::time_point now,
       std::chrono::minutes max_age, double min_climb_rate = 0);

/**
 * A climb currently in the waypoint database.
 */
struct Entry {
  WaypointPtr waypoint;

  /**
   * The climb as parsed from the file, before the terrain elevation
   * fallback; terrain heights depend on the currently loaded terrain
   * tiles and must not make an unchanged climb look modified.
   */
  Waypoint parsed;
};

struct UpdateResult {
  unsigned added = 0, removed = 0, kept = 0;

  constexpr bool IsModified() const noexcept {
    return added > 0 || removed > 0;
  }
};

/**
 * Synchronise the climbs previously added to #waypoints (tracked in
 * #current) with a freshly downloaded list: keep identical ones,
 * erase those that disappeared and append new ones (taking a missing
 * elevation from #terrain).  Calls Waypoints::Optimise() if anything
 * changed.
 *
 * The caller must ensure exclusive access to #waypoints.
 */
UpdateResult
Update(Waypoints &waypoints, std::vector<Entry> &current,
       std::vector<Waypoint> &&downloaded,
       const RasterTerrain *terrain = nullptr) noexcept;

} // namespace RecentClimbs

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Aprs.hpp"
#include "Geo/GeoPoint.hpp"
#include "time/Stamp.hpp"

#include <optional>
#include <span>
#include <unordered_map>

struct TrafficList;
struct OGNSettings;

/**
 * Traffic cache for the OGN APRS feed.
 *
 * Each aircraft is kept for #STALE seconds after its last beacon.
 * #Publish() selects the most relevant targets (near first, then
 * climbing, then the rest) for the device's traffic list.
 */
class OGNTraffic {
public:
  static constexpr FloatDuration STALE = std::chrono::seconds(20);
  static constexpr std::size_t MAX_TARGETS = 5000;
  static constexpr double PRIORITY_RANGE_M = 20000;

  enum class IngestResult {
    ACCEPTED,

    /** a late copy of an older beacon was ignored */
    OLDER,
  };

private:
  struct Target {
    OGN::AprsReport report;
    TimeStamp updated;
  };

  std::unordered_map<uint32_t, Target> targets;

public:
  IngestResult Ingest(const OGN::AprsReport &report, TimeStamp now) noexcept;

  /**
   * Remove stale targets and replace the contents of #list with the
   * selected targets.  Without a valid own location, nothing is
   * published.
   */
  void Publish(TrafficList &list, TimeStamp now,
               GeoPoint own_location, std::optional<double> own_altitude,
               std::span<const FlarmId> own_ids,
               const OGNSettings &settings) noexcept;

  std::size_t GetTargetCount() const noexcept {
    return targets.size();
  }

  void Clear() noexcept {
    targets.clear();
  }
};

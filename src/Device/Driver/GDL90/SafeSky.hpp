// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "FLARM/Traffic.hpp"
#include "Geo/GeoPoint.hpp"
#include "time/Stamp.hpp"

#include <cstdint>
#include <optional>
#include <vector>

struct TrafficList;

/**
 * Settings of the "SafeSky GDL90 (UDP)" port.
 */
struct SafeSkySettings {
  static constexpr unsigned DEFAULT_HRANGE_M = 60000;
  static constexpr unsigned MIN_HRANGE_M = 1000;
  static constexpr unsigned MAX_HRANGE_M = 200000;
  static constexpr unsigned MAX_VRANGE_M = 10000;

  /** Maximum horizontal distance from the SafeSky ownship [m]. */
  unsigned hrange = DEFAULT_HRANGE_M;

  /** Maximum altitude difference [m]; 0 disables the vertical filter. */
  unsigned vrange = 0;
};

extern SafeSkySettings safesky_settings;

void
LoadFromProfile(SafeSkySettings &settings) noexcept;

void
SaveToProfile(const SafeSkySettings &settings) noexcept;

/**
 * Traffic cache for the SafeSky GDL90 stream.
 *
 * SafeSky sends traffic reports irregularly and with a zero vertical
 * velocity.  This cache keeps each target for #STALE seconds,
 * estimates the climb rate from the altitude history and publishes
 * the most relevant targets (near first, then climbing, then the
 * rest) into the device's traffic list on every received datagram.
 *
 * SafeSky's ownship report is only used for filtering and ranking;
 * it is never published as a GPS fix.
 */
class SafeSkyTraffic {
public:
  static constexpr FloatDuration STALE = std::chrono::seconds(15);
  static constexpr FloatDuration CLIMB_WINDOW = std::chrono::seconds(10);
  static constexpr FloatDuration CLIMB_MIN_SPAN = std::chrono::seconds(4);
  static constexpr std::size_t MAX_TARGETS = 1000;
  static constexpr std::size_t MAX_HISTORY = 64;
  static constexpr double PRIORITY_RANGE_M = 20000;

  struct Report {
    uint32_t address;
    uint8_t address_type;

    /** Navigation integrity category is non-zero */
    bool valid_position;

    /** Decoded target with absolute location/altitude */
    FlarmTraffic traffic;
  };

private:
  struct Sample {
    TimeStamp time;
    double altitude;
  };

  struct Target {
    Report report;
    TimeStamp updated;
    std::vector<Sample> history;
    bool climb_estimated;
  };

  std::vector<Target> targets;

  struct Ownship {
    uint32_t address;
    uint8_t address_type;
    GeoPoint location;
    std::optional<double> pressure_altitude;
    bool valid_position;
    TimeStamp updated;
  };

  std::optional<Ownship> ownship;

  std::optional<bool> heartbeat_gps_valid;
  TimeStamp heartbeat_time = TimeStamp::Undefined();

public:
  void OnHeartbeat(bool gps_valid, TimeStamp now) noexcept;

  void OnOwnship(uint32_t address, uint8_t address_type,
                 GeoPoint location, bool valid_position,
                 std::optional<double> pressure_altitude,
                 TimeStamp now) noexcept;

  void OnTraffic(const Report &report, TimeStamp now) noexcept;

  /**
   * Remove stale targets and replace the contents of #list with the
   * selected targets.
   */
  void Publish(TrafficList &list, TimeStamp now,
               const SafeSkySettings &settings) noexcept;

  std::size_t GetTargetCount() const noexcept {
    return targets.size();
  }

private:
  [[gnu::pure]]
  const Ownship *GetFreshOwnship(TimeStamp now) const noexcept;

  static void EstimateClimb(Target &target) noexcept;
};

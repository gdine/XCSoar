// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include <chrono>
#include <cmath>
#include <string>

/**
 * Settings for the "Recent climbs" service, which periodically
 * downloads a CUP file listing recently detected climbs and shows
 * them as temporary waypoints.
 */
struct RecentClimbsSettings {
  static constexpr std::chrono::seconds DEFAULT_INTERVAL{std::chrono::minutes{3}};
  static constexpr std::chrono::seconds MIN_INTERVAL{std::chrono::minutes{1}};
  static constexpr std::chrono::seconds MAX_INTERVAL{std::chrono::minutes{60}};
  static constexpr std::chrono::seconds INTERVAL_STEP{std::chrono::minutes{1}};
  static constexpr unsigned DEFAULT_MAX_AGE = 60;
  static constexpr unsigned MIN_MAX_AGE = 15;
  static constexpr unsigned MAX_MAX_AGE = 1440;
  static constexpr unsigned MAX_AGE_STEP = 15;

  bool enabled = false;

  /**
   * Google Drive share link or direct HTTP(S) URL of the CUP file.
   */
  std::string url;

  std::chrono::seconds interval = DEFAULT_INTERVAL;

  /** Maximum observation age in minutes. */
  unsigned max_age = DEFAULT_MAX_AGE;

  /** Minimum average climb rate in m/s; zero disables rate filtering. */
  double min_climb_rate = 0;

  bool operator==(const RecentClimbsSettings &) const = default;

  void SetDefaults() noexcept {
    enabled = false;
    url.clear();
    interval = DEFAULT_INTERVAL;
    max_age = DEFAULT_MAX_AGE;
    min_climb_rate = 0;
  }

  static constexpr std::chrono::seconds
  ClampInterval(std::chrono::seconds value) noexcept {
    if (value < MIN_INTERVAL)
      return MIN_INTERVAL;
    if (value > MAX_INTERVAL)
      return MAX_INTERVAL;
    return value;
  }

  static constexpr unsigned ClampMaxAge(unsigned value) noexcept {
    if (value < MIN_MAX_AGE)
      return MIN_MAX_AGE;
    if (value > MAX_MAX_AGE)
      return MAX_MAX_AGE;
    return value / MAX_AGE_STEP * MAX_AGE_STEP;
  }

  static double ClampMinClimbRate(double value) noexcept {
    return std::isfinite(value) && value > 0 ? value : 0.;
  }
};

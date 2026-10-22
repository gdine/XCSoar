// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

/**
 * Settings of the "OGN traffic receiver" port.
 */
struct OGNSettings {
  static constexpr unsigned DEFAULT_GLIDER_RANGE_M = 200000;
  static constexpr unsigned DEFAULT_OTHER_RANGE_M = 10000;
  static constexpr unsigned MIN_GLIDER_RANGE_M = 5000;
  static constexpr unsigned MAX_RANGE_M = 300000;
  static constexpr unsigned MAX_VRANGE_M = 10000;

  /** Maximum distance of gliders [m] */
  unsigned glider_range = DEFAULT_GLIDER_RANGE_M;

  /** Maximum distance of all other aircraft [m]; 0 hides them */
  unsigned other_range = DEFAULT_OTHER_RANGE_M;

  /** Maximum altitude difference [m]; 0 disables the vertical filter */
  unsigned vrange = 0;

  /**
   * Ignore SafeSky users relayed to OGN; they are received directly
   * through the SafeSky port (under another id).
   */
  bool ignore_safesky = true;

  /**
   * Radius of the APRS-IS server filter [km].
   */
  constexpr unsigned GetFilterRadiusKm() const noexcept {
    const unsigned range = glider_range > other_range
      ? glider_range : other_range;
    return range / 1000 + 20;
  }
};

extern OGNSettings ogn_settings;

void
LoadFromProfile(OGNSettings &settings) noexcept;

void
SaveToProfile(const OGNSettings &settings) noexcept;

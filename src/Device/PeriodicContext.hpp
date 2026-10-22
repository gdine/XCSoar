// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "FLARM/Id.hpp"
#include "Geo/GeoPoint.hpp"
#include "util/StaticArray.hxx"
#include "util/StaticString.hxx"

#include <optional>

/**
 * Snapshot of XCSoar's own state passed to
 * Device::OnPeriodicUpdate().
 */
struct DevicePeriodicContext {
  /** Merged own position; invalid if unknown */
  GeoPoint location = GeoPoint::Invalid();

  /** Merged own altitude above MSL [m] */
  std::optional<double> altitude;

  /** Registration of the configured plane (may be empty) */
  StaticString<32> registration;

  /** FLARM/OGN ids of the own aircraft */
  StaticArray<FlarmId, 16> own_ids;

  /** XCSoar version string */
  const char *version = "";
};

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "RecentClimbs/Settings.hpp"

#include <algorithm>
#include <chrono>
#include <string>

struct CollaborativeFlightSettings {
  bool enabled = false;
  bool debug = false;
  std::string folder_url;
  std::string upload_url;
  std::string token;
  std::string public_identity = "CF";
  std::chrono::seconds interval{std::chrono::minutes{3}};
  RecentClimbsSettings display;

  static constexpr std::chrono::seconds MIN_INTERVAL{60};
  static constexpr std::chrono::seconds MAX_INTERVAL{3600};
  static constexpr std::chrono::seconds INTERVAL_STEP{60};

  void SetDefaults() noexcept {
    *this = CollaborativeFlightSettings{};
  }

  static constexpr auto ClampInterval(std::chrono::seconds value) noexcept {
    return std::clamp(value, MIN_INTERVAL, MAX_INTERVAL);
  }

  bool operator==(const CollaborativeFlightSettings &) const = default;

  bool HasSameUploadConfiguration(
    const CollaborativeFlightSettings &other) const {
    return enabled == other.enabled && debug == other.debug &&
      folder_url == other.folder_url && upload_url == other.upload_url &&
      token == other.token && public_identity == other.public_identity &&
      interval == other.interval;
  }

  RecentClimbsSettings GetDisplaySettings() const {
    auto result = display;
    result.enabled = enabled;
    return result;
  }
};

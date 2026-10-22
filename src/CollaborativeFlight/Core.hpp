// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Geo/GeoPoint.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CollaborativeFlight {

using Clock = std::chrono::system_clock;
static constexpr std::chrono::hours MAX_CLIMB_DURATION{2};

struct Sample {
  Clock::time_point time;
  GeoPoint location;
  double altitude;
};

struct Climb {
  Clock::time_point start, end;
  GeoPoint location;
  double base, top, rate;

  std::string Id(std::string_view client) const;
};

/** Bounded, fixed-lag circling HMM, similar to RecentClimbs' IGC detector. */
class Detector {
  std::vector<Sample> points;
  Clock::time_point last_emitted{};

public:
  static constexpr unsigned MAX_SAMPLES = 7200;
  static constexpr std::chrono::seconds LOOKAHEAD{300};

  void Add(const Sample &sample);
  std::vector<Climb> Detect(Clock::time_point now);
  void Reset() noexcept { points.clear(); }

private:
  std::vector<Climb> DetectSegment(std::span<const Sample> segment,
                                   Clock::time_point now);
};

struct PendingClimb {
  Climb climb;
  bool delivered = false;
  std::string public_identity;
};

/** Summaries only: raw fixes are never written to disk or uploaded. */
class Queue {
public:
  static constexpr unsigned MAX_CLIMBS = 512;
  static constexpr std::chrono::hours RETENTION{24};
  std::string client;
  std::vector<PendingClimb> climbs;

  void Add(const Climb &climb, std::string_view public_identity = {});
  unsigned Prune(Clock::time_point now) noexcept;
  void Acknowledge(const std::vector<std::string> &ids);
  std::string Serialize() const;
  static Queue Parse(std::string_view body);
};

struct Batch {
  std::string id, body;
  std::vector<std::string> climb_ids;
};

/** Throw on malformed configuration or protocol data. */
void ValidatePublicIdentity(std::string_view identity);
std::string DefaultPublicIdentity(std::string_view pilot,
                                  std::string_view registration);
std::string FolderId(std::string_view url);
void ValidateUploadUrl(std::string_view url);
Batch MakeBatch(const Queue &queue, std::string_view folder,
                std::string_view token, Clock::time_point now);
void ValidateAcknowledgement(std::string_view response, const Batch &batch);

} // namespace CollaborativeFlight

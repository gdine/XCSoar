// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Traffic.hpp"
#include "Settings.hpp"
#include "FLARM/List.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

OGNTraffic::IngestResult
OGNTraffic::Ingest(const OGN::AprsReport &report, TimeStamp now) noexcept
try {
  auto i = targets.find(report.address);
  if (i != targets.end()) {
    Target &target = i->second;
    if (target.report.beacon_time && report.beacon_time &&
        OGN::IsOlderBeacon(*report.beacon_time, *target.report.beacon_time))
      return IngestResult::OLDER;

    const auto previous_name = target.report.traffic.name;
    target.report = report;
    if (target.report.traffic.name.empty())
      target.report.traffic.name = previous_name;
    target.updated = now;
    return IngestResult::ACCEPTED;
  }

  if (targets.size() >= MAX_TARGETS) {
    auto oldest = std::min_element(targets.begin(), targets.end(),
                                   [](const auto &a, const auto &b) {
                                     return a.second.updated < b.second.updated;
                                   });
    targets.erase(oldest);
  }

  targets.emplace(report.address, Target{report, now});
  return IngestResult::ACCEPTED;
} catch (const std::bad_alloc &) {
  targets.clear();
  return IngestResult::ACCEPTED;
}

void
OGNTraffic::Publish(TrafficList &list, TimeStamp now,
                    GeoPoint own_location, std::optional<double> own_altitude,
                    std::span<const FlarmId> own_ids,
                    const OGNSettings &settings) noexcept
{
  std::erase_if(targets, [now](const auto &item) {
    const TimeStamp updated = item.second.updated;
    return updated > now || now - updated >= STALE;
  });

  struct Candidate {
    const FlarmTraffic *traffic;
    unsigned rank;
    double distance;
  };

  std::vector<Candidate> candidates;

  if (own_location.IsValid()) {
    try {
      candidates.reserve(targets.size());
    } catch (const std::bad_alloc &) {
      return;
    }

    for (const auto &[address, target] : targets) {
      /* SafeSky republishes its users (including this pilot, under
         another id) to OGN; they already arrive through the SafeSky
         port */
      if (settings.ignore_safesky && target.report.safesky_relay)
        continue;

      const FlarmTraffic &traffic = target.report.traffic;
      if (!traffic.location.IsValid() ||
          std::find(own_ids.begin(), own_ids.end(), traffic.id) != own_ids.end())
        continue;

      const double distance = own_location.Distance(traffic.location);
      const unsigned range =
        traffic.type == FlarmTraffic::AircraftType::GLIDER
        ? settings.glider_range
        : settings.other_range;
      if (distance > range)
        continue;

      if (settings.vrange > 0 && traffic.altitude_available && own_altitude &&
          std::fabs(double(traffic.altitude) - *own_altitude) > settings.vrange)
        continue;

      const bool climbing = traffic.climb_rate_received &&
        traffic.climb_rate >= 0.05;
      const unsigned rank = distance <= PRIORITY_RANGE_M
        ? 0u
        : (climbing ? 1u : 2u);
      candidates.push_back({&traffic, rank, distance});
    }
  }

  const auto count = std::min(candidates.size(), TrafficList::MAX_COUNT);
  std::partial_sort(candidates.begin(), candidates.begin() + count,
                    candidates.end(),
                    [](const Candidate &a, const Candidate &b) {
                      if (a.rank != b.rank)
                        return a.rank < b.rank;
                      return a.distance < b.distance;
                    });

  TrivialArray<FlarmId, TrafficList::MAX_COUNT> old_ids;
  old_ids.clear();
  for (const auto &traffic : list.list)
    if (!old_ids.full())
      old_ids.push_back(traffic.id);

  const bool was_empty = list.list.empty();
  list.list.clear();

  bool new_traffic = false;
  for (std::size_t i = 0; i < count; ++i) {
    FlarmTraffic *slot = list.AllocateTraffic();
    if (slot == nullptr)
      break;

    *slot = *candidates[i].traffic;
    slot->valid.Update(now);

    if (std::find(old_ids.begin(), old_ids.end(), slot->id) == old_ids.end())
      new_traffic = true;
  }

  if (new_traffic)
    list.new_traffic.Update(now);

  if (!list.list.empty() || !was_empty)
    list.modified.Update(now);
}

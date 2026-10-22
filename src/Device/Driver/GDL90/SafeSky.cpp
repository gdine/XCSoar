// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "SafeSky.hpp"
#include "FLARM/List.hpp"
#include "Geo/GeoVector.hpp"
#include "Profile/Keys.hpp"
#include "Profile/ProfileMap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

SafeSkySettings safesky_settings;

void
LoadFromProfile(SafeSkySettings &settings) noexcept
{
  settings = SafeSkySettings{};

  Profile::Get(ProfileKeys::SafeSkyHorizontalRange, settings.hrange);
  Profile::Get(ProfileKeys::SafeSkyVerticalRange, settings.vrange);

  settings.hrange = std::clamp(settings.hrange, SafeSkySettings::MIN_HRANGE_M,
                               SafeSkySettings::MAX_HRANGE_M);
  settings.vrange = std::min(settings.vrange, SafeSkySettings::MAX_VRANGE_M);
}

void
SaveToProfile(const SafeSkySettings &settings) noexcept
{
  Profile::Set(ProfileKeys::SafeSkyHorizontalRange, settings.hrange);
  Profile::Set(ProfileKeys::SafeSkyVerticalRange, settings.vrange);
}

void
SafeSkyTraffic::OnHeartbeat(bool gps_valid, TimeStamp now) noexcept
{
  heartbeat_gps_valid = gps_valid;
  heartbeat_time = now;
}

void
SafeSkyTraffic::OnOwnship(uint32_t address, uint8_t address_type,
                          GeoPoint location, bool valid_position,
                          std::optional<double> pressure_altitude,
                          TimeStamp now) noexcept
{
  ownship = Ownship{
    address, address_type, location, pressure_altitude,
    valid_position && location.IsValid(), now,
  };
}

const SafeSkyTraffic::Ownship *
SafeSkyTraffic::GetFreshOwnship(TimeStamp now) const noexcept
{
  if (!ownship || !ownship->valid_position ||
      !ownship->updated.IsDefined() || now - ownship->updated >= STALE)
    return nullptr;

  /* A fresh heartbeat that reports no GPS fix invalidates the
     ownship position. */
  if (heartbeat_gps_valid && !*heartbeat_gps_valid &&
      heartbeat_time.IsDefined() && now - heartbeat_time < STALE)
    return nullptr;

  return &*ownship;
}

void
SafeSkyTraffic::EstimateClimb(Target &target) noexcept
{
  FlarmTraffic &traffic = target.report.traffic;
  target.climb_estimated = false;

  /* Only replace a zero or unknown vertical velocity; SafeSky always
     sends zero. */
  if (traffic.climb_rate_received && traffic.climb_rate != 0)
    return;

  const auto &history = target.history;
  if (history.size() < 3)
    return;

  const TimeStamp start = history.front().time;
  if (history.back().time - start < CLIMB_MIN_SPAN)
    return;

  double mean_t = 0, mean_a = 0;
  for (const auto &sample : history) {
    mean_t += (sample.time - start).count();
    mean_a += sample.altitude;
  }

  mean_t /= history.size();
  mean_a /= history.size();

  double covariance = 0, variance = 0;
  for (const auto &sample : history) {
    const double dt = (sample.time - start).count() - mean_t;
    covariance += dt * (sample.altitude - mean_a);
    variance += dt * dt;
  }

  if (variance <= 0)
    return;

  const double climb = covariance / variance;
  if (!std::isfinite(climb))
    return;

  traffic.climb_rate = climb;
  traffic.climb_rate_received = true;
  target.climb_estimated = true;
}

void
SafeSkyTraffic::OnTraffic(const Report &report, TimeStamp now) noexcept
{
  /* SafeSky may echo the ownship as a traffic report */
  if (ownship && ownship->address == report.address &&
      ownship->address_type == report.address_type)
    return;

  auto i = std::find_if(targets.begin(), targets.end(),
                        [&report](const Target &t) {
                          return t.report.address == report.address &&
                            t.report.address_type == report.address_type;
                        });

  try {
    if (i == targets.end()) {
      if (targets.size() >= MAX_TARGETS) {
        i = std::min_element(targets.begin(), targets.end(),
                             [](const Target &a, const Target &b) {
                               return a.updated < b.updated;
                             });
      } else {
        targets.emplace_back();
        i = std::prev(targets.end());
      }

      i->history.clear();
    }

    Target &target = *i;
    target.report = report;
    target.updated = now;

    auto &history = target.history;
    std::erase_if(history, [now](const Sample &s) {
      return s.time > now || now - s.time > CLIMB_WINDOW;
    });

    if (report.traffic.altitude_available) {
      if (history.size() >= MAX_HISTORY)
        history.erase(history.begin());
      history.push_back({now, double(report.traffic.altitude)});
    }

    EstimateClimb(target);
  } catch (const std::bad_alloc &) {
    targets.clear();
  }
}

void
SafeSkyTraffic::Publish(TrafficList &list, TimeStamp now,
                        const SafeSkySettings &settings) noexcept
{
  std::erase_if(targets, [now](const Target &t) {
    return t.updated > now || now - t.updated >= STALE;
  });

  const Ownship *own = GetFreshOwnship(now);

  struct Candidate {
    const Target *target;
    unsigned rank;
    double distance;
  };

  std::vector<Candidate> candidates;
  try {
    candidates.reserve(targets.size());
  } catch (const std::bad_alloc &) {
    return;
  }

  for (const auto &target : targets) {
    const FlarmTraffic &traffic = target.report.traffic;
    if (!target.report.valid_position || !traffic.location.IsValid())
      continue;

    const bool climbing = traffic.climb_rate_received &&
      traffic.climb_rate >= 0.05;

    if (own == nullptr) {
      /* without a SafeSky ownship there is nothing to filter against;
         prefer climbing, then most recent targets */
      candidates.push_back({&target, climbing ? 1u : 2u,
                            -target.updated.ToDuration().count()});
      continue;
    }

    const double distance = own->location.Distance(traffic.location);
    if (distance > settings.hrange)
      continue;

    if (settings.vrange > 0 && traffic.altitude_available &&
        own->pressure_altitude &&
        std::fabs(double(traffic.altitude) - *own->pressure_altitude) >
        settings.vrange)
      continue;

    const unsigned rank = distance <= PRIORITY_RANGE_M
      ? 0u
      : (climbing ? 1u : 2u);
    candidates.push_back({&target, rank, distance});
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

    *slot = candidates[i].target->report.traffic;
    slot->valid.Update(now);

    if (std::find(old_ids.begin(), old_ids.end(), slot->id) == old_ids.end())
      new_traffic = true;
  }

  if (new_traffic)
    list.new_traffic.Update(now);

  if (!list.list.empty() || !was_empty)
    list.modified.Update(now);
}

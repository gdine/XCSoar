// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "TrafficTraceComputer.hpp"
#include "TraceComputer.hpp"
#include "NMEA/MoreData.hpp"
#include "Engine/Trace/Vector.hpp"
#include "LogFileDecl.hpp"

#include <algorithm>
#include <cmath>

using namespace std::chrono;

void
TrafficTraceComputer::Reset() noexcept
{
  const std::lock_guard lock{mutex};
  targets.clear();
  last_time = last_clock = TimeStamp::Undefined();
}

void
TrafficTraceComputer::SetRetention(seconds timeout) noexcept
{
  const std::lock_guard lock{mutex};
  retention = std::clamp(timeout, seconds{MIN_RETENTION},
                         seconds{MAX_RETENTION});
}

void
TrafficTraceComputer::Update(const MoreData &basic) noexcept
{
  const std::lock_guard lock{mutex};

  if (last_clock.IsDefined() && basic.clock < last_clock)
    targets.clear();

  last_clock = basic.clock;

  std::erase_if(targets, [&basic, this](const auto &item) {
    return item.second.last_received.IsOlderThan(basic.clock, retention);
  });

  const bool have_time = basic.time_available && basic.time.IsDefined();
  if (have_time && last_time.IsDefined() && basic.time < last_time)
    targets.clear();

  if (have_time)
    last_time = basic.time;

  for (const auto &traffic : basic.flarm.traffic.list) {
    if (!traffic.id.IsDefined())
      continue;

    if (traffic.no_track || !traffic.location_available ||
        !traffic.location.IsValid() || !traffic.altitude_available ||
        (!traffic.absolute_location && traffic.relative_north == 0 &&
         traffic.relative_east == 0)) {
      targets.erase(traffic.id);
      continue;
    }

    if (traffic.valid.IsOlderThan(basic.clock, seconds{2}))
      continue;

    auto it = targets.find(traffic.id);
    if (it == targets.end()) {
      if (targets.size() >= MAX_TARGETS) {
        auto oldest = std::min_element(targets.begin(), targets.end(),
          [](const auto &a, const auto &b) {
            return a.second.last_received < b.second.last_received;
          });
        targets.erase(oldest);
      }

      it = targets.try_emplace(traffic.id).first;
    }

    auto &target = it->second;
    auto &previous = target.previous;
    if (previous.valid &&
        (previous.source != traffic.source ||
         previous.id_type != traffic.id_type ||
         !traffic.valid.Modified(previous.valid))) {
      if (previous.source == traffic.source &&
          previous.id_type == traffic.id_type &&
          traffic.valid == previous.valid)
        continue;

      target.segments.clear();
      previous.Clear();
      target.last_received.Clear();
    }

    const bool reception_gap = target.last_received &&
      traffic.valid.GetTimeDifference(target.last_received) > seconds{30};
    target.last_received = traffic.valid;
    if (!have_time)
      continue;

    target.last_report_time = basic.time;

    const bool have_previous = previous.valid &&
      !previous.valid.IsOlderThan(basic.clock, seconds{30});

    /* Online merging refreshes validity even without a new report.
       Keep the last measurement, not the last merge tick, for climb. */
    if (have_previous && FlarmTraffic::IsInjectedSource(traffic.source) &&
        !traffic.climb_rate_received &&
        traffic.location == previous.location &&
        traffic.altitude == previous.altitude)
      continue;

    if (target.segments.empty() || reception_gap || !have_previous) {
      if (target.segments.size() >= MAX_SEGMENTS)
        target.segments.pop_front();
      target.segments.emplace_back(basic.time.Cast<TracePoint::Time>());
    }

    if (traffic.climb_rate_received || (have_previous && !reception_gap)) {
      const double climb = traffic.climb_rate_received
        ? traffic.climb_rate
        : (double(traffic.altitude) - double(previous.altitude)) /
          traffic.valid.GetTimeDifference(previous.valid).count();

      if (std::isfinite(climb)) {
        target.segments.back().trace.push_back(TracePoint(traffic.location,
          basic.time.Cast<TracePoint::Time>(), traffic.altitude, climb, 0));
        unsigned size = 0;
        for (const auto &segment : target.segments)
          size += segment.trace.size();
        while (size > MAX_POINTS && target.segments.size() > 1) {
          size -= target.segments.front().trace.size();
          target.segments.pop_front();
        }
      } else {
        LogFormat("Ignoring non-finite traffic climb rate for %06x",
                  traffic.id.Value());
        target.segments.clear();
      }
    }

    previous = traffic;
  }
}

void
TrafficTraceComputer::LockedCopyTo(FlarmId id, const TrailQuery &query,
                                   TracePointVector &points) const noexcept
{
  const std::lock_guard lock{mutex};
  points.clear();
  if (const auto it = targets.find(id); it != targets.end()) {
    TracePointVector segment_points;
    for (const auto &segment : it->second.segments) {
      segment.trace.GetPoints(segment_points, query.min_time, query.bounds,
                             query.project_location, query.min_distance_m,
                             query.point_stride, query.max_points);
      points.insert(points.end(), segment_points.begin(), segment_points.end());
    }
  }
}

void
TrafficTraceComputer::LockedCopyAll(const TrailQuery &query, TimeStamp now,
                                    std::vector<Snapshot> &snapshots) const
  noexcept
{
  const std::lock_guard lock{mutex};
  snapshots.clear();
  for (const auto &[id, target] : targets) {
    if (target.last_received.IsOlderThan(now, retention))
      continue;

    const auto age = duration_cast<TracePoint::Time>(
      last_time - target.last_report_time);
    const auto min_time = query.min_time > age
      ? query.min_time - age : TracePoint::Time{};
    for (const auto &segment : target.segments) {
      Snapshot snapshot{id, target.previous.source, segment.start_time,
                        min_time, {}};
      segment.trace.GetPoints(snapshot.points, min_time, query.bounds,
                             query.project_location, query.min_distance_m,
                             query.point_stride, query.max_points);
      if (!snapshot.points.empty())
        snapshots.push_back(std::move(snapshot));
    }
  }
}

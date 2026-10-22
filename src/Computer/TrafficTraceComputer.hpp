// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Engine/Trace/Trace.hpp"
#include "Engine/Trace/Vector.hpp"
#include "FLARM/List.hpp"
#include "thread/Mutex.hxx"

#include <list>
#include <map>

struct MoreData;
struct TrailQuery;

/** Bounded, ground-referenced histories, owned by CalculationThread. */
class TrafficTraceComputer {
  struct Segment {
    Trace trace{{}, Trace::null_time, MAX_POINTS};
    TracePoint::Time start_time;

    explicit Segment(TracePoint::Time time) noexcept : start_time(time) {}
  };

  struct Target {
    std::list<Segment> segments;
    FlarmTraffic previous{};
    Validity last_received{};
    TimeStamp last_report_time{};
  };

  mutable Mutex mutex;
  std::map<FlarmId, Target> targets;
  TimeStamp last_time = TimeStamp::Undefined();
  TimeStamp last_clock = TimeStamp::Undefined();
  std::chrono::seconds retention = DEFAULT_RETENTION;

public:
  static constexpr unsigned MAX_POINTS = 1024;
  static constexpr unsigned MAX_TARGETS = TrafficList::MAX_COUNT * 4;
  static constexpr unsigned MAX_SEGMENTS = 16;
  static constexpr auto DEFAULT_RETENTION = std::chrono::hours{1};
  static constexpr auto MIN_RETENTION = std::chrono::minutes{5};
  static constexpr auto MAX_RETENTION = std::chrono::hours{24};
  static constexpr auto RETENTION_STEP = std::chrono::minutes{5};

  struct Snapshot {
    FlarmId id;
    FlarmTraffic::SourceType source;
    TracePoint::Time start_time;
    TracePoint::Time min_time;
    TracePointVector points;
  };

  void Reset() noexcept;
  void SetRetention(std::chrono::seconds timeout) noexcept;
  void Update(const MoreData &basic) noexcept;

  /** Copy one target's time/viewport-filtered history under the mutex. */
  void LockedCopyTo(FlarmId id, const TrailQuery &query,
                    TracePointVector &points) const noexcept;

  /**
   * Copy retained segments, freezing trail length at the last report.
   * Check expiry against now even when calculation/GPS updates have stopped.
   */
  void LockedCopyAll(const TrailQuery &query, TimeStamp now,
                     std::vector<Snapshot> &snapshots) const noexcept;
};

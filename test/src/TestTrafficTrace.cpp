// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Computer/TrafficTraceComputer.hpp"
#include "Computer/TraceComputer.hpp"
#include "Engine/Trace/Vector.hpp"
#include "NMEA/MoreData.hpp"
#include "TestUtil.hpp"

#include <limits>

using namespace std::chrono;

static TimeStamp
At(unsigned seconds) noexcept
{
  return TimeStamp{FloatDuration{seconds}};
}

struct Fixture {
  TrafficTraceComputer computer;
  MoreData basic{};
  TrailQuery query{};

  Fixture() noexcept {
    basic.Reset();
    query.bounds = GeoBounds{
      GeoPoint{Angle::Degrees(7), Angle::Degrees(49)},
      GeoPoint{Angle::Degrees(9), Angle::Degrees(47)}};
    query.project_location =
      GeoPoint{Angle::Degrees(8), Angle::Degrees(48)};
  }

  void SetTime(unsigned time) noexcept {
    basic.time = basic.clock = At(time);
    basic.time_available.Update(basic.clock);
  }

  FlarmTraffic &Add(unsigned id, unsigned time) noexcept {
    auto &traffic = basic.flarm.traffic.list.append();
    traffic = {};
    traffic.id = FlarmId::FromValue(id);
    traffic.source = FlarmTraffic::SourceType::FLARM;
    traffic.id_type = FlarmTraffic::IdType::FLARM;
    traffic.location = query.project_location;
    traffic.location_available = traffic.absolute_location = true;
    traffic.altitude = 1000;
    traffic.altitude_available = true;
    traffic.climb_rate = 2;
    traffic.climb_rate_received = true;
    traffic.valid.Update(At(time));
    return traffic;
  }

  void Sample(FlarmTraffic &traffic, unsigned time) noexcept {
    SetTime(time);
    traffic.valid.Update(basic.clock);
    traffic.location.longitude = Angle::Degrees(8 + time * 0.00001);
    computer.Update(basic);
  }

  TracePointVector Copy(unsigned id) noexcept {
    TracePointVector result;
    computer.LockedCopyTo(FlarmId::FromValue(id), query, result);
    return result;
  }
};

static void
TestRecording() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  auto &b = f.Add(2, 1);
  b.climb_rate = -3;
  f.Sample(a, 1);
  f.Sample(a, 3);
  f.Sample(b, 5);
  auto points = f.Copy(1);
  ok1(points.size() == 2);
  ok1(points.front().GetTime() == seconds{1});
  ok1(points.back().GetTime() == seconds{3});
  ok1(equals(points.back().GetVario(), 2));
  ok1(points.back().GetAltitude() == 1000);
  points = f.Copy(2);
  ok1(points.size() == 2);
  ok1(equals(points.back().GetVario(), -3));
  f.computer.Update(f.basic);
  ok1(f.Copy(2).size() == 2);
  f.Sample(b, 6);
  ok1(f.Copy(2).size() == 2);
  ok1(f.Copy(999).empty());
}

static void
TestCalculatedClimb() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  a.climb_rate_received = false;
  a.climb_rate = 999;
  f.Sample(a, 1);
  ok1(f.Copy(1).empty());
  a.altitude = 1004;
  a.relative_altitude = -100;
  f.Sample(a, 3);
  ok1(f.Copy(1).size() == 1);
  ok1(equals(f.Copy(1).back().GetVario(), 2));
  a.altitude = 1000;
  a.relative_altitude = 100;
  f.Sample(a, 5);
  ok1(f.Copy(1).size() == 2);
  ok1(equals(f.Copy(1).back().GetVario(), -2));
  a.climb_rate_received = true;
  a.climb_rate = 5;
  f.Sample(a, 7);
  ok1(equals(f.Copy(1).back().GetVario(), 5));
}

static void
TestReportedZeroClimb() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  a.source = FlarmTraffic::SourceType::ADSB;
  a.climb_rate = 0;
  f.Sample(a, 1);
  a.altitude = 1040;
  f.Sample(a, 3);
  a.altitude = 1000;
  f.Sample(a, 5);
  const auto points = f.Copy(1);
  ok1(points.size() == 3);
  for (const auto &point : points)
    ok1(equals(point.GetVario(), 0));
}

static void
TestRepeatedOnlineReports() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  a.source = FlarmTraffic::SourceType::OGN;
  a.climb_rate_received = false;
  f.Sample(a, 1);

  /* MergeOnlineTraffic refreshes validity between actual reports. */
  for (unsigned t = 2; t <= 10; ++t) {
    f.SetTime(t);
    a.valid.Update(f.basic.clock);
    f.computer.Update(f.basic);
  }
  ok1(f.Copy(1).empty());

  a.altitude = 1020;
  f.Sample(a, 11);
  auto points = f.Copy(1);
  ok1(points.size() == 1);
  ok1(equals(points.back().GetVario(), 2));

  for (unsigned t = 12; t <= 20; ++t) {
    f.SetTime(t);
    a.valid.Update(f.basic.clock);
    f.computer.Update(f.basic);
  }
  points = f.Copy(1);
  ok1(points.size() == 1);
  ok1(equals(points.back().GetVario(), 2));

  a.altitude = 1010;
  f.Sample(a, 21);
  points = f.Copy(1);
  ok1(points.size() == 2);
  ok1(equals(points.back().GetVario(), -1));

  /* Moving at constant altitude is a genuine zero-climb measurement. */
  f.Sample(a, 23);
  points = f.Copy(1);
  ok1(points.size() == 3);
  ok1(equals(points.back().GetVario(), 0));

  /* A received climb rate is meaningful even at unchanged position. */
  a.climb_rate_received = true;
  a.climb_rate = 3;
  f.SetTime(25);
  a.valid.Update(f.basic.clock);
  f.computer.Update(f.basic);
  points = f.Copy(1);
  ok1(points.size() == 4);
  ok1(equals(points.back().GetVario(), 3));
}

static void
TestPrivacyAndAvailability() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Sample(a, 1);
  f.Sample(a, 3);
  a.no_track = true;
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
  a.no_track = false;
  f.Sample(a, 5);
  ok1(f.Copy(1).size() == 1);
  a.location_available = false;
  f.Sample(a, 7);
  ok1(f.Copy(1).empty());
  a.location_available = true;
  a.absolute_location = false;
  f.Sample(a, 9);
  ok1(f.Copy(1).empty());
  a.relative_north = 100;
  f.Sample(a, 11);
  ok1(f.Copy(1).size() == 1);
  a.altitude_available = false;
  f.Sample(a, 13);
  ok1(f.Copy(1).empty());
  a.altitude_available = true;
  a.climb_rate = std::numeric_limits<double>::quiet_NaN();
  f.Sample(a, 15);
  ok1(f.Copy(1).empty());
  a.climb_rate = 2;
  f.Sample(a, 17);
  ok1(f.Copy(1).size() == 1);
  f.SetTime(21);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 1);
  a.id.Clear();
  f.Sample(a, 23);
  ok1(f.Copy(0).empty());
}

static void
TestDiscontinuities() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Sample(a, 1);
  f.Sample(a, 3);
  f.Sample(a, 33);
  ok1(f.Copy(1).size() == 3);
  f.Sample(a, 64);
  ok1(f.Copy(1).size() == 4);
  a.source = FlarmTraffic::SourceType::OGN;
  f.Sample(a, 66);
  ok1(f.Copy(1).size() == 1);
  a.id_type = FlarmTraffic::IdType::ICAO;
  f.Sample(a, 68);
  ok1(f.Copy(1).size() == 1);
  f.Sample(a, 70);
  ok1(f.Copy(1).size() == 2);
  f.Sample(a, 10);
  ok1(f.Copy(1).size() == 1);
  f.basic.time = At(12);
  f.basic.clock = At(5);
  a.valid.Update(f.basic.clock);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 1);
  f.computer.Reset();
  ok1(f.Copy(1).empty());
  f.basic.time_available.Clear();
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
}

static void
TestBoundsAndBudgets() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  for (unsigned t = 1; t < 4096; t += 2)
    f.Sample(a, t);
  auto points = f.Copy(1);
  ok1(points.size() <= TrafficTraceComputer::MAX_POINTS);
  ok1(points.size() > 2);
  ok1(points.front().GetTime() == seconds{1});
  ok1(points.back().GetTime() == seconds{4095});
  f.query.min_time = seconds{4000};
  points = f.Copy(1);
  ok1(!points.empty());
  ok1(points.front().GetTime() >= seconds{4000});
  f.query.max_points = 10;
  points = f.Copy(1);
  ok1(points.size() <= 10);
  ok1(points.back().GetTime() == seconds{4095});
  f.query.bounds = GeoBounds{
    GeoPoint{Angle::Degrees(7), Angle::Degrees(49)},
    GeoPoint{Angle::Degrees(7.5), Angle::Degrees(47)}};
  points = f.Copy(1);
  ok1(points.size() <= 1);
  f.basic.flarm.traffic.Clear();
  f.SetTime(7696);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
}

static void
TestRetention() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1000);
  f.Sample(a, 1000);
  f.Sample(a, 1002);
  f.basic.flarm.traffic.Clear();
  f.SetTime(4602);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 2);

  std::vector<TrafficTraceComputer::Snapshot> snapshots;
  f.query.min_time = seconds{4002};
  f.computer.LockedCopyAll(f.query, f.basic.clock, snapshots);
  ok1(snapshots.size() == 1);
  ok1(snapshots.front().min_time == seconds{402});
  ok1(snapshots.front().points.size() == 2);
  ok1(snapshots.front().id == FlarmId::FromValue(1));

  auto &returned = f.Add(1, 4602);
  f.Sample(returned, 4602);
  f.query.min_time = {};
  f.computer.LockedCopyAll(f.query, f.basic.clock, snapshots);
  ok1(snapshots.size() == 2);
  ok1(snapshots.front().points.size() == 2);
  ok1(snapshots.back().points.size() == 1);
  ok1(f.Copy(1).size() == 3);

  f.basic.flarm.traffic.Clear();
  f.SetTime(4603);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 3);
  f.SetTime(8202);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 3);
  f.SetTime(8203);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
  f.computer.LockedCopyAll(f.query, f.basic.clock, snapshots);
  ok1(snapshots.empty());
}

static void
TestUnchangedReportRetention() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  a.source = FlarmTraffic::SourceType::OGN;
  a.climb_rate_received = false;
  f.Sample(a, 1);
  a.altitude = 1004;
  f.Sample(a, 3);
  f.SetTime(30);
  a.valid.Update(f.basic.clock);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 1);
  f.basic.flarm.traffic.Clear();
  f.SetTime(3630);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 1);
  f.SetTime(3631);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
}

static void
TestStaleReportRetention() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Sample(a, 1);
  f.SetTime(3601);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 1);
  f.SetTime(3602);
  f.basic.time_available.Clear();
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
}

static void
TestIndependentTimeouts() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Add(2, 1);
  f.SetTime(1);
  f.computer.Update(f.basic);
  f.Sample(a, 3500);
  ok1(f.Copy(1).size() == 2);
  ok1(f.Copy(2).size() == 1);
  f.SetTime(3602);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 2);
  ok1(f.Copy(2).empty());
  f.SetTime(7100);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 2);
  f.SetTime(7101);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());
}

static void
TestDisplayExpiryWithoutGPS() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 80000);
  f.Sample(a, 80000);
  f.Sample(a, 80002);
  f.basic.time_available.Clear();
  f.basic.location_available.Clear();
  std::vector<TrafficTraceComputer::Snapshot> snapshots;

  /* No Update() calls: both blackboard clocks stay at the last fix. */
  f.computer.LockedCopyAll(f.query, At(83602), snapshots);
  ok1(snapshots.size() == 1);
  ok1(snapshots.front().points.size() == 2);
  f.computer.LockedCopyAll(f.query, At(83603), snapshots);
  ok1(snapshots.empty());
  f.computer.LockedCopyAll(f.query, At(108000), snapshots);
  ok1(snapshots.empty());
  ok1(f.Copy(1).size() == 2);

  /* A new report renews the timeout, not a redraw of old data. */
  f.SetTime(108000);
  a.valid.Update(f.basic.clock);
  a.location.longitude = Angle::Degrees(8);
  f.computer.Update(f.basic);
  f.computer.LockedCopyAll(f.query, At(111600), snapshots);
  ok1(snapshots.size() == 1);
  f.computer.LockedCopyAll(f.query, At(111601), snapshots);
  ok1(snapshots.empty());
}

static void
TestReportRenewsWithoutGPS() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Sample(a, 1);
  f.Sample(a, 3);
  f.basic.time_available.Clear();
  f.basic.clock = At(3500);
  a.valid.Update(f.basic.clock);
  f.computer.Update(f.basic);
  std::vector<TrafficTraceComputer::Snapshot> snapshots;
  f.computer.LockedCopyAll(f.query, At(3604), snapshots);
  ok1(snapshots.size() == 1);
  ok1(snapshots.front().points.size() == 2);
  f.computer.LockedCopyAll(f.query, At(7100), snapshots);
  ok1(snapshots.size() == 1);
  f.computer.LockedCopyAll(f.query, At(7101), snapshots);
  ok1(snapshots.empty());
}

static void
TestDisplayExpiryPerTarget() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Add(2, 1);
  f.SetTime(1);
  f.computer.Update(f.basic);
  f.Sample(a, 3500);
  std::vector<TrafficTraceComputer::Snapshot> snapshots;
  f.computer.LockedCopyAll(f.query, At(3602), snapshots);
  ok1(snapshots.size() == 2);
  ok1(snapshots.front().id == FlarmId::FromValue(1));
  ok1(snapshots.back().id == FlarmId::FromValue(1));
  f.computer.LockedCopyAll(f.query, At(7101), snapshots);
  ok1(snapshots.empty());
}

static void
TestConfigurableTimeout() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  f.Sample(a, 1);
  f.Sample(a, 3);
  f.computer.SetRetention(minutes{5});
  std::vector<TrafficTraceComputer::Snapshot> snapshots;
  f.computer.LockedCopyAll(f.query, At(303), snapshots);
  ok1(snapshots.size() == 1);
  f.computer.LockedCopyAll(f.query, At(304), snapshots);
  ok1(snapshots.empty());

  f.computer.SetRetention(hours{24});
  f.computer.LockedCopyAll(f.query, At(86403), snapshots);
  ok1(snapshots.size() == 1);
  f.computer.LockedCopyAll(f.query, At(86404), snapshots);
  ok1(snapshots.empty());
  f.basic.flarm.traffic.Clear();
  f.SetTime(86403);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).size() == 2);
  f.SetTime(86404);
  f.computer.Update(f.basic);
  ok1(f.Copy(1).empty());

  f.basic.flarm.traffic.Clear();
  auto &b = f.Add(2, 86405);
  f.Sample(b, 86405);
  f.computer.SetRetention(minutes{5});
  f.basic.flarm.traffic.Clear();
  f.SetTime(86706);
  f.computer.Update(f.basic);
  ok1(f.Copy(2).empty());
}

static void
TestSegmentBudgets() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  for (unsigned segment = 0; segment < 20; ++segment)
    for (unsigned point = 0; point < 80; ++point)
      f.Sample(a, 1 + segment * 200 + point * 2);
  ok1(f.Copy(1).size() <= TrafficTraceComputer::MAX_POINTS);
  std::vector<TrafficTraceComputer::Snapshot> snapshots;
  f.computer.LockedCopyAll(f.query, f.basic.clock, snapshots);
  ok1(snapshots.size() <= TrafficTraceComputer::MAX_SEGMENTS);

  f.computer.Reset();
  for (unsigned segment = 0; segment < 20; ++segment) {
    f.Sample(a, 1 + segment * 40);
    f.Sample(a, 3 + segment * 40);
  }
  f.computer.LockedCopyAll(f.query, f.basic.clock, snapshots);
  ok1(snapshots.size() == TrafficTraceComputer::MAX_SEGMENTS);
  ok1(f.Copy(1).size() == 2 * TrafficTraceComputer::MAX_SEGMENTS);
}

static void
TestCalculatedClimbAfterGap() noexcept
{
  Fixture f;
  auto &a = f.Add(1, 1);
  a.climb_rate_received = false;
  f.Sample(a, 1);
  a.altitude = 1004;
  f.Sample(a, 3);
  a.altitude = 1100;
  f.Sample(a, 100);
  ok1(f.Copy(1).size() == 1);
  a.altitude = 1104;
  f.Sample(a, 102);
  std::vector<TrafficTraceComputer::Snapshot> snapshots;
  f.computer.LockedCopyAll(f.query, f.basic.clock, snapshots);
  ok1(snapshots.size() == 2);
  ok1(snapshots.front().points.size() == 1);
  ok1(snapshots.back().points.size() == 1);
  ok1(equals(snapshots.back().points.front().GetVario(), 2));
}

static void
TestTargetBudget() noexcept
{
  Fixture f;
  for (unsigned batch = 0; batch < 4; ++batch) {
    f.basic.flarm.traffic.Clear();
    for (unsigned i = 1; i <= TrafficList::MAX_COUNT; ++i)
      f.Add(batch * TrafficList::MAX_COUNT + i, batch + 1);
    f.SetTime(batch + 1);
    f.computer.Update(f.basic);
  }
  bool all_present = true;
  for (unsigned i = 1; i <= TrafficTraceComputer::MAX_TARGETS; ++i)
    all_present &= f.Copy(i).size() == 1;
  ok1(all_present);
  f.basic.flarm.traffic.Clear();
  f.Add(1000, 5);
  f.SetTime(5);
  f.computer.Update(f.basic);
  ok1(f.Copy(1000).size() == 1);
  unsigned retained = 0;
  for (unsigned i = 1; i <= TrafficTraceComputer::MAX_TARGETS; ++i)
    retained += !f.Copy(i).empty();
  ok1(retained == TrafficTraceComputer::MAX_TARGETS - 1);
}

int
main()
{
  plan_tests(10 + 6 + 4 + 11 + 10 + 9 + 10 + 13 + 3 + 2 + 6 +
             7 + 4 + 4 + 7 + 4 + 5 + 3);
  TestRecording();
  TestCalculatedClimb();
  TestReportedZeroClimb();
  TestRepeatedOnlineReports();
  TestPrivacyAndAvailability();
  TestDiscontinuities();
  TestBoundsAndBudgets();
  TestRetention();
  TestUnchangedReportRetention();
  TestStaleReportRetention();
  TestIndependentTimeouts();
  TestDisplayExpiryWithoutGPS();
  TestReportRenewsWithoutGPS();
  TestDisplayExpiryPerTarget();
  TestConfigurableTimeout();
  TestSegmentBudgets();
  TestCalculatedClimbAfterGap();
  TestTargetBudget();
  return exit_status();
}

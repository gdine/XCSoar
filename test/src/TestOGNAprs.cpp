// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Device/Driver/OGN/Aprs.hpp"
#include "Device/Driver/OGN/Settings.hpp"
#include "Device/Driver/OGN/Traffic.hpp"
#include "FLARM/List.hpp"
#include "TestUtil.hpp"
#include "Units/System.hpp"
#include "util/StringAPI.hxx"

#include <cmath>
#include <string>

OGNSettings ogn_settings;

static std::string
Aprs(const char *address = "DDA5BA", const char *flags = "06",
     const char *latitude = "4800.60N", const char *longitude = "00200.60E",
     const char *altitude = "006562", const char *time = "155413h",
     const char *tocall = "OGFLR", const char *extra = "")
{
  return std::string("FLR") + address + ">" + tocall + ",qAS,LFMX:/" + time +
    latitude + "/" + longitude + "'090/060/A=" + altitude +
    " !W00! id" + flags + address + " -198fpm +0.1rot gps2x3" + extra;
}

static bool
Near(double a, double b, double tolerance = 1e-6)
{
  return std::fabs(a - b) < tolerance;
}

static void
TestParseFlarmGlider()
{
  const auto r = OGN::ParseAprsPosition(
    "ICA38E75B>OGFLR,qAS,LFMX1:/155412h4400.29N/00603.59E'330/055/A=004390 "
    "!W04! id0538E75B -157fpm +0.1rot 17.0dB +12.1kHz gps5x5\r\n");
  ok1(r.has_value());
  if (!r)
    return;

  ok1(r->address == 0x38E75B);
  ok1(!r->safesky_relay);
  ok1(r->beacon_time == 15u * 3600 + 54 * 60 + 12);

  const FlarmTraffic &t = r->traffic;
  ok1(t.id == FlarmId::Parse("38E75B", nullptr));
  ok1(t.id_type == FlarmTraffic::IdType::ICAO);
  ok1(t.source == FlarmTraffic::SourceType::FLARM);
  ok1(t.type == FlarmTraffic::AircraftType::GLIDER);
  ok1(t.absolute_location && t.location_available);
  ok1(Near(t.location.latitude.Degrees(), 44 + 0.290 / 60, 1e-7));
  ok1(Near(t.location.longitude.Degrees(), 6 + 3.594 / 60, 1e-7));
  ok1(t.altitude_available && Near(t.altitude, 4390 * 0.3048, 1));
  ok1(t.track_received && Near(Angle(t.track).Degrees(), 330, 1));
  ok1(t.speed_received && Near(t.speed, 55 * 0.5144444444, 0.1));
  ok1(t.climb_rate_received && Near(t.climb_rate, -157 * 0.3048 / 60, 0.001));
  ok1(t.name.empty());
}

static void
TestParseAdsb()
{
  const auto r = OGN::ParseAprsPosition(
    "ICA4632B2>OGADSB,qAS,AVX1134:/155412h4538.72N\\00333.81E^161/478/A=039215 "
    "!W23! id214632B2 -1472fpm FL387.07 A1:ABF4 Sq6342");
  ok1(r.has_value());
  if (!r)
    return;

  ok1(r->traffic.source == FlarmTraffic::SourceType::ADSB);
  ok1(r->traffic.type == FlarmTraffic::AircraftType::POWERED_AIRCRAFT);
  ok1(StringIsEqual(r->traffic.name.c_str(), "ABF4"));
  ok1(Near(r->traffic.location.longitude.Degrees(), 3 + 33.813 / 60, 1e-7));
}

static void
TestParseRejects()
{
  ok1(!OGN::ParseAprsPosition(
        "chaudefon>OGNSDR,TCPIP*,qAC,GLIDERN2:/155413h4704.96NI00648.33E&/A=003350"));
  ok1(!OGN::ParseAprsPosition(
        "Bardo>OGNDVS,TCPIP*,qAC,GLIDERN2:>155412h 10:0 2.546s/68ms 33dB"));
  ok1(!OGN::ParseAprsPosition("# aprsc 2.1.20-gf.5-g0178a1b"));
  ok1(!OGN::ParseAprsPosition(""));
  ok1(!OGN::ParseAprsPosition("garbage"));
  /* no-tracking flag */
  ok1(!OGN::ParseAprsPosition(Aprs("DDA5BA", "46")));
  ok1(!OGN::ParseAprsPosition(Aprs("DDA5BA", "06", "9100.00N")));
}

static void
TestParseVariants()
{
  auto r = OGN::ParseAprsPosition(Aprs("DDA5BA", "06", "3330.00S",
                                       "07030.00W", "-00100"));
  ok1(r.has_value());
  if (r) {
    ok1(Near(r->traffic.location.latitude.Degrees(), -33.5));
    ok1(Near(r->traffic.location.longitude.Degrees(), -70.5));
    ok1(Near(r->traffic.altitude, -30.48, 1));
  }

  r = OGN::ParseAprsPosition(Aprs("DDA5BA", "06", "4800.60N", "00200.60E",
                                  "006562", "221545z"));
  ok1(r.has_value() && r->beacon_time == 15u * 3600 + 45 * 60);

  r = OGN::ParseAprsPosition(Aprs("F53667", "06", "4800.60N", "00200.60E",
                                  "006562", "162335h", "OGNSKY"));
  ok1(r.has_value() && r->safesky_relay);
}

static void
TestBeaconOrder()
{
  ok1(OGN::IsOlderBeacon(100, 101));
  ok1(!OGN::IsOlderBeacon(101, 100));
  ok1(!OGN::IsOlderBeacon(100, 100));
  /* midnight wrap-around */
  ok1(OGN::IsOlderBeacon(86399, 2));
  ok1(!OGN::IsOlderBeacon(2, 86399));
}

static void
TestCallsign()
{
  ok1(OGN::MakeAprsCallsign("F-CGCN", 7) == "F0CGCN-7");
  ok1(OGN::MakeAprsCallsign("f-cgcn", 15) == "F0CGCN-15");
  ok1(OGN::MakeAprsCallsign("F-CGCN", 16) == "F0CGCN-0");

  const auto longer = OGN::MakeAprsCallsign("HB-12345", 15);
  ok1(longer.size() <= OGN::MAX_CALLSIGN_LENGTH);
  ok1(longer.starts_with("HB01234-"));

  const auto none = OGN::MakeAprsCallsign("", 1234);
  ok1(none.starts_with("XCS"));
  ok1(none.size() <= OGN::MAX_CALLSIGN_LENGTH);

  ok1(OGN::MakeAprsRangeFilter(43.68, 7.11, 220) == "r/43.6800/7.1100/220");
}

static TimeStamp
At(double seconds)
{
  return TimeStamp{FloatDuration{seconds}};
}

static OGN::AprsReport
Report(const char *address, const char *flags, const char *latitude,
       const char *time = "155413h", const char *tocall = "OGFLR")
{
  return *OGN::ParseAprsPosition(Aprs(address, flags, latitude, "00200.00E",
                                      "006562", time, tocall));
}

static const GeoPoint own(Angle::Degrees(2), Angle::Degrees(48));

static void
TestRanges()
{
  OGNTraffic cache;
  OGNSettings settings;
  /* ~150 km north: glider kept, powered aircraft hidden; ~5 km:
     powered aircraft kept */
  cache.Ingest(Report("000001", "06", "4921.00N"), At(1));
  cache.Ingest(Report("000002", "22", "4921.00N"), At(1));
  cache.Ingest(Report("000003", "22", "4802.70N"), At(1));

  TrafficList list;
  list.Clear();
  cache.Publish(list, At(1), own, 2000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 2);
  ok1(list.FindTraffic(FlarmId::Parse("000001", nullptr)) != nullptr);
  ok1(list.FindTraffic(FlarmId::Parse("000002", nullptr)) == nullptr);
  ok1(list.FindTraffic(FlarmId::Parse("000003", nullptr)) != nullptr);
  /* near target ranked first */
  ok1(list.list[0].id == FlarmId::Parse("000003", nullptr));

  settings.glider_range = 100000;
  settings.other_range = 0;
  cache.Publish(list, At(1), own, 2000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 0);

  /* no own position: nothing */
  settings = {};
  cache.Publish(list, At(1), GeoPoint::Invalid(), 2000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 0);

  /* own ids are excluded */
  const FlarmId own_ids[] = {FlarmId::Parse("000003", nullptr)};
  cache.Publish(list, At(1), own, 2000., own_ids, settings);
  ok1(list.GetActiveTrafficCount() == 1);

  /* vertical filter: target at 2000 m (6562 ft) */
  settings.vrange = 500;
  cache.Publish(list, At(1), own, 1000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 0);
  cache.Publish(list, At(1), own, 1700., {}, settings);
  ok1(list.GetActiveTrafficCount() == 2);

  /* stale targets expire */
  settings = {};
  cache.Publish(list, At(100), own, 2000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 0);
  ok1(cache.GetTargetCount() == 0);
}

static void
TestSafeSkyAndDedup()
{
  OGNTraffic cache;
  OGNSettings settings;
  TrafficList list;
  list.Clear();

  cache.Ingest(Report("F53667", "06", "4801.00N", "162335h", "OGNSKY"), At(1));
  cache.Publish(list, At(1), own, 2000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 0);
  settings.ignore_safesky = false;
  cache.Publish(list, At(1), own, 2000., {}, settings);
  ok1(list.GetActiveTrafficCount() == 1);

  settings = {};
  ok1(cache.Ingest(Report("DDA5BA", "06", "4801.00N", "155413h"), At(1)) ==
      OGNTraffic::IngestResult::ACCEPTED);
  ok1(cache.Ingest(Report("DDA5BA", "06", "4805.00N", "155412h"), At(1.5)) ==
      OGNTraffic::IngestResult::OLDER);
  cache.Publish(list, At(2), own, 2000., {}, settings);
  const auto *t = list.FindTraffic(FlarmId::Parse("DDA5BA", nullptr));
  ok1(t != nullptr &&
      Near(t->location.latitude.Degrees(), 48 + 1. / 60, 1e-6));
}

int
main()
{
  plan_tests(64);

  TestParseFlarmGlider();
  TestParseAdsb();
  TestParseRejects();
  TestParseVariants();
  TestBeaconOrder();
  TestCallsign();
  TestRanges();
  TestSafeSkyAndDedup();

  return exit_status();
}

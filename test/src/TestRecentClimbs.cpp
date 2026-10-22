// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "RecentClimbs/Update.hpp"
#include "RecentClimbs/Settings.hpp"
#include "Formatter/TimeFormatter.hpp"
#include "Units/System.hpp"
#include "Engine/Waypoint/Waypoints.hpp"
#include "TestUtil.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <limits>

using std::string_view_literals::operator""sv;

static constexpr std::string_view HEADER =
  "name,code,country,lat,lon,elev,style,rwdir,rwlen,freq,desc\n";

static std::string
Cup(std::string_view lines)
{
  return std::string{HEADER} + std::string{lines};
}

static void
TestResolveDownloadUrl()
{
  constexpr auto direct =
    "https://drive.google.com/uc?export=download&id=1AbC-d_9"sv;

  ok1(RecentClimbs::ResolveDownloadUrl(
        "https://drive.google.com/file/d/1AbC-d_9/view?usp=sharing") == direct);
  ok1(RecentClimbs::ResolveDownloadUrl(
        "  https://drive.google.com/file/d/1AbC-d_9/view  ") == direct);
  ok1(RecentClimbs::ResolveDownloadUrl(
        "https://drive.google.com/open?id=1AbC-d_9") == direct);
  ok1(RecentClimbs::ResolveDownloadUrl(
        "https://drive.google.com/uc?export=download&id=1AbC-d_9") == direct);

  /* other URLs are used unchanged */
  ok1(RecentClimbs::ResolveDownloadUrl(
        "https://example.com/climbs.cup?id=5") ==
      "https://example.com/climbs.cup?id=5"sv);
  ok1(RecentClimbs::ResolveDownloadUrl(
        "https://drive.google.com/drive/folders") ==
      "https://drive.google.com/drive/folders"sv);
}

static void
TestParse()
{
  const auto climbs = RecentClimbs::Parse(Cup(
    "\"C1\",\"C1\",,4530.000N,00600.000E,1500.0m,1,,,,\"+2.5 m/s\"\n"
    "\"Field\",\"F\",,4531.000N,00601.000E,500.0m,2,090,800m,,\"\"\n"));

  ok1(climbs.size() == 2);

  bool climb_ok = false, field_ok = false;
  for (const auto &wp : climbs) {
    if (wp.name == "C1")
      climb_ok = wp.type == Waypoint::Type::THERMAL_HOTSPOT &&
        wp.origin == WaypointOrigin::NONE &&
        wp.has_elevation && wp.elevation == 1500 &&
        wp.comment == "+2.5 m/s";
    else if (wp.name == "Field")
      field_ok = wp.IsLandable();
  }
  ok1(climb_ok);
  ok1(field_ok);

  ok1(RecentClimbs::Parse(std::string{HEADER}).empty());

  bool html_rejected = false;
  try {
    RecentClimbs::Parse("\n <!DOCTYPE html><html>Sign in</html>");
  } catch (const std::runtime_error &) {
    html_rejected = true;
  }
  ok1(html_rejected);
}

static bool
Contains(const Waypoints &waypoints, std::string_view name)
{
  return waypoints.LookupName(std::string{name}) != nullptr;
}

static void
TestUpdate()
{
  Waypoints waypoints;
  {
    Waypoint own{GeoPoint{Angle::Degrees(6), Angle::Degrees(45)}};
    own.name = "Own";
    own.origin = WaypointOrigin::USER;
    waypoints.Append(std::move(own));
    waypoints.Optimise();
  }

  std::vector<RecentClimbs::Entry> current;

  auto r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.000N,00600.000E,1500.0m,1,,,,\n"
    "\"B\",\"B\",,4535.000N,00605.000E,1600.0m,1,,,,\n")));
  ok1(r.added == 2 && r.removed == 0 && r.kept == 0);
  ok1(waypoints.size() == 3 && current.size() == 2);

  const auto a = waypoints.LookupName("A");
  const unsigned a_id = a != nullptr ? a->id : 0;

  /* A stays (same pointer and id), B disappears, C is new */
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"C\",\"C\",,4540.000N,00610.000E,1700.0m,1,,,,\n"
    "\"A\",\"A\",,4530.000N,00600.000E,1500.0m,1,,,,\n")));
  ok1(r.added == 1 && r.removed == 1 && r.kept == 1);
  ok1(waypoints.size() == 3);
  ok1(Contains(waypoints, "A") && !Contains(waypoints, "B") &&
      Contains(waypoints, "C") && Contains(waypoints, "Own"));
  ok1(waypoints.LookupName("A") == a && a->id == a_id);

  /* a moved climb with the same name is replaced */
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"C\",\"C\",,4540.000N,00610.000E,1700.0m,1,,,,\n"
    "\"A\",\"A\",,4530.500N,00600.000E,1500.0m,1,,,,\n")));
  ok1(r.added == 1 && r.removed == 1 && r.kept == 1);
  ok1(waypoints.LookupName("A") != a);

  /* unchanged file: nothing to do */
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.500N,00600.000E,1500.0m,1,,,,\n"
    "\"C\",\"C\",,4540.000N,00610.000E,1700.0m,1,,,,\n")));
  ok1(r.added == 0 && r.removed == 0 && r.kept == 2 && !r.IsModified());

  /* a climb without elevation in the file stays unchanged even if
     its stored elevation came from terrain (which varies with the
     loaded terrain tiles) */
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.500N,00600.000E,1500.0m,1,,,,\n"
    "\"C\",\"C\",,4540.000N,00610.000E,1700.0m,1,,,,\n"
    "\"N\",\"N\",,4541.000N,00611.000E,,1,,,,\n")));
  ok1(r.added == 1 && r.kept == 2);
  if (const auto n = waypoints.LookupName("N"); n != nullptr) {
    auto &stored = const_cast<Waypoint &>(*n);
    stored.elevation = 812;
    stored.has_elevation = true;
  }
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.500N,00600.000E,1500.0m,1,,,,\n"
    "\"C\",\"C\",,4540.000N,00610.000E,1700.0m,1,,,,\n"
    "\"N\",\"N\",,4541.000N,00611.000E,,1,,,,\n")));
  ok1(r.kept == 3 && !r.IsModified());

  /* duplicates in the file are kept as separate waypoints */
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.500N,00600.000E,1500.0m,1,,,,\n"
    "\"A\",\"A\",,4530.500N,00600.000E,1500.0m,1,,,,\n"
    "\"C\",\"C\",,4540.000N,00610.000E,1700.0m,1,,,,\n")));
  ok1(r.added == 1 && r.kept == 2 && waypoints.size() == 4);

  /* empty file removes all climbs but never the user's waypoints */
  r = RecentClimbs::Update(waypoints, current, {});
  ok1(r.removed == 3 && current.empty());
  ok1(waypoints.size() == 1 && Contains(waypoints, "Own"));

  /* waypoint database reloaded behind our back: stale pointers are
     dropped instead of erased, and the climbs are added again */
  RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.000N,00600.000E,1500.0m,1,,,,\n")));
  waypoints.Clear();
  r = RecentClimbs::Update(waypoints, current, RecentClimbs::Parse(Cup(
    "\"A\",\"A\",,4530.000N,00600.000E,1500.0m,1,,,,\n")));
  ok1(r.added == 1 && r.removed == 0 && r.kept == 0);
  ok1(waypoints.size() == 1 && current.size() == 1);
}

static void
TestAgeAndRate()
{
  using namespace std::chrono;
  const auto now = ParseISO8601Utc("2026-10-19T16:00:00Z");
  const auto parsed = RecentClimbs::Parse(Cup(
    "\"Boundary\",,,4530.000N,00600.000E,,1,,,,"
    "\"Avg vario: +2.5 m/s; Observed: 2026-10-19 15:00:00 UTC\"\n"
    "\"Old\",,,4531.000N,00600.000E,,1,,,,"
    "\"Avg vario: +1.0 m/s; Observed: 2026-10-19 14:59:59 UTC\"\n"
    "\"Fresh\",,,4532.000N,00600.000E,,1,,,,"
    "\"Avg vario: +0.0 m/s; Observed: 2026-10-19 15:45:00 UTC\"\n"
    "\"Unknown\",,,4533.000N,00600.000E,,1,,,,\"Avg vario: +3.0 m/s\"\n"
    "\"Invalid\",,,4534.000N,00600.000E,,1,,,,"
    "\"Avg vario: nan m/s; Observed: 2026-02-30 15:00:00 UTC\"\n"
    "\"Future\",,,4535.000N,00600.000E,,1,,,,"
    "\"Avg vario: -1.0 m/s; Observed: 2026-10-19 16:00:01 UTC\"\n"));
  ok1(parsed.size() == 6);
  const auto selected = RecentClimbs::Select(parsed, now, minutes{60});
  ok1(selected.climbs.size() == 2);
  ok1(selected.unknown_age == 2);
  ok1(selected.next_expiry == now + seconds{1});
  bool boundary = false, zero = false, invalid_rate = false;
  for (const auto &wp : parsed) {
    if (wp.name == "Boundary")
      boundary = wp.recent_climb &&
        wp.recent_climb->observed == now - minutes{60} &&
        wp.recent_climb->lift_rate == 2.5;
    if (wp.name == "Fresh")
      zero = wp.recent_climb && wp.recent_climb->lift_rate == 0.;
    if (wp.name == "Future")
      invalid_rate = wp.recent_climb && !wp.recent_climb->lift_rate;
  }
  ok1(boundary);
  ok1(zero);
  ok1(invalid_rate);
  ok1(RecentClimbs::Select(parsed, now, minutes{15}).climbs.size() == 1);
  ok1(RecentClimbs::Select(parsed, now, minutes{75}).climbs.size() == 3);
  ok1(RecentClimbs::Select(parsed, now + seconds{1}, minutes{60})
      .climbs.size() == 2);
  const auto expired =
    RecentClimbs::Select(parsed, now + hours{25}, minutes{60});
  ok1(expired.climbs.empty());
  ok1(expired.next_expiry == system_clock::time_point::max());

  Waypoints waypoints;
  std::vector<RecentClimbs::Entry> current;
  auto selection = RecentClimbs::Select(parsed, now, minutes{60});
  RecentClimbs::Update(waypoints, current, std::move(selection.climbs));
  selection = RecentClimbs::Select(parsed, now + hours{2}, minutes{60});
  const auto result =
    RecentClimbs::Update(waypoints, current, std::move(selection.climbs));
  ok1(result.removed == 2 && current.empty() && waypoints.IsEmpty());

  const auto midnight = ParseISO8601Utc("2026-10-20T00:15:00Z");
  const auto overnight = RecentClimbs::Parse(Cup(
    "\"Yesterday\",,,4530.000N,00600.000E,,1,,,,"
    "\"Observed: 2026-10-19 23:15:00 UTC\"\n"
    "\"DayOld\",,,4531.000N,00600.000E,,1,,,,"
    "\"Observed: 2026-10-19 00:15:00 UTC; Avg vario: inf m/s\"\n"));
  ok1(RecentClimbs::Select(overnight, midnight, minutes{60})
      .climbs.size() == 1);
  ok1(RecentClimbs::Select(overnight, midnight, minutes{1440})
      .climbs.size() == 2);
  ok1(RecentClimbs::Select(overnight, midnight + seconds{1}, minutes{1440})
      .climbs.size() == 1);
  ok1(overnight.size() == 2 &&
      !overnight.front().recent_climb->lift_rate &&
      !overnight.back().recent_climb->lift_rate);
}

static void
TestMinimumRate()
{
  using namespace std::chrono;
  const auto now = ParseISO8601Utc("2026-10-20T06:00:00Z");
  const auto parsed = RecentClimbs::Parse(Cup(
    "\"Weak\",,,4530.000N,00600.000E,,1,,,,"
    "\"Avg vario: +0.9 m/s; Observed: 2026-10-20 05:30:00 UTC\"\n"
    "\"Boundary\",,,4531.000N,00600.000E,,1,,,,"
    "\"Avg vario: +1.0 m/s; Observed: 2026-10-20 05:30:00 UTC\"\n"
    "\"Strong\",,,4532.000N,00600.000E,,1,,,,"
    "\"Avg vario: +2.5 m/s; Observed: 2026-10-20 05:30:00 UTC\"\n"
    "\"Unknown\",,,4533.000N,00600.000E,,1,,,,"
    "\"Observed: 2026-10-20 05:30:00 UTC\"\n"
    "\"Invalid\",,,4534.000N,00600.000E,,1,,,,"
    "\"Avg vario: nan m/s; Observed: 2026-10-20 05:30:00 UTC\"\n"
    "\"Zero\",,,4535.000N,00600.000E,,1,,,,"
    "\"Avg vario: +0.0 m/s; Observed: 2026-10-20 05:30:00 UTC\"\n"));
  const auto selected = RecentClimbs::Select(parsed, now, minutes{60}, 1.);
  ok1(selected.climbs.size() == 2);
  ok1(selected.unknown_rate == 2);
  ok1(selected.unknown_age == 0);
  ok1(selected.next_expiry == now + minutes{30} + seconds{1});
  for (const auto &wp : selected.climbs)
    ok1(wp.name == "Boundary" || wp.name == "Strong");
  ok1(RecentClimbs::Select(parsed, now, minutes{60}, 0).climbs.size() == 6);
  ok1(RecentClimbs::Select(parsed, now, minutes{60}, 0).unknown_rate == 0);
  ok1(RecentClimbs::Select(parsed, now, minutes{60}, 2.5).climbs.size() == 1);
  ok1(RecentClimbs::Select(parsed, now, minutes{60}, 2.6).climbs.empty());
  ok1(RecentClimbs::Select(parsed, now, minutes{15}, 1.).climbs.empty());
  ok1(RecentClimbs::Select(parsed, now + hours{1}, minutes{60}, 1.)
      .climbs.empty());

  Waypoints waypoints;
  std::vector<RecentClimbs::Entry> current;
  auto selection = RecentClimbs::Select(parsed, now, minutes{60}, 0);
  RecentClimbs::Update(waypoints, current, std::move(selection.climbs));
  selection = RecentClimbs::Select(parsed, now, minutes{60}, 1.);
  auto result =
    RecentClimbs::Update(waypoints, current, std::move(selection.climbs));
  ok1(result.removed == 4 && result.kept == 2 && current.size() == 2);
  selection = RecentClimbs::Select(parsed, now, minutes{60}, 0);
  result = RecentClimbs::Update(waypoints, current, std::move(selection.climbs));
  ok1(result.added == 4 && result.kept == 2 && current.size() == 6);

  const auto imperial = Units::ToUserUnit(1., Unit::FEET_PER_MINUTE);
  ok1(imperial > 196 && imperial < 197);
  ok1(equals(Units::ToSysUnit(imperial, Unit::FEET_PER_MINUTE), 1.));
  const auto rate = Units::ToSysUnit(200., Unit::FEET_PER_MINUTE);
  ok1(RecentClimbs::Select(parsed, now, minutes{60}, rate).climbs.size() == 1);
  ok1(Units::ToUserUnit(1., Unit::METER_PER_SECOND) == 1.);
}

static void
TestIndependentServices()
{
  using namespace std::chrono;
  const auto now = ParseISO8601Utc("2026-10-21T12:00:00Z");
  const auto body = Cup(
    "\"Shared-name\",,,4530.000N,00600.000E,1500m,1,,,,"
    "\"Observed: 2026-10-21 11:30:00 UTC; Avg vario: +2.0 m/s\"\n"
    "\"Older\",,,4531.000N,00600.000E,1600m,1,,,,"
    "\"Observed: 2026-10-21 10:30:00 UTC; Avg vario: +1.0 m/s\"\n");
  const auto downloaded = RecentClimbs::Parse(body);
  Waypoints waypoints;
  std::vector<RecentClimbs::Entry> recent, collaborative;
  auto selection = RecentClimbs::Select(downloaded, now, minutes{60});
  RecentClimbs::Update(waypoints, recent, std::move(selection.climbs));
  selection = RecentClimbs::Select(downloaded, now, minutes{120});
  RecentClimbs::Update(waypoints, collaborative, std::move(selection.climbs));
  ok1(recent.size() == 1 && collaborative.size() == 2);
  ok1(waypoints.size() == 3);
  const auto recent_wp = recent.front().waypoint;
  const auto group_wp = std::find_if(
    collaborative.begin(), collaborative.end(), [](const auto &entry) {
      return entry.parsed.name == "Shared-name";
    })->waypoint;
  ok1(recent_wp != group_wp);
  ok1(recent_wp->id != group_wp->id);
  ok1(recent_wp->recent_climb && group_wp->recent_climb &&
      recent_wp->type == Waypoint::Type::THERMAL_HOTSPOT &&
      group_wp->type == Waypoint::Type::THERMAL_HOTSPOT);

  auto result = RecentClimbs::Update(waypoints, recent, {});
  ok1(result.removed == 1 && waypoints.size() == 2);
  ok1(collaborative.size() == 2 && std::any_of(
    collaborative.begin(), collaborative.end(), [&](const auto &entry) {
      return entry.waypoint == group_wp;
    }));
  selection = RecentClimbs::Select(downloaded, now, minutes{120}, 1.5);
  result = RecentClimbs::Update(waypoints, collaborative,
                                std::move(selection.climbs));
  ok1(result.removed == 1 && result.kept == 1 && waypoints.size() == 1);
  ok1(collaborative.front().waypoint == group_wp);

  selection = RecentClimbs::Select(downloaded, now, minutes{60});
  RecentClimbs::Update(waypoints, recent, std::move(selection.climbs));
  ok1(recent.size() == 1 && waypoints.size() == 2);
  const auto restored_recent = recent.front().waypoint;
  result = RecentClimbs::Update(waypoints, collaborative, {});
  ok1(result.removed == 1 && waypoints.size() == 1);
  ok1(recent.front().waypoint == restored_recent);
  selection = RecentClimbs::Select(downloaded, now, minutes{120});
  result = RecentClimbs::Update(waypoints, collaborative,
                                std::move(selection.climbs));
  ok1(result.added == 2 && waypoints.size() == 3);
  ok1(recent.front().waypoint == restored_recent);
  selection = RecentClimbs::Select(downloaded, now + minutes{31}, minutes{60});
  result = RecentClimbs::Update(waypoints, recent, std::move(selection.climbs));
  ok1(result.removed == 1 && collaborative.size() == 2 &&
      waypoints.size() == 2);

  waypoints.Clear();
  selection = RecentClimbs::Select(downloaded, now, minutes{60});
  result = RecentClimbs::Update(waypoints, recent, std::move(selection.climbs));
  ok1(result.added == 1 && result.removed == 0);
  selection = RecentClimbs::Select(downloaded, now, minutes{120});
  result = RecentClimbs::Update(waypoints, collaborative,
                                std::move(selection.climbs));
  ok1(result.added == 2 && result.removed == 0 && waypoints.size() == 3);
  RecentClimbs::Update(waypoints, recent, {});
  RecentClimbs::Update(waypoints, collaborative, {});
  ok1(waypoints.IsEmpty());
}

static void
TestSettings()
{
  RecentClimbsSettings settings;
  settings.SetDefaults();
  ok1(settings.max_age == 60);
  ok1(RecentClimbsSettings::MIN_MAX_AGE == 15 &&
      RecentClimbsSettings::MAX_MAX_AGE == 1440 &&
      RecentClimbsSettings::MAX_AGE_STEP == 15);
  ok1(RecentClimbsSettings::ClampMaxAge(0) == 15);
  ok1(RecentClimbsSettings::ClampMaxAge(61) == 60);
  ok1(RecentClimbsSettings::ClampMaxAge(1441) == 1440);
  for (unsigned age = 15; age <= 1440; age += 15)
    ok1(RecentClimbsSettings::ClampMaxAge(age) == age);
  ok1(settings.min_climb_rate == 0);
  ok1(RecentClimbsSettings::ClampMinClimbRate(-1.) == 0);
  ok1(RecentClimbsSettings::ClampMinClimbRate(1.5) == 1.5);
  ok1(RecentClimbsSettings::ClampMinClimbRate(
        std::numeric_limits<double>::quiet_NaN()) == 0);
  ok1(RecentClimbsSettings::ClampMinClimbRate(
        std::numeric_limits<double>::infinity()) == 0);
}

int
main()
{
  plan_tests(6 + 5 + 16 + 17 + 5 + 96 + 18 + 5 + 18);

  TestResolveDownloadUrl();
  TestParse();
  TestUpdate();
  TestAgeAndRate();
  TestMinimumRate();
  TestSettings();
  TestIndependentServices();

  return exit_status();
}

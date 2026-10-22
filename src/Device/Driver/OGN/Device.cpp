// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Device.hpp"
#include "Settings.hpp"
#include "Device/PeriodicContext.hpp"
#include "Device/Port/Port.hpp"
#include "NMEA/Info.hpp"
#include "Operation/Operation.hpp"
#include "LogFile.hpp"

#include <random>

bool
OGNDevice::DataReceived(std::span<const std::byte> s,
                        NMEAInfo &info) noexcept
try {
  const std::lock_guard lock{mutex};
  last_received = info.clock;

  for (const std::byte b : s) {
    const char ch = static_cast<char>(b);
    if (ch == '\n') {
      LineReceived(line_buffer, info.clock);
      line_buffer.clear();
    } else if (line_buffer.size() < MAX_LINE_LENGTH) {
      line_buffer.push_back(ch);
    }
  }

  /* the traffic list is published by OnPeriodicUpdate() */
  return false;
} catch (const std::bad_alloc &) {
  line_buffer.clear();
  return false;
}

void
OGNDevice::LineReceived(std::string_view line, TimeStamp now) noexcept
{
  while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
    line.remove_suffix(1);

  if (line.empty())
    return;

  if (line.front() == '#') {
    if (line.starts_with("# logresp")) {
      login_acknowledged = true;
      if (!login_response_logged) {
        login_response_logged = true;
        LogFormat("OGN: %.*s", int(line.size()), line.data());
      }
    }

    return;
  }

  ++n_lines;

  if (const auto report = OGN::ParseAprsPosition(line)) {
    ++n_positions;
    traffic.Ingest(*report, now);
  }
}

bool
OGNDevice::Send(std::string_view line) noexcept
try {
  NullOperationEnvironment env;
  port.FullWrite(line, env, std::chrono::seconds(2));
  return true;
} catch (...) {
  LogError(std::current_exception(), "OGN: failed to send to server");
  return false;
}

static std::string
SanitizeToken(const char *s)
{
  std::string result;
  for (; *s != '\0'; ++s)
    result.push_back(*s == ' ' || *s == '\t' ? '_' : *s);
  if (result.empty())
    result = "unknown";
  return result;
}

void
OGNDevice::UpdateLogin(const DevicePeriodicContext &context,
                       TimeStamp now) noexcept
try {
  const unsigned radius_km = ogn_settings.GetFilterRadiusKm();
  const GeoPoint location = context.location;

  if (!login_sent) {
    std::random_device random;
    callsign = OGN::MakeAprsCallsign(context.registration.c_str(), random());

    std::string login = "user " + callsign + " pass -1 vers XCSoar " +
      SanitizeToken(context.version);
    if (location.IsValid()) {
      login += " filter ";
      login += OGN::MakeAprsRangeFilter(location.latitude.Degrees(),
                                        location.longitude.Degrees(),
                                        radius_km);
      filter_center = location;
      filter_radius_km = radius_km;
    }

    LogFormat("OGN: %s", login.c_str());
    login += "\r\n";
    if (!Send(login))
      return;

    login_sent = true;
    last_keepalive = now;
    return;
  }

  if (location.IsValid() &&
      (!filter_center.IsValid() || radius_km != filter_radius_km ||
       filter_center.Distance(location) > REFILTER_DISTANCE_M)) {
    std::string filter = "#filter " +
      OGN::MakeAprsRangeFilter(location.latitude.Degrees(),
                               location.longitude.Degrees(),
                               radius_km);
    LogFormat("OGN: %s", filter.c_str());
    filter += "\r\n";
    if (Send(filter)) {
      filter_center = location;
      filter_radius_km = radius_km;
      last_keepalive = now;
    }
  }

  if (!last_keepalive.IsDefined() || now - last_keepalive >= KEEPALIVE_INTERVAL) {
    if (Send("#keepalive\r\n"))
      last_keepalive = now;
  }
} catch (...) {
  LogError(std::current_exception(), "OGN: login failed");
}

bool
OGNDevice::OnPeriodicUpdate(const DevicePeriodicContext &context,
                            NMEAInfo &info) noexcept
{
  const TimeStamp now = info.clock;

  UpdateLogin(context, now);

  const std::lock_guard lock{mutex};

  if (last_received.IsDefined() && now >= last_received &&
      now - last_received < ALIVE_TIMEOUT)
    info.alive.Update(now);

  traffic.Publish(info.flarm.traffic, now, context.location, context.altitude,
                  context.own_ids, ogn_settings);

  if (!last_stats.IsDefined() || now - last_stats >= STATS_INTERVAL) {
    if (last_stats.IsDefined())
      LogFormat("OGN: %s logged in=%d lines=%u positions=%u cached=%u "
                "published=%u",
                callsign.c_str(), int(login_acknowledged), n_lines,
                n_positions, unsigned(traffic.GetTargetCount()),
                unsigned(info.flarm.traffic.list.size()));
    last_stats = now;
  }

  return true;
}

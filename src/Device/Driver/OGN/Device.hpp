// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Device/Driver.hpp"
#include "Traffic.hpp"
#include "Geo/GeoPoint.hpp"
#include "thread/Mutex.hxx"
#include "time/Stamp.hpp"

#include <string>
#include <string_view>

/**
 * Receives OGN traffic from an APRS-IS server (aprs.glidernet.org).
 *
 * Lines are parsed into a cache on the port thread; the cache is
 * published to the traffic list twice per second by
 * OnPeriodicUpdate(), which also sends the login, the range filter
 * around the own position and keepalives.
 */
class OGNDevice final : public AbstractDevice {
  static constexpr FloatDuration ALIVE_TIMEOUT = std::chrono::seconds(60);
  static constexpr FloatDuration KEEPALIVE_INTERVAL = std::chrono::seconds(240);
  static constexpr FloatDuration STATS_INTERVAL = std::chrono::minutes(5);
  static constexpr double REFILTER_DISTANCE_M = 20000;
  static constexpr std::size_t MAX_LINE_LENGTH = 2048;

  Port &port;

  /** Protects the attributes below */
  Mutex mutex;
  OGNTraffic traffic;
  TimeStamp last_received = TimeStamp::Undefined();
  bool login_acknowledged = false;
  bool login_response_logged = false;
  unsigned n_lines = 0, n_positions = 0;

  /** Only accessed by the port thread */
  std::string line_buffer;

  /* only accessed by the main thread */
  std::string callsign;
  bool login_sent = false;
  GeoPoint filter_center = GeoPoint::Invalid();
  unsigned filter_radius_km = 0;
  TimeStamp last_keepalive = TimeStamp::Undefined();
  TimeStamp last_stats = TimeStamp::Undefined();

public:
  explicit OGNDevice(Port &_port) noexcept
    :port(_port) {}

  /* virtual methods from class Device */
  bool DataReceived(std::span<const std::byte> s,
                    NMEAInfo &info) noexcept override;
  bool OnPeriodicUpdate(const DevicePeriodicContext &context,
                        NMEAInfo &info) noexcept override;

private:
  void LineReceived(std::string_view line, TimeStamp now) noexcept;
  bool Send(std::string_view line) noexcept;
  void UpdateLogin(const DevicePeriodicContext &context,
                   TimeStamp now) noexcept;
};

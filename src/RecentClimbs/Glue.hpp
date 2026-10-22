// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Update.hpp"
#include "net/AsyncTask.hpp"
#include "thread/Mutex.hxx"
#include "time/PeriodClock.hpp"
#include "ui/event/Notify.hpp"

#include <optional>
#include <string>
#include <vector>

struct RecentClimbsSettings;
class CurlGlobal;

/**
 * Periodically downloads one service's CUP file of detected climbs and
 * keeps the matching temporary waypoints (WaypointOrigin::NONE) in the
 * waypoint database in sync with it.
 * Separate instances own independent caches, filters and waypoint handles,
 * even when two sources contain identical entries.
 *
 * All public methods must be called from the UI thread.
 */
class RecentClimbsGlue {
public:
  enum class Source {
    RECENT,
    COLLABORATIVE,
  };

private:
  CurlGlobal &curl;
  const Source source;

  Net::AsyncTask task;

  UI::Notify download_notify{[this]{ OnDownloadNotification(); }};

  PeriodClock clock;

  /**
   * The configured URL the current/last download was started for.
   */
  std::string active_url;

  /**
   * Climbs added to the waypoint database (UI thread only).
   */
  std::vector<RecentClimbs::Entry> current;
  std::vector<Waypoint> downloaded;
  unsigned active_max_age = 0;
  double active_min_climb_rate = 0;
  std::chrono::system_clock::time_point next_expiry =
    std::chrono::system_clock::time_point::max();

  mutable Mutex mutex;

  /**
   * Downloaded body waiting to be applied (protected by #mutex).
   */
  std::optional<std::string> pending_body;
  std::string pending_url;

public:
  explicit RecentClimbsGlue(CurlGlobal &_curl,
                           Source _source = Source::RECENT) noexcept;
  ~RecentClimbsGlue() noexcept;

  RecentClimbsGlue(const RecentClimbsGlue &) = delete;
  RecentClimbsGlue &operator=(const RecentClimbsGlue &) = delete;

  void BeginShutdown() noexcept;

  void OnTimer(const RecentClimbsSettings &settings) noexcept;

private:
  RecentClimbsSettings GetSettings() const;
  const char *GetLogName() const noexcept;
  void CancelDownload() noexcept;
  void RemoveAll() noexcept;
  void Refresh(const RecentClimbsSettings &settings) noexcept;

  Co::InvokeTask Download(std::string configured_url);
  void OnCompletion(std::exception_ptr error) noexcept;
  void OnDownloadNotification() noexcept;
};

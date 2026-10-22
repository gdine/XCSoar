// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Core.hpp"
#include "Settings.hpp"
#include "net/AsyncTask.hpp"
#include "thread/Mutex.hxx"
#include "time/PeriodClock.hpp"
#include "ui/event/Notify.hpp"

#include <memory>

class CurlGlobal;
struct NMEAInfo;
struct DerivedInfo;

namespace CollaborativeFlight {

class Worker;

/** UI-thread coordinator; CPU/disk work and HTTP never run on the UI thread. */
class Glue {
  CurlGlobal &curl;
  Net::AsyncTask task;
  std::unique_ptr<Worker> worker;
  UI::Notify completion_notify{[this]{ OnCompletionNotification(); }};
  PeriodClock work_clock, upload_clock;
  CollaborativeFlightSettings active;
  std::optional<Clock::time_point> last_fix;
  std::optional<Batch> in_flight;
  bool cpu_busy = false;
  unsigned generation = 0;
  Mutex completion_mutex;
  unsigned completed_generation = 0;
  unsigned running_generation = 0;
  std::exception_ptr completed_error;

public:
  explicit Glue(CurlGlobal &curl);
  ~Glue() noexcept;
  void BeginShutdown() noexcept;
  void OnTimer(const CollaborativeFlightSettings &settings,
               const NMEAInfo &basic, const DerivedInfo &calculated) noexcept;

private:
  Co::InvokeTask Upload(CollaborativeFlightSettings settings, Batch batch);
  void OnUploadCompletion(std::exception_ptr error) noexcept;
  void OnCompletionNotification() noexcept;
};

} // namespace CollaborativeFlight

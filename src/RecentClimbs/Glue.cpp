// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Glue.hpp"
#include "Settings.hpp"
#include "Update.hpp"
#include "Components.hpp"
#include "DataComponents.hpp"
#include "Engine/Waypoint/Waypoints.hpp"
#include "Terrain/RasterTerrain.hpp"
#include "Protection.hpp"
#include "Interface.hpp"
#include "LogFile.hpp"
#include "net/State.hpp"
#include "lib/curl/CoRequest.hxx"
#include "lib/curl/Easy.hxx"
#include "lib/curl/Global.hxx"
#include "lib/curl/Setup.hxx"
#include "co/InvokeTask.hxx"

#include <utility>

/**
 * Refuse files larger than this; a list of recent climbs is tiny.
 */
static constexpr curl_off_t MAX_FILE_SIZE = 1024 * 1024;

RecentClimbsGlue::RecentClimbsGlue(CurlGlobal &_curl, Source _source) noexcept
  :curl(_curl),
   source(_source),
   task(curl.GetEventLoop())
{
}

RecentClimbsSettings
RecentClimbsGlue::GetSettings() const
{
  const auto &settings = CommonInterface::GetComputerSettings();
  return source == Source::COLLABORATIVE
    ? settings.collaborative_flight.GetDisplaySettings()
    : settings.recent_climbs;
}

const char *
RecentClimbsGlue::GetLogName() const noexcept
{
  return source == Source::COLLABORATIVE
    ? "Collaborative climbs" : "Recent climbs";
}

RecentClimbsGlue::~RecentClimbsGlue() noexcept
{
  BeginShutdown();
}

void
RecentClimbsGlue::BeginShutdown() noexcept
{
  task.BeginShutdown();
  download_notify.ClearNotification();
}

void
RecentClimbsGlue::CancelDownload() noexcept
{
  task.Cancel();
  download_notify.ClearNotification();
  const std::lock_guard lock{mutex};
  pending_body.reset();
  pending_url.clear();
}

void
RecentClimbsGlue::RemoveAll() noexcept
{
  downloaded.clear();
  active_max_age = 0;
  active_min_climb_rate = 0;
  next_expiry = std::chrono::system_clock::time_point::max();
  if (current.empty() || data_components == nullptr ||
      data_components->waypoints == nullptr) {
    current.clear();
    return;
  }

  RecentClimbs::UpdateResult result;
  {
    const ScopeSuspendAllThreads suspend;
    result = RecentClimbs::Update(*data_components->waypoints, current, {});
  }

  LogFmt("{}: removed {} waypoints", GetLogName(), result.removed);
  if (result.IsModified())
    TriggerMapUpdate();
}

void
RecentClimbsGlue::Refresh(const RecentClimbsSettings &settings) noexcept
{
  if (data_components == nullptr || data_components->waypoints == nullptr)
    return;

  active_max_age = RecentClimbsSettings::ClampMaxAge(settings.max_age);
  active_min_climb_rate =
    RecentClimbsSettings::ClampMinClimbRate(settings.min_climb_rate);
  auto selected = RecentClimbs::Select(downloaded,
                                      std::chrono::system_clock::now(),
                                      std::chrono::minutes{active_max_age},
                                      active_min_climb_rate);
  next_expiry = selected.next_expiry;
  if (selected.unknown_age > 0)
    LogFmt("{}: hiding {} entries with missing or invalid "
           "observation time", GetLogName(), selected.unknown_age);
  if (selected.unknown_rate > 0)
    LogFmt("{}: hiding {} entries with missing or invalid "
           "climb rate", GetLogName(), selected.unknown_rate);

  RecentClimbs::UpdateResult result;
  {
    const ScopeSuspendAllThreads suspend;
    result = RecentClimbs::Update(*data_components->waypoints, current,
                                  std::move(selected.climbs),
                                  data_components->terrain.get());
  }

  LogFmt("{}: {} added, {} removed, {} unchanged",
         GetLogName(), result.added, result.removed, result.kept);
  if (result.IsModified())
    TriggerMapUpdate();
}

void
RecentClimbsGlue::OnTimer(const RecentClimbsSettings &settings) noexcept
{
  if (task.IsShuttingDown())
    return;

  if (!settings.enabled || settings.url.empty()) {
    CancelDownload();
    active_url.clear();
    clock.Reset();
    RemoveAll();
    return;
  }

  if (settings.url != active_url) {
    /* new file: drop a download of the old one and fetch at once */
    CancelDownload();
    RemoveAll();
    active_url = settings.url;
    clock.Reset();
  }

  if (active_max_age !=
      RecentClimbsSettings::ClampMaxAge(settings.max_age) ||
      active_min_climb_rate !=
      RecentClimbsSettings::ClampMinClimbRate(settings.min_climb_rate) ||
      std::chrono::system_clock::now() >= next_expiry)
    Refresh(settings);

  if (GetNetState() == NetState::DISCONNECTED)
    return;

  if (task.IsRunning())
    return;

  if (!clock.CheckUpdate(RecentClimbsSettings::ClampInterval(settings.interval)))
    return;

  task.Start(Download(settings.url), BIND_THIS_METHOD(OnCompletion));
}

Co::InvokeTask
RecentClimbsGlue::Download(std::string configured_url)
{
  const auto url = RecentClimbs::ResolveDownloadUrl(configured_url);

  CurlEasy easy{url.c_str()};
  Curl::Setup(easy);
  easy.SetFailOnError();
  /* Google Drive answers with a redirect to the actual content */
  easy.SetOption(CURLOPT_FOLLOWLOCATION, 1L);
  easy.SetOption(CURLOPT_MAXREDIRS, 5L);
  easy.SetOption(CURLOPT_MAXFILESIZE_LARGE, MAX_FILE_SIZE);
  easy.SetTimeout(60);

  auto response = co_await Curl::CoRequest(curl, std::move(easy));

  {
    const std::lock_guard lock{mutex};
    pending_body = std::move(response.body);
    pending_url = std::move(configured_url);
  }

  download_notify.SendNotification();
}

void
RecentClimbsGlue::OnCompletion(std::exception_ptr error) noexcept
{
  if (error) {
    LogFmt("{}: download failed", GetLogName());
    LogError(error);
  }
}

void
RecentClimbsGlue::OnDownloadNotification() noexcept
{
  std::optional<std::string> body;
  std::string url;
  {
    const std::lock_guard lock{mutex};
    body = std::exchange(pending_body, std::nullopt);
    url = std::move(pending_url);
  }

  if (!body || task.IsShuttingDown())
    return;

  const auto settings = GetSettings();
  if (!settings.enabled || url != settings.url)
    /* settings changed while downloading */
    return;

  if (data_components == nullptr || data_components->waypoints == nullptr)
    return;

  std::vector<Waypoint> climbs;
  try {
    climbs = RecentClimbs::Parse(*body);
  } catch (...) {
    LogFmt("{}: invalid CUP file", GetLogName());
    LogError(std::current_exception());
    return;
  }

  downloaded = std::move(climbs);
  Refresh(settings);
}

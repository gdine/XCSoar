// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Glue.hpp"
#include "NMEA/Info.hpp"
#include "NMEA/Derived.hpp"
#include "LocalPath.hpp"
#include "LogFile.hpp"
#include "thread/StandbyThread.hpp"
#include "system/FileUtil.hpp"
#include "system/SystemLoad.hpp"
#include "io/FileReader.hxx"
#include "io/FileOutputStream.hxx"
#include "util/SpanCast.hxx"
#include "lib/curl/Global.hxx"
#include "lib/curl/CoRequest.hxx"
#include "lib/curl/Setup.hxx"
#include "lib/curl/Slist.hxx"
#include "co/InvokeTask.hxx"
#include "net/State.hpp"

#include <random>
#include <utility>

namespace CollaborativeFlight {

static constexpr curl_off_t MAX_RESPONSE_SIZE = 65536;

static CurlEasy
CreateRequest(const char *url)
{
  CurlEasy easy{url};
  Curl::Setup(easy);
  easy.SetVerifyHost(true);
  easy.SetVerifyPeer(true);
#ifndef ANDROID
  /* Android's bundled curl is built with CURL_DISABLE_NETRC. */
  easy.SetOption(CURLOPT_NETRC, long{CURL_NETRC_IGNORED});
#endif
  easy.SetOption(CURLOPT_PROTOCOLS_STR, "https");
  easy.SetOption(CURLOPT_MAXFILESIZE_LARGE, MAX_RESPONSE_SIZE);
  easy.SetXferInfoFunction(
    [](void *, curl_off_t, curl_off_t downloaded,
       curl_off_t, curl_off_t) noexcept {
      return downloaded > MAX_RESPONSE_SIZE ? 1 : 0;
    }, nullptr);
  easy.SetTimeout(30);
  easy.SetFailOnError();
  return easy;
}

class Worker final : public StandbyThread {
  const AllocatedPath directory;
  CollaborativeFlightSettings settings;
  std::vector<Sample> incoming;
  std::vector<std::string> acknowledgements;
  std::optional<Batch> prepared;
  std::string prepared_folder;
  bool reset = false, defer = false;

  Detector detector;
  Queue queue;
  std::string loaded_folder, saved;
  AllocatedPath path;

public:
  Worker() :StandbyThread("CollabFlight"),
            directory(GetPrimaryDataPath()) {}
  ~Worker() noexcept { LockStop(); }

  void Submit(const CollaborativeFlightSettings &s,
              const std::optional<Sample> &sample, bool busy, bool trigger)
  {
    const std::lock_guard lock{mutex};
    if (!s.HasSameUploadConfiguration(settings))
      prepared.reset();
    if (!s.enabled || settings.folder_url != s.folder_url) {
      reset = true;
      incoming.clear();
    }
    settings = s;
    defer = busy;
    if (sample) {
      if (incoming.size() >= 120) {
        incoming.clear();
        reset = true;
        LogString("Collaborative flight: CPU backlog; discarded raw fixes");
      }
      incoming.push_back(*sample);
    }
    if (trigger && !IsBusy())
      Trigger();
  }

  std::optional<Batch> TakeBatch(std::string_view folder)
  {
    const std::lock_guard lock{mutex};
    if (prepared_folder != folder)
      return {};
    return std::exchange(prepared, std::nullopt);
  }

  void Acknowledge(const std::vector<std::string> &ids)
  {
    const std::lock_guard lock{mutex};
    acknowledgements.insert(acknowledgements.end(), ids.begin(), ids.end());
  }

private:
  void Load(std::string_view folder)
  {
    if (folder == loaded_folder)
      return;

    const std::string name = "collaborative-flight-" + std::string{folder} +
      ".json";
    path = AllocatedPath::Build(directory, name.c_str());
    Queue next;
    if (File::Exists(path)) {
      FileReader reader{path};
      if (reader.GetSize() > 1024 * 1024)
        throw std::runtime_error("Collaborative queue file exceeds 1 MiB");
      std::string body(reader.GetSize(), '\0');
      reader.ReadFull(std::as_writable_bytes(std::span{body}));
      next = Queue::Parse(body);
    } else {
      std::random_device random;
      for (unsigned i = 0; i < 4; ++i) {
        if (i)
          next.client += '-';
        next.client += std::to_string(random());
      }
    }
    queue = std::move(next);
    loaded_folder = folder;
    saved.clear();
    detector = Detector{};
  }

  void Tick() noexcept override
  {
    SetIdlePriority();
    auto s = settings;
    auto samples = std::exchange(incoming, {});
    auto ack = std::exchange(acknowledgements, {});
    const bool should_reset = std::exchange(reset, false);
    const bool busy = defer;
    std::optional<Batch> next;
    {
      const ScopeUnlock unlock{mutex};
      try {
        if (should_reset)
          detector.Reset();
        if (s.enabled) {
          ValidateUploadUrl(s.upload_url);
          ValidatePublicIdentity(s.public_identity);
          const auto folder = FolderId(s.folder_url);
          Load(folder);
          const auto now = Clock::now();
          const unsigned expired = queue.Prune(now);
          if (expired)
            LogFmt("Collaborative flight: expired {} queued climb summaries",
                   expired);
          queue.Acknowledge(ack);
          for (auto &pending : queue.climbs)
            if (!pending.delivered && pending.public_identity.empty())
              pending.public_identity = s.public_identity;
          for (const auto &sample : samples) {
            if (sample.time > now + std::chrono::minutes{5} ||
                sample.time < now - Queue::RETENTION)
              throw std::runtime_error("GPS and device UTC clocks disagree");
            detector.Add(sample);
          }

          if (!busy)
            for (const auto &climb : detector.Detect(now))
              queue.Add(climb, s.public_identity);

          const auto body = queue.Serialize();
          if (body != saved) {
            FileOutputStream output{path};
            output.Write(AsBytes(body));
            output.Sync();
            output.Commit();
            saved = body;
          }
          if (!busy) {
            auto batch = MakeBatch(queue, folder, s.token, now);
            if (!batch.climb_ids.empty())
              next = std::move(batch);
          } else if (s.debug) {
            LogString("Collaborative flight: deferred work; CPU load >= 80%");
          }
        }
      } catch (...) {
        LogError(std::current_exception(),
                 "Collaborative flight: background processing failed");
      }
    }
    if (settings.HasSameUploadConfiguration(s))
      prepared = std::move(next);
    prepared_folder = s.folder_url;
  }
};

Glue::Glue(CurlGlobal &_curl)
  :curl(_curl), task(curl.GetEventLoop()), worker(new Worker) {}

Glue::~Glue() noexcept
{
  BeginShutdown();
}

void
Glue::BeginShutdown() noexcept
{
  task.BeginShutdown();
  worker.reset();
}

void
Glue::OnTimer(const CollaborativeFlightSettings &settings,
              const NMEAInfo &basic, const DerivedInfo &calculated) noexcept
{
  if (task.IsShuttingDown())
    return;

  const bool changed = !settings.HasSameUploadConfiguration(active);
  if (changed) {
    task.Cancel();
    ++generation;
    in_flight.reset();
    active = settings;
    upload_clock.Reset();
    work_clock.Reset();
    last_fix.reset();
  }
  if (!settings.enabled) {
    if (changed) {
      try {
        worker->Submit(settings, {}, false, true);
      } catch (...) {
        LogError(std::current_exception(),
                 "Collaborative flight: stopping detector failed");
      }
    }
    return;
  }

  const bool tick = work_clock.CheckUpdate(std::chrono::seconds{15});
  if (tick) {
    const auto load = SystemLoadCPU();
    cpu_busy = load && *load >= 80;
  }

  std::optional<Sample> sample;
  if (basic.gps.real && !basic.gps.replay && !basic.gps.simulator &&
      basic.alive && basic.location_available && basic.time_available &&
      basic.gps_altitude_available && basic.date_time_utc.IsPlausible() &&
      calculated.flight.flying) {
    const auto time = basic.date_time_utc.ToTimePoint();
    if (!last_fix || time > *last_fix) {
      /* UTC seconds also bound the sampling rate to one fix per second. */
      last_fix = time;
      sample = Sample{time, basic.location, basic.gps_altitude};
    }
  }

  try {
    worker->Submit(settings, sample, cpu_busy, tick);
  } catch (...) {
    LogError(std::current_exception(),
             "Collaborative flight: scheduling detector failed");
    return;
  }

  if (cpu_busy || task.IsRunning() || in_flight ||
      GetNetState() == NetState::DISCONNECTED)
    return;
  const auto interval =
    CollaborativeFlightSettings::ClampInterval(settings.interval);
  if (upload_clock.IsDefined() && upload_clock.Elapsed() < interval)
    return;

  auto batch = worker->TakeBatch(settings.folder_url);
  if (!batch)
    return;
  upload_clock.Update();
  if (settings.debug)
    LogFmt("Collaborative flight: uploading {} summaries ({} bytes)",
           batch->climb_ids.size(), batch->body.size());
  in_flight = *batch;
  {
    const std::lock_guard lock{completion_mutex};
    running_generation = ++generation;
  }
  task.Start(Upload(settings, std::move(*batch)),
             BIND_THIS_METHOD(OnUploadCompletion));
}

void
Glue::OnUploadCompletion(std::exception_ptr error) noexcept
{
  {
    const std::lock_guard lock{completion_mutex};
    completed_generation = running_generation;
    completed_error = error;
  }
  completion_notify.SendNotification();
}

Co::InvokeTask
Glue::Upload(CollaborativeFlightSettings settings, Batch batch)
{
  ValidateUploadUrl(settings.upload_url);
  auto easy = CreateRequest(settings.upload_url.c_str());
  CurlSlist headers;
  headers.Append("Content-Type: application/json");
  easy.SetRequestHeaders(headers.Get());
  easy.SetPost();
  easy.SetRequestBody(batch.body);
  auto response = co_await Curl::CoRequest(curl, std::move(easy));
  if (response.status == 302 || response.status == 303) {
    const auto location = response.headers.find("location");
    if (location == response.headers.end() ||
        !location->second.starts_with("https://script.googleusercontent.com/"))
      throw std::runtime_error("Unexpected Apps Script response redirect");
    /* ContentService redirects to a GET; never forward the secret/body. */
    auto result = CreateRequest(location->second.c_str());
    response = co_await Curl::CoRequest(curl, std::move(result));
  }
  if (response.status != 200)
    throw std::runtime_error("Collaborative upload returned non-200 status");
  ValidateAcknowledgement(response.body, batch);
}

void
Glue::OnCompletionNotification() noexcept
{
  std::exception_ptr error;
  unsigned serial;
  {
    const std::lock_guard lock{completion_mutex};
    serial = completed_generation;
    error = std::exchange(completed_error, {});
  }
  if (task.IsShuttingDown() || serial != generation || !in_flight)
    return;
  if (error) {
    LogError(error, "Collaborative flight: upload failed; retained for retry");
  } else {
    worker->Acknowledge(in_flight->climb_ids);
    if (active.debug)
      LogFmt("Collaborative flight: acknowledged {} summaries",
             in_flight->climb_ids.size());
  }
  in_flight.reset();
}

} // namespace CollaborativeFlight

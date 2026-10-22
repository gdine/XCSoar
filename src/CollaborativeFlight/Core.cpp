// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Core.hpp"
#include "json/Serialize.hxx"
#include "io/StringOutputStream.hxx"
#include "util/UTF8.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace CollaborativeFlight {

void
ValidatePublicIdentity(std::string_view identity)
{
  if (identity.empty() || !ValidateUTF8(identity) ||
      TruncateStringUTF8(identity, 10) != identity.size() ||
      std::any_of(identity.begin(), identity.end(), [](unsigned char c) {
        return c < 32 || c == 127;
      }) ||
      identity.find_first_not_of(' ') == std::string_view::npos)
    throw std::runtime_error("Public identity must be 1-10 Unicode characters");
}

std::string
DefaultPublicIdentity(std::string_view pilot, std::string_view registration)
{
  std::string identity{pilot};
  if (!registration.empty()) {
    if (!identity.empty())
      identity += ' ';
    identity += registration;
  }
  if (identity.empty())
    identity = "CF";
  if (!ValidateUTF8(identity))
    throw std::runtime_error("Invalid UTF-8 in pilot name or registration");
  identity.resize(TruncateStringUTF8(identity, 10));
  ValidatePublicIdentity(identity);
  return identity;
}

static int64_t
Seconds(Clock::time_point time) noexcept
{
  return std::chrono::duration_cast<std::chrono::seconds>(
    time.time_since_epoch()).count();
}

static double
Elapsed(Clock::time_point a, Clock::time_point b) noexcept
{
  return std::chrono::duration<double>(a - b).count();
}

std::string
Climb::Id(std::string_view client) const
{
  return std::string{client} + "-" + std::to_string(Seconds(start));
}

static bool
Valid(const Sample &p) noexcept
{
  return p.location.Check() && std::isfinite(p.altitude) &&
    p.altitude >= -1000 && p.altitude <= 20000 && Seconds(p.time) > 0;
}

void
Detector::Add(const Sample &sample)
{
  if (!Valid(sample))
    throw std::runtime_error("Invalid collaborative flight fix");

  if (!points.empty()) {
    const double dt = Elapsed(sample.time, points.back().time);
    if (dt <= 0)
      return;
  }
  if (points.size() == MAX_SAMPLES)
    points.erase(points.begin());
  points.push_back(sample);
}

std::vector<Climb>
Detector::Detect(Clock::time_point now)
{
  std::vector<Climb> result;
  std::size_t begin = 0;
  for (std::size_t i = 1; i <= points.size(); ++i) {
    bool split = i == points.size();
    if (!split) {
      const double dt = Elapsed(points[i].time, points[i - 1].time);
      split = dt > 15 ||
        std::abs(points[i].altitude - points[i - 1].altitude) / dt > 50 ||
        points[i - 1].location.Distance(points[i].location) / dt > 150;
    }
    if (split) {
      auto detected = DetectSegment(
        std::span{points}.subspan(begin, i - begin), now);
      result.insert(result.end(), detected.begin(), detected.end());
      begin = i;
    }
  }
  return result;
}

std::vector<Climb>
Detector::DetectSegment(std::span<const Sample> points, Clock::time_point now)
{
  std::vector<Climb> result;
  if (points.size() < 3)
    return result;

  const std::size_t n = points.size();
  std::vector<double> bearings(n);
  for (std::size_t i = 0; i + 1 < n; ++i)
    bearings[i] = points[i].location.Bearing(points[i + 1].location).Degrees();
  bearings.back() = bearings[n - 2];

  /* Same two-state circling model as the Python IGC detector. */
  static constexpr double initial[2] = {0.80, 0.20};
  static constexpr double transition[2][2] = {
    {0.982, 0.018}, {0.030, 0.970},
  };
  static constexpr double emission[2][2] = {
    {0.942, 0.058}, {0.093, 0.907},
  };
  std::array<double, 2> score{
    std::log(initial[0] * emission[0][0]),
    std::log(initial[1] * emission[1][0]),
  };
  std::vector<std::array<unsigned, 2>> previous(n);
  std::size_t reference = 0;
  for (std::size_t i = 1; i < n; ++i) {
    while (reference + 1 < i &&
           Elapsed(points[i].time, points[reference + 1].time) >= 5)
      ++reference;

    const double dt = Elapsed(points[i].time, points[reference].time);
    const double speed = points[i - 1].location.Distance(points[i].location) /
      Elapsed(points[i].time, points[i - 1].time);
    const double turn = std::remainder(bearings[i] - bearings[reference], 360);
    const unsigned observation = dt >= 5 && dt <= 15 &&
      speed > 15. / 3.6 && std::abs(turn / dt) > 6;
    std::array<double, 2> next;
    for (unsigned target = 0; target < 2; ++target) {
      const double a = score[0] + std::log(transition[0][target]);
      const double b = score[1] + std::log(transition[1][target]);
      const unsigned source = b > a;
      previous[i][target] = source;
      next[target] = (source ? b : a) +
        std::log(emission[target][observation]);
    }
    score = next;
  }
  std::vector<unsigned> state(n);
  state.back() = score[1] > score[0];
  for (std::size_t i = n - 1; i > 0; --i)
    state[i - 1] = previous[i][state[i]];

  std::optional<std::size_t> entry;
  for (std::size_t i = 0; i < n; ++i) {
    if (!entry && state[i])
      entry = i;
    else if (entry && !state[i]) {
      const auto &first = points[*entry], &last = points[i];
      const double duration = Elapsed(last.time, first.time);
      if (last.time <= now - LOOKAHEAD && first.time > last_emitted &&
          duration > 60 &&
          duration <= std::chrono::duration<double>(MAX_CLIMB_DURATION).count() &&
          last.altitude > first.altitude &&
          !(this->points.size() == MAX_SAMPLES &&
            points.data() == this->points.data() &&
            Elapsed(first.time, points.front().time) < 15)) {
        double lat = 0, lon_offset = 0;
        for (std::size_t j = *entry; j <= i; ++j) {
          lat += points[j].location.latitude.Degrees();
          lon_offset += std::remainder(points[j].location.longitude.Degrees() -
                                      first.location.longitude.Degrees(), 360);
        }
        const double count = i - *entry + 1;
        const auto location = GeoPoint{
          Angle::Degrees(std::remainder(
            first.location.longitude.Degrees() + lon_offset / count, 360)),
          Angle::Degrees(lat / count),
        };
        result.push_back({first.time, last.time, location,
                          first.altitude, last.altitude,
                          (last.altitude - first.altitude) / duration});
        last_emitted = last.time;
      }
      entry.reset();
    }
  }
  return result;
}

static bool
IsId(std::string_view id) noexcept
{
  return !id.empty() && id.size() <= 128 &&
    std::all_of(id.begin(), id.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

void
Queue::Add(const Climb &climb, std::string_view public_identity)
{
  if (!public_identity.empty())
    ValidatePublicIdentity(public_identity);
  if (std::any_of(climbs.begin(), climbs.end(), [&](const auto &p) {
    return p.climb.start == climb.start;
  }))
    return;
  if (climbs.size() >= MAX_CLIMBS)
    throw std::runtime_error("Collaborative flight queue is full");
  climbs.push_back({climb, false, std::string{public_identity}});
}

unsigned
Queue::Prune(Clock::time_point now) noexcept
{
  return std::erase_if(climbs, [now](const auto &p) {
    return p.climb.end < now - RETENTION;
  });
}

void
Queue::Acknowledge(const std::vector<std::string> &ids)
{
  for (auto &pending : climbs)
    if (std::find(ids.begin(), ids.end(), pending.climb.Id(client)) != ids.end())
      pending.delivered = true;
}

static boost::json::object
ToJson(const Climb &c)
{
  return {
    {"start", Seconds(c.start)}, {"end", Seconds(c.end)},
    {"lat", c.location.latitude.Degrees()},
    {"lon", c.location.longitude.Degrees()},
    {"base", c.base}, {"top", c.top}, {"rate", c.rate},
  };
}

static double
Number(const boost::json::value &v)
{
  return boost::json::value_to<double>(v);
}

static Climb
FromJson(const boost::json::object &o)
{
  const auto start_seconds = boost::json::value_to<int64_t>(o.at("start"));
  const auto end_seconds = boost::json::value_to<int64_t>(o.at("end"));
  constexpr auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
    Clock::duration::max()).count();
  if (start_seconds <= 0 || end_seconds > max_seconds ||
      end_seconds <= start_seconds ||
      end_seconds - start_seconds >
      std::chrono::duration_cast<std::chrono::seconds>(
        MAX_CLIMB_DURATION).count())
    throw std::runtime_error("Invalid stored collaborative timestamps");
  const auto start = Clock::time_point{std::chrono::seconds{start_seconds}};
  const auto end = Clock::time_point{std::chrono::seconds{end_seconds}};
  Climb c{start, end,
          {Angle::Degrees(Number(o.at("lon"))),
           Angle::Degrees(Number(o.at("lat")))},
          Number(o.at("base")), Number(o.at("top")), Number(o.at("rate"))};
  if (!Valid({c.start, c.location, c.base}) ||
      !Valid({c.end, c.location, c.top}) ||
      c.top <= c.base || !std::isfinite(c.rate) || c.rate <= 0 || c.rate > 50 ||
      std::abs(c.rate - (c.top - c.base) / Elapsed(c.end, c.start)) > 0.01)
    throw std::runtime_error("Invalid stored collaborative climb");
  return c;
}

std::string
Queue::Serialize() const
{
  boost::json::array entries;
  for (const auto &p : climbs) {
    auto o = ToJson(p.climb);
    o["delivered"] = p.delivered;
    if (!p.public_identity.empty())
      o["public_identity"] = p.public_identity;
    entries.push_back(std::move(o));
  }
  StringOutputStream output;
  Json::Serialize(output, boost::json::object{
    {"version", 1}, {"client", client}, {"climbs", std::move(entries)},
  });
  return std::move(output).GetValue();
}

Queue
Queue::Parse(std::string_view body)
{
  const auto value = boost::json::parse(body);
  const auto &o = value.as_object();
  if (o.at("version").as_int64() != 1)
    throw std::runtime_error("Unsupported collaborative queue version");
  Queue result;
  result.client = boost::json::value_to<std::string>(o.at("client"));
  if (!IsId(result.client) || o.at("climbs").as_array().size() > MAX_CLIMBS)
    throw std::runtime_error("Invalid collaborative queue");
  for (const auto &v : o.at("climbs").as_array()) {
    const auto count = result.climbs.size();
    result.Add(FromJson(v.as_object()));
    if (count == result.climbs.size())
      throw std::runtime_error("Duplicate stored collaborative climb");
    result.climbs.back().delivered = v.as_object().at("delivered").as_bool();
    if (const auto *identity = v.as_object().if_contains("public_identity")) {
      auto text = boost::json::value_to<std::string>(*identity);
      ValidatePublicIdentity(text);
      result.climbs.back().public_identity = std::move(text);
    }
  }
  return result;
}

std::string
FolderId(std::string_view url)
{
  constexpr std::string_view prefix = "https://drive.google.com/drive/folders/";
  if (!url.starts_with(prefix))
    throw std::runtime_error("Expected a Google Drive folder share link");
  url.remove_prefix(prefix.size());
  url = url.substr(0, url.find_first_of("?#"));
  if (!IsId(url))
    throw std::runtime_error("Invalid Google Drive folder ID");
  return std::string{url};
}

void
ValidateUploadUrl(std::string_view url)
{
  constexpr std::string_view prefix = "https://script.google.com/macros/s/";
  if (!url.starts_with(prefix) || !url.ends_with("/exec") ||
      !IsId(url.substr(prefix.size(), url.size() - prefix.size() - 5)))
    throw std::runtime_error("Expected a deployed Google Apps Script /exec URL");
}

Batch
MakeBatch(const Queue &queue, std::string_view folder, std::string_view token,
          Clock::time_point now)
{
  if (!IsId(queue.client) || !IsId(folder) ||
      token.size() < 32 || token.size() > 256)
    throw std::runtime_error("Invalid collaborative upload configuration");
  Batch result;
  result.id = std::string{queue.client} + "-" + std::to_string(Seconds(now));
  boost::json::array entries;
  for (const auto &p : queue.climbs) {
    if (p.delivered || p.climb.end < now - Queue::RETENTION)
      continue;
    auto o = ToJson(p.climb);
    auto id = p.climb.Id(queue.client);
    o["id"] = id;
    const auto identity = p.public_identity.empty()
      ? std::string_view{"CF"} : std::string_view{p.public_identity};
    ValidatePublicIdentity(identity);
    o["public_identity"] = identity;
    result.climb_ids.push_back(std::move(id));
    entries.push_back(std::move(o));
    if (entries.size() == 64)
      break;
  }
  StringOutputStream output;
  Json::Serialize(output, boost::json::object{
    {"version", 1}, {"folder_id", folder}, {"token", token},
    {"batch_id", result.id}, {"climbs", std::move(entries)},
  });
  result.body = std::move(output).GetValue();
  return result;
}

void
ValidateAcknowledgement(std::string_view response, const Batch &batch)
{
  const auto value = boost::json::parse(response);
  const auto &o = value.as_object();
  if (!o.at("ok").as_bool() ||
      o.at("batch_id").as_string() != batch.id ||
      boost::json::value_to<std::size_t>(o.at("accepted")) !=
      batch.climb_ids.size())
    throw std::runtime_error("Collaborative upload was not acknowledged");
}

} // namespace CollaborativeFlight

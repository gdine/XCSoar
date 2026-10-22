// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Update.hpp"
#include "Engine/Waypoint/Waypoints.hpp"
#include "Waypoint/Factory.hpp"
#include "Waypoint/WaypointReaderSeeYou.hpp"
#include "io/BufferedReader.hxx"
#include "io/MemoryReader.hxx"
#include "util/SpanCast.hxx"
#include "util/StringStrip.hxx"
#include "util/DecimalParser.hxx"
#include "Formatter/TimeFormatter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

using std::string_view_literals::operator""sv;

namespace RecentClimbs {

[[gnu::pure]]
static bool
IsDriveIdChar(char ch) noexcept
{
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
}

[[gnu::pure]]
static std::string_view
ExtractDriveId(std::string_view rest) noexcept
{
  std::size_t n = 0;
  while (n < rest.size() && IsDriveIdChar(rest[n]))
    ++n;
  return rest.substr(0, n);
}

[[gnu::pure]]
static std::string_view
FindDriveId(std::string_view url) noexcept
{
  if (const auto p = url.find("/file/d/"sv); p != url.npos)
    return ExtractDriveId(url.substr(p + 8));

  for (const auto key : {"?id="sv, "&id="sv})
    if (const auto p = url.find(key); p != url.npos)
      return ExtractDriveId(url.substr(p + key.size()));

  return {};
}

std::string
ResolveDownloadUrl(std::string_view url) noexcept
{
  url = Strip(url);

  /* only rewrite links pointing to Google Drive itself; a direct
     "drive.usercontent.google.com" link is used as is */
  const bool is_drive = url.starts_with("https://drive.google.com/"sv) ||
    url.starts_with("http://drive.google.com/"sv);
  if (is_drive) {
    const auto id = FindDriveId(url);
    if (!id.empty())
      return std::string{"https://drive.google.com/uc?export=download&id="sv} +
        std::string{id};
  }

  return std::string{url};
}

[[gnu::pure]]
static std::string_view
GetField(std::string_view comment, std::string_view key) noexcept
{
  while (!comment.empty()) {
    const auto end = comment.find(';');
    const auto field = Strip(comment.substr(0, end));
    if (field.starts_with(key))
      return Strip(field.substr(key.size()));
    if (end == comment.npos)
      break;
    comment.remove_prefix(end + 1);
  }
  return {};
}

static Waypoint::RecentClimb
ParseMetadata(std::string_view comment)
{
  Waypoint::RecentClimb result;
  const auto observed = GetField(comment, "Observed:");
  if (observed.size() == 23 && observed[10] == ' ' &&
      observed.ends_with(" UTC")) {
    std::string iso{observed.substr(0, 19)};
    iso[10] = 'T';
    iso += 'Z';
    try {
      result.observed = ParseISO8601Utc(iso);
    } catch (const std::runtime_error &) {
      /* Reported as an unknown-age entry by the caller. */
    }
  }

  auto vario = GetField(comment, "Avg vario:");
  if (vario.ends_with(" m/s")) {
    vario.remove_suffix(4);
    const auto rate = ParseDecimal(Strip(vario));
    if (rate && std::isfinite(*rate) && *rate >= 0)
      result.lift_rate = *rate;
  }
  return result;
}

std::vector<Waypoint>
Parse(std::string_view body)
{
  const auto content = StripLeft(body);
  if (content.starts_with('<'))
    throw std::runtime_error("Received an HTML page instead of a CUP file "
                             "(is the file shared with \"Anyone with the link\"?)");

  Waypoints parsed;
  MemoryReader reader{AsBytes(body)};
  BufferedReader buffered{reader};
  ParseSeeYou(WaypointFactory{WaypointOrigin::NONE}, parsed, buffered);
  parsed.Optimise();

  std::vector<Waypoint> result;
  result.reserve(parsed.size());
  for (const auto &ptr : parsed) {
    Waypoint wp = *ptr;
    wp.id = 0;
    if (wp.type == Waypoint::Type::NORMAL)
      wp.type = Waypoint::Type::THERMAL_HOTSPOT;
    wp.recent_climb = ParseMetadata(wp.comment);
    result.push_back(std::move(wp));
  }

  return result;
}

Selection
Select(const std::vector<Waypoint> &downloaded,
       std::chrono::system_clock::time_point now,
       std::chrono::minutes max_age, double min_climb_rate)
{
  Selection result;
  for (const auto &wp : downloaded) {
    if (!wp.recent_climb || !wp.recent_climb->observed) {
      ++result.unknown_age;
      continue;
    }

    const auto observed = *wp.recent_climb->observed;
    if (observed > now) {
      result.next_expiry = std::min(result.next_expiry, observed);
      continue;
    }
    if (now - observed > max_age)
      continue;

    if (min_climb_rate > 0) {
      if (!wp.recent_climb->lift_rate) {
        ++result.unknown_rate;
        continue;
      }
      if (*wp.recent_climb->lift_rate < min_climb_rate)
        continue;
    }

    result.climbs.push_back(wp);
    result.next_expiry = std::min(result.next_expiry,
                                 observed + max_age +
                                 std::chrono::seconds{1});
  }
  return result;
}

[[gnu::pure]]
static bool
IsSameClimb(const Waypoint &a, const Waypoint &b) noexcept
{
  return a.location == b.location &&
    a.name == b.name &&
    a.shortname == b.shortname &&
    a.comment == b.comment &&
    a.details == b.details &&
    a.recent_climb == b.recent_climb &&
    a.type == b.type &&
    a.has_elevation == b.has_elevation &&
    (!a.has_elevation || a.elevation == b.elevation);
}

UpdateResult
Update(Waypoints &waypoints, std::vector<Entry> &current,
       std::vector<Waypoint> &&downloaded,
       const RasterTerrain *terrain) noexcept
{
  UpdateResult result;

  /* forget climbs which are no longer in the database, e.g. because
     the waypoint files were reloaded in the meantime */
  if (!current.empty()) {
    std::unordered_set<const Waypoint *> present;
    present.reserve(waypoints.size());
    for (const auto &ptr : waypoints)
      present.insert(ptr.get());

    std::erase_if(current, [&present](const Entry &entry){
      return !present.contains(entry.waypoint.get());
    });
  }

  std::vector<bool> keep(current.size(), false);
  std::vector<Waypoint> to_add;

  for (auto &wp : downloaded) {
    bool found = false;
    for (std::size_t i = 0; i < current.size(); ++i) {
      if (!keep[i] && IsSameClimb(current[i].parsed, wp)) {
        keep[i] = true;
        found = true;
        break;
      }
    }

    if (found)
      ++result.kept;
    else
      to_add.push_back(std::move(wp));
  }

  std::vector<Entry> next;
  next.reserve(result.kept + to_add.size());

  for (std::size_t i = 0; i < current.size(); ++i) {
    if (keep[i]) {
      next.push_back(std::move(current[i]));
    } else {
      waypoints.Erase(std::move(current[i].waypoint));
      ++result.removed;
    }
  }

  const WaypointFactory factory{WaypointOrigin::NONE, 0, terrain};
  for (auto &parsed : to_add) {
    parsed.origin = WaypointOrigin::NONE;

    Waypoint wp = parsed;
    if (!wp.has_elevation)
      factory.FallbackElevation(wp);

    next.push_back({waypoints.Append(std::move(wp)), std::move(parsed)});
    ++result.added;
  }

  current = std::move(next);

  if (result.IsModified())
    waypoints.Optimise();

  return result;
}

} // namespace RecentClimbs

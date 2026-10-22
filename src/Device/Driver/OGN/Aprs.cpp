// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Aprs.hpp"
#include "Units/System.hpp"

#include <cmath>
#include <cstdio>

namespace OGN {

static constexpr bool
IsDigit(char ch) noexcept
{
  return ch >= '0' && ch <= '9';
}

static constexpr bool
IsAlnum(char ch) noexcept
{
  return IsDigit(ch) || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static constexpr char
ToUpper(char ch) noexcept
{
  return ch >= 'a' && ch <= 'z' ? char(ch - 'a' + 'A') : ch;
}

static constexpr int
HexValue(char ch) noexcept
{
  if (IsDigit(ch))
    return ch - '0';
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  return -1;
}

static constexpr bool
IsCallsignChar(char ch) noexcept
{
  return IsAlnum(ch) || ch == '-';
}

/**
 * Parse exactly #n decimal digits at #pos.
 */
static std::optional<unsigned>
ParseDigits(std::string_view s, std::size_t pos, std::size_t n) noexcept
{
  if (pos + n > s.size())
    return std::nullopt;

  unsigned value = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const char ch = s[pos + i];
    if (!IsDigit(ch))
      return std::nullopt;
    value = value * 10 + unsigned(ch - '0');
  }

  return value;
}

/**
 * Parse "DDMM.mm" (#degree_digits = 2) or "DDDMM.mm" (3) into
 * minutes of arc.
 */
static std::optional<double>
ParseCoordinate(std::string_view s, std::size_t pos,
                std::size_t degree_digits) noexcept
{
  if (pos + degree_digits + 5 > s.size() || s[pos + degree_digits + 2] != '.')
    return std::nullopt;

  const auto degrees = ParseDigits(s, pos, degree_digits);
  const auto minutes = ParseDigits(s, pos + degree_digits, 2);
  const auto hundredths = ParseDigits(s, pos + degree_digits + 3, 2);
  if (!degrees || !minutes || !hundredths)
    return std::nullopt;

  return *degrees * 60. + *minutes + *hundredths / 100.;
}

static std::optional<int>
ParseSignedInteger(std::string_view s) noexcept
{
  if (s.empty())
    return std::nullopt;

  bool negative = false;
  if (s.front() == '+' || s.front() == '-') {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }

  if (s.empty() || s.size() > 6)
    return std::nullopt;

  int value = 0;
  for (const char ch : s) {
    if (!IsDigit(ch))
      return std::nullopt;
    value = value * 10 + (ch - '0');
  }

  return negative ? -value : value;
}

/**
 * Decoded tokens of the APRS comment.
 */
struct AprsComment {
  std::optional<uint8_t> flags;
  uint32_t address = 0;
  unsigned extra_lat = 0, extra_lon = 0;
  std::optional<int> climb_fpm;
  std::optional<std::string_view> callsign;
};

static void
ParseCommentToken(std::string_view token, AprsComment &comment) noexcept
{
  if (token.size() == 10 && token.starts_with("id")) {
    if (comment.flags)
      return;

    uint32_t value = 0;
    for (std::size_t i = 2; i < token.size(); ++i) {
      const int digit = HexValue(token[i]);
      if (digit < 0)
        return;
      value = (value << 4) | unsigned(digit);
    }

    comment.flags = uint8_t(value >> 24);
    comment.address = value & 0xffffff;
  } else if (token.size() == 5 && token[0] == '!' && token[1] == 'W' &&
             IsDigit(token[2]) && IsDigit(token[3]) && token[4] == '!') {
    comment.extra_lat = unsigned(token[2] - '0');
    comment.extra_lon = unsigned(token[3] - '0');
  } else if (token.size() > 3 && token.ends_with("fpm") &&
             (token[0] == '+' || token[0] == '-')) {
    if (!comment.climb_fpm)
      comment.climb_fpm = ParseSignedInteger(token.substr(0, token.size() - 3));
  } else if (token.size() >= 4 && token[0] == 'A' && IsDigit(token[1]) &&
             token[2] == ':') {
    const auto callsign = token.substr(3);
    if (callsign.size() > 8 || comment.callsign)
      return;
    for (const char ch : callsign)
      if (!IsCallsignChar(ch))
        return;
    comment.callsign = callsign;
  }
}

static AprsComment
ParseComment(std::string_view s) noexcept
{
  AprsComment comment;

  while (true) {
    const auto start = s.find_first_not_of(" \t");
    if (start == s.npos)
      break;
    s.remove_prefix(start);

    const auto end = s.find_first_of(" \t");
    ParseCommentToken(s.substr(0, end), comment);

    if (end == s.npos)
      break;
    s.remove_prefix(end);
  }

  return comment;
}

std::optional<AprsReport>
ParseAprsPosition(std::string_view line) noexcept
{
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                           line.back() == ' '))
    line.remove_suffix(1);

  if (line.empty() || line.front() == '#')
    return std::nullopt;

  /* sender ">" tocall */
  const auto gt = line.find('>');
  if (gt == line.npos || gt == 0 || gt > 9)
    return std::nullopt;
  for (std::size_t i = 0; i < gt; ++i)
    if (!IsCallsignChar(line[i]))
      return std::nullopt;

  std::size_t pos = gt + 1;
  const std::size_t tocall_start = pos;
  while (pos < line.size() && IsCallsignChar(line[pos]))
    ++pos;
  const std::size_t tocall_length = pos - tocall_start;
  if (tocall_length == 0 || tocall_length > 9)
    return std::nullopt;

  char tocall[10];
  for (std::size_t i = 0; i < tocall_length; ++i)
    tocall[i] = ToUpper(line[tocall_start + i]);
  tocall[tocall_length] = '\0';
  const std::string_view tocall_view{tocall, tocall_length};

  const auto colon = line.find(':', pos);
  if (colon == line.npos)
    return std::nullopt;

  pos = colon + 1;
  if (pos >= line.size() || (line[pos] != '/' && line[pos] != '@'))
    return std::nullopt;
  ++pos;

  /* time stamp */
  const auto t1 = ParseDigits(line, pos, 2);
  const auto t2 = ParseDigits(line, pos + 2, 2);
  const auto t3 = ParseDigits(line, pos + 4, 2);
  if (!t1 || !t2 || !t3 || pos + 6 >= line.size())
    return std::nullopt;

  const char time_format = line[pos + 6];
  std::optional<unsigned> beacon_time;
  if (time_format == 'h')
    beacon_time = *t1 * 3600 + *t2 * 60 + *t3;
  else if (time_format == 'z')
    beacon_time = *t2 * 3600 + *t3 * 60;
  else if (time_format != '/')
    return std::nullopt;

  if (beacon_time && *beacon_time >= 86400)
    return std::nullopt;

  pos += 7;

  /* "DDMM.mmN", symbol table, "DDDMM.mmE", symbol code */
  const auto lat_minutes = ParseCoordinate(line, pos, 2);
  if (!lat_minutes || pos + 8 >= line.size())
    return std::nullopt;
  const char ns = line[pos + 7];
  if (ns != 'N' && ns != 'S')
    return std::nullopt;
  pos += 9;

  const auto lon_minutes = ParseCoordinate(line, pos, 3);
  if (!lon_minutes || pos + 9 >= line.size())
    return std::nullopt;
  const char ew = line[pos + 8];
  if (ew != 'E' && ew != 'W')
    return std::nullopt;
  pos += 10;

  std::string_view rest = line.substr(pos);

  /* optional "course/speed" */
  std::optional<unsigned> course, speed_kt;
  if (rest.size() >= 7 && rest[3] == '/') {
    const auto c = ParseDigits(rest, 0, 3);
    const auto s = ParseDigits(rest, 4, 3);
    if (c && s) {
      course = c;
      speed_kt = s;
      rest.remove_prefix(7);
    }
  }

  /* optional "/A=altitude" in feet */
  std::optional<int> altitude_ft;
  if (rest.starts_with("/A=")) {
    std::size_t n = 3;
    if (n < rest.size() && rest[n] == '-')
      ++n;
    const std::size_t digits_start = n;
    while (n < rest.size() && IsDigit(rest[n]) && n - digits_start < 6)
      ++n;
    if (n - digits_start >= 5) {
      altitude_ft = ParseSignedInteger(rest.substr(3, n - 3));
      rest.remove_prefix(n);
    }
  }

  const AprsComment comment = ParseComment(rest);

  /* receiver beacons and other stations do not carry an aircraft
     id */
  if (!comment.flags || comment.address == 0)
    return std::nullopt;

  /* pilot requested no tracking */
  if (*comment.flags & 0x40)
    return std::nullopt;

  double latitude = (*lat_minutes + comment.extra_lat / 1000.) / 60.;
  double longitude = (*lon_minutes + comment.extra_lon / 1000.) / 60.;
  if (ns == 'S')
    latitude = -latitude;
  if (ew == 'W')
    longitude = -longitude;

  if (std::fabs(latitude) > 90 || std::fabs(longitude) > 180)
    return std::nullopt;

  char id_buffer[8];
  snprintf(id_buffer, sizeof(id_buffer), "%06X",
           unsigned(comment.address & 0xffffff));
  const FlarmId id = FlarmId::Parse(id_buffer, nullptr);
  if (!id.IsDefined())
    return std::nullopt;

  AprsReport report;
  report.address = comment.address;
  report.beacon_time = beacon_time;
  report.safesky_relay = tocall_view == "OGNSKY";

  FlarmTraffic &traffic = report.traffic;
  traffic = {};
  traffic.Clear();

  traffic.id = id;

  static constexpr FlarmTraffic::IdType id_types[] = {
    FlarmTraffic::IdType::RANDOM,
    FlarmTraffic::IdType::ICAO,
    FlarmTraffic::IdType::FLARM,
    /* OGN tracker */
    FlarmTraffic::IdType::FLARM,
  };
  traffic.id_type = id_types[*comment.flags & 3];

  /* like the PFLAA sentences tested with GDL90toNMEA: ADS-B relays
     are ADS-B, everything else is reported as FLARM */
  traffic.source = tocall_view == "OGADSB"
    ? FlarmTraffic::SourceType::ADSB
    : FlarmTraffic::SourceType::FLARM;

  traffic.type = FlarmTraffic::AircraftType((*comment.flags >> 2) & 0xf);
  traffic.alarm_level = FlarmTraffic::AlarmType::NONE;

  traffic.location = GeoPoint(Angle::Degrees(longitude),
                              Angle::Degrees(latitude));
  traffic.location_available = true;
  traffic.absolute_location = true;
  traffic.relative_north = 0;
  traffic.relative_east = 0;

  if (altitude_ft) {
    traffic.altitude = Units::ToSysUnit(*altitude_ft, Unit::FEET);
    traffic.altitude_available = true;
    traffic.absolute_altitude = true;
  } else {
    traffic.altitude = 0;
    traffic.altitude_available = false;
  }
  traffic.relative_altitude = 0;

  traffic.track_received = course.has_value();
  traffic.track = Angle::Degrees(course ? *course % 360 : 0);
  traffic.speed_received = speed_kt.has_value();
  traffic.speed = speed_kt ? Units::ToSysUnit(*speed_kt, Unit::KNOTS) : 0;

  traffic.climb_rate_received = comment.climb_fpm.has_value();
  traffic.climb_rate = comment.climb_fpm
    ? Units::ToSysUnit(*comment.climb_fpm, Unit::FEET_PER_MINUTE)
    : 0;
  traffic.climb_rate_avg30s = 0;
  traffic.climb_rate_avg30s_available = false;
  traffic.turn_rate = 0;
  traffic.turn_rate_received = false;

  if (comment.callsign)
    traffic.name.assign(*comment.callsign);

  traffic.stealth = false;
  traffic.no_track = false;
  traffic.rssi_available = false;

  return report;
}

bool
IsOlderBeacon(unsigned beacon, unsigned reference) noexcept
{
  const int difference =
    ((int(reference) - int(beacon)) % 86400 + 86400) % 86400;
  return difference > 0 && difference < 43200;
}

std::string
MakeAprsCallsign(std::string_view registration, unsigned random)
{
  std::string base;
  for (const char ch : registration) {
    if (ch == '-')
      base.push_back('0');
    else if (IsAlnum(ch))
      base.push_back(ToUpper(ch));
  }

  if (base.empty()) {
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "XCS%04u", (random / 16) % 10000);
    base = buffer;
  }

  /* leave room for "-" and at least one SSID digit */
  if (base.size() > MAX_CALLSIGN_LENGTH - 2)
    base.resize(MAX_CALLSIGN_LENGTH - 2);

  const unsigned max_ssid = base.size() + 3 <= MAX_CALLSIGN_LENGTH ? 16 : 10;
  base.push_back('-');
  base += std::to_string(random % max_ssid);
  return base;
}

std::string
MakeAprsRangeFilter(double latitude, double longitude, unsigned radius_km)
{
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "r/%.4f/%.4f/%u",
           latitude, longitude, radius_km);
  return buffer;
}

} // namespace OGN

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Details.hpp"
#include "Id.hpp"
#include "Global.hpp"
#include "TrafficDatabases.hpp"
#include "MessagingDatabase.hpp"
#include "MessagingRecord.hpp"
#include "Language/Language.hpp"
#include "util/Macros.hpp"
#include "util/StringCompare.hxx"

#include <string>

#include <cassert>
#include <utility>

// Weak reference to SaveFlarmMessagingPeriodic to avoid requiring Glue.cpp in all tests
extern void SaveFlarmMessagingPeriodic() noexcept __attribute__((weak));

namespace FlarmDetails {

const FlarmNetRecord *
LookupRecord(FlarmId id) noexcept
{
  // try to find flarm from FlarmNet.org File
  if (traffic_databases == nullptr)
    return NULL;

  return traffic_databases->flarm_net.FindRecordById(id);
}

const char *
LookupCallsign(FlarmId id) noexcept
{
  if (traffic_databases == nullptr)
    return nullptr;

  return traffic_databases->FindNameById(id);
}

FlarmId
LookupId(const char *cn) noexcept
{
  assert(traffic_databases != nullptr);

  return traffic_databases->FindIdByName(cn);
}

bool
AddSecondaryItem(FlarmId id, const char *name) noexcept
{
  assert(id.IsDefined());
  assert(traffic_databases != nullptr);

  return traffic_databases->flarm_names.Set(id, name);
}

void
StoreMessagingRecord(const MessagingRecord &record) noexcept
{
  if (traffic_databases == nullptr)
    return;

  traffic_databases->flarm_messages.Update(record);

  if (SaveFlarmMessagingPeriodic != nullptr)
    SaveFlarmMessagingPeriodic();
}

unsigned
FindIdsByCallSign(const char *cn, FlarmId array[], unsigned size) noexcept
{
  assert(cn != NULL);
  assert(!StringIsEmpty(cn));
  assert(traffic_databases != nullptr);

  return traffic_databases->FindIdsByName(cn, array, size);
}

static std::string
NormalizeRegistration(const char *registration)
{
  std::string result;
  for (const char *p = registration; *p != '\0'; ++p) {
    const char ch = *p;
    if (ch == '-' || ch == ' ')
      continue;
    result.push_back(ch >= 'a' && ch <= 'z' ? char(ch - 'a' + 'A') : ch);
  }

  return result;
}

unsigned
FindIdsByRegistration(const char *registration, FlarmId array[],
                      unsigned size) noexcept
try {
  assert(registration != nullptr);

  if (traffic_databases == nullptr || size == 0)
    return 0;

  const std::string wanted = NormalizeRegistration(registration);
  if (wanted.empty())
    return 0;

  unsigned count = 0;
  for (const auto &[id, record] : traffic_databases->flarm_net) {
    if (NormalizeRegistration(record.registration) == wanted) {
      array[count++] = id;
      if (count >= size)
        break;
    }
  }

  return count;
} catch (...) {
  return 0;
}

ResolvedInfo
ResolveInfo(FlarmId id)
{
  ResolvedInfo out;
  if (traffic_databases == nullptr)
    return out;

  const auto msg = std::as_const(traffic_databases->flarm_messages).FindRecordById(id);
  const auto *net = traffic_databases->flarm_net.FindRecordById(id);

  const char *name = traffic_databases->FindNameById(id);
  if (name != nullptr)
    out.callsign = name;

  if (msg.has_value()) {
    out.source = ResolvedSource::MESSAGING;

    out.pilot = msg->pilot;
    out.plane_type = msg->plane_type;
    out.registration = msg->registration;

    if (msg->frequency.IsDefined())
      out.frequency = msg->frequency;
  } else if (net != nullptr) {
    out.source = ResolvedSource::FLARMNET;

    if (!net->pilot.empty()) out.pilot = net->pilot.c_str();
    if (!net->plane_type.empty()) out.plane_type = net->plane_type.c_str();
    if (!net->registration.empty()) out.registration = net->registration.c_str();

    if (!net->airfield.empty() &&
        (out.registration.empty() || net->airfield != out.registration.c_str()))
      out.airfield = net->airfield.c_str();

    if (net->frequency.IsDefined())
      out.frequency = net->frequency;
  }

  const char *user_callsign = traffic_databases->flarm_names.Get(id);
  if (user_callsign != nullptr)
    out.callsign = user_callsign;

  return out;
}

static const char *const resolved_source_strings[] = {
  N_("Unresolved"),
  N_("Flarm Messaging"),
  N_("FLARMnet"),
};

const char *ToString(ResolvedSource source) noexcept
{
  unsigned i = (unsigned)source;
  const char *text = i < ARRAY_SIZE(resolved_source_strings)
    ? resolved_source_strings[i]
    : N_("Unknown");

  return gettext(text);
}

static_assert(ARRAY_SIZE(resolved_source_strings) == static_cast<unsigned>(ResolvedSource::COUNT));

} // namespace FlarmDetails

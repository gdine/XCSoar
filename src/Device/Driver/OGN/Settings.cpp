// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Settings.hpp"
#include "Profile/Keys.hpp"
#include "Profile/ProfileMap.hpp"

#include <algorithm>

OGNSettings ogn_settings;

void
LoadFromProfile(OGNSettings &settings) noexcept
{
  settings = OGNSettings{};

  Profile::Get(ProfileKeys::OGNGliderRange, settings.glider_range);
  Profile::Get(ProfileKeys::OGNOtherRange, settings.other_range);
  Profile::Get(ProfileKeys::OGNVerticalRange, settings.vrange);
  Profile::Get(ProfileKeys::OGNIgnoreSafeSky, settings.ignore_safesky);

  settings.glider_range = std::clamp(settings.glider_range,
                                     OGNSettings::MIN_GLIDER_RANGE_M,
                                     OGNSettings::MAX_RANGE_M);
  settings.other_range = std::min(settings.other_range,
                                  OGNSettings::MAX_RANGE_M);
  settings.vrange = std::min(settings.vrange, OGNSettings::MAX_VRANGE_M);
}

void
SaveToProfile(const OGNSettings &settings) noexcept
{
  Profile::Set(ProfileKeys::OGNGliderRange, settings.glider_range);
  Profile::Set(ProfileKeys::OGNOtherRange, settings.other_range);
  Profile::Set(ProfileKeys::OGNVerticalRange, settings.vrange);
  Profile::Set(ProfileKeys::OGNIgnoreSafeSky, settings.ignore_safesky);
}

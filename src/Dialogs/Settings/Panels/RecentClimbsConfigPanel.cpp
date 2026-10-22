// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "RecentClimbsConfigPanel.hpp"
#include "RecentClimbs/Settings.hpp"
#include "Profile/Keys.hpp"
#include "Profile/Profile.hpp"
#include "Language/Language.hpp"
#include "Widget/RowFormWidget.hpp"
#include "Form/DataField/Boolean.hpp"
#include "Form/DataField/Listener.hpp"
#include "Interface.hpp"
#include "UIGlobals.hpp"
#include "Units/Units.hpp"

enum ControlIndex {
  RecentClimbsEnabled,
  RecentClimbsURL,
  RecentClimbsInterval,
  RecentClimbsMaxAge,
  RecentClimbsMinClimbRate,
};

class RecentClimbsConfigPanel final
  : public RowFormWidget, DataFieldListener {
  Unit climb_rate_unit = Unit::METER_PER_SECOND;

public:
  RecentClimbsConfigPanel() noexcept
    :RowFormWidget(UIGlobals::GetDialogLook()) {}

  void SetEnabled(bool enabled) noexcept;

  /* virtual methods from class Widget */
  void Prepare(ContainerWindow &parent, const PixelRect &rc) noexcept override;
  bool Save(bool &changed) noexcept override;

private:
  /* methods from DataFieldListener */
  void OnModified(DataField &df) noexcept override;
};

void
RecentClimbsConfigPanel::SetEnabled(bool enabled) noexcept
{
  SetRowEnabled(RecentClimbsURL, enabled);
  SetRowEnabled(RecentClimbsInterval, enabled);
  SetRowEnabled(RecentClimbsMaxAge, enabled);
  SetRowEnabled(RecentClimbsMinClimbRate, enabled);
}

void
RecentClimbsConfigPanel::OnModified(DataField &df) noexcept
{
  if (IsDataField(RecentClimbsEnabled, df)) {
    const DataFieldBoolean &dfb = (const DataFieldBoolean &)df;
    SetEnabled(dfb.GetValue());
  }
}

void
RecentClimbsConfigPanel::Prepare(ContainerWindow &parent,
                                 const PixelRect &rc) noexcept
{
  const auto &settings = CommonInterface::GetComputerSettings().recent_climbs;

  RowFormWidget::Prepare(parent, rc);

  AddBoolean(_("Recent climbs"),
             _("Periodically download a CUP file of recently detected "
               "climbs and show them as temporary waypoints. Climbs no "
               "longer listed in the file are removed. These waypoints "
               "are never saved to your waypoint files."),
             settings.enabled, this);

  AddText(_("CUP file link"),
          _("Google Drive share link of the CUP file (shared with "
            "\"Anyone with the link\"), or a direct HTTP(S) URL."),
          settings.url.c_str());

  AddDuration(_("Update interval"),
              _("How often the CUP file is downloaded."),
              RecentClimbsSettings::MIN_INTERVAL,
              RecentClimbsSettings::MAX_INTERVAL,
              RecentClimbsSettings::INTERVAL_STEP,
              settings.interval);

  AddInteger(_("Maximum age"),
             _("Maximum climb age in minutes. Climbs without a valid "
               "observation time are hidden."),
             _("%d min"), "%d",
             RecentClimbsSettings::MIN_MAX_AGE,
             RecentClimbsSettings::MAX_MAX_AGE,
             RecentClimbsSettings::MAX_AGE_STEP, settings.max_age);

  const bool feet = Units::GetUserAltitudeUnit() == Unit::FEET;
  climb_rate_unit = feet ? Unit::FEET_PER_MINUTE : Unit::METER_PER_SECOND;
  AddFloat(_("Minimum climb rate"),
           _("Minimum average climb rate to display. Zero disables this "
             "filter. With a positive minimum, climbs without a valid "
             "climb rate are hidden. Units follow the altitude setting."),
           feet ? "%.0f ft/min" : "%.1f m/s", feet ? "%.0f" : "%.1f",
           0, feet ? 4000 : 20, feet ? 20 : 0.1, false,
           Units::ToUserUnit(settings.min_climb_rate, climb_rate_unit));

  SetEnabled(settings.enabled);
}

bool
RecentClimbsConfigPanel::Save(bool &_changed) noexcept
{
  bool changed = false;

  auto &settings = CommonInterface::SetComputerSettings().recent_climbs;

  changed |= SaveValue(RecentClimbsURL, ProfileKeys::RecentClimbsURL,
                       settings.url);

  changed |= SaveValue(RecentClimbsInterval, ProfileKeys::RecentClimbsInterval,
                       settings.interval);

  changed |= SaveValueInteger(RecentClimbsMaxAge,
                              ProfileKeys::RecentClimbsMaxAge,
                              settings.max_age);

  double rate = Units::ToUserUnit(settings.min_climb_rate, climb_rate_unit);
  if (SaveValue(RecentClimbsMinClimbRate, rate)) {
    settings.min_climb_rate = Units::ToSysUnit(rate, climb_rate_unit);
    Profile::Set(ProfileKeys::RecentClimbsMinClimbRate,
                 settings.min_climb_rate);
    changed = true;
  }

  changed |= SaveValue(RecentClimbsEnabled, ProfileKeys::RecentClimbsEnabled,
                       settings.enabled);

  _changed |= changed;

  return true;
}

std::unique_ptr<Widget>
CreateRecentClimbsConfigPanel() noexcept
{
  return std::make_unique<RecentClimbsConfigPanel>();
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "ConfigurationDialog.hpp"
#include "Device/Driver/OGN/Settings.hpp"
#include "Dialogs/WidgetDialog.hpp"
#include "Language/Language.hpp"
#include "Profile/Profile.hpp"
#include "UIGlobals.hpp"
#include "Units/Units.hpp"
#include "Widget/RowFormWidget.hpp"

#include <algorithm>
#include <cmath>

class OGNConfigurationWidget final : public RowFormWidget {
  enum Controls {
    GLIDER_RANGE,
    OTHER_RANGE,
    VRANGE,
    IGNORE_SAFESKY,
  };

  OGNSettings settings;

  static constexpr double RANGE_STEP_M = 5000;
  static constexpr double OTHER_RANGE_STEP_M = 1000;
  static constexpr double VRANGE_STEP_M = 100;

public:
  explicit OGNConfigurationWidget(const DialogLook &look)
    :RowFormWidget(look) {}

  void Prepare([[maybe_unused]] ContainerWindow &parent,
               [[maybe_unused]] const PixelRect &rc) noexcept override
  {
    LoadFromProfile(settings);

    const Unit distance_u = Units::GetUserDistanceUnit();
    const Unit altitude_u = Units::GetUserAltitudeUnit();

    AddFloat(_("Glider range"),
             _("Gliders further away are ignored. The OGN server sends "
               "all traffic within this range plus 20 km."),
             "%.0f %s", "%.0f",
             Units::ToUserUnit(OGNSettings::MIN_GLIDER_RANGE_M, distance_u),
             Units::ToUserUnit(OGNSettings::MAX_RANGE_M, distance_u),
             Units::ToUserUnit(RANGE_STEP_M, distance_u),
             false,
             UnitGroup::DISTANCE,
             double(settings.glider_range));

    AddFloat(_("Other aircraft range"),
             _("Other aircraft (tow planes, paragliders, ADS-B ...) "
               "further away are ignored. 0 hides them."),
             "%.0f %s", "%.0f",
             0,
             Units::ToUserUnit(OGNSettings::MAX_RANGE_M, distance_u),
             Units::ToUserUnit(OTHER_RANGE_STEP_M, distance_u),
             false,
             UnitGroup::DISTANCE,
             double(settings.other_range));

    AddFloat(_("Vertical Range"),
             _("OGN traffic with a larger altitude difference is "
               "ignored. 0 disables this filter."),
             "%.0f %s", "%.0f",
             0,
             Units::ToUserUnit(OGNSettings::MAX_VRANGE_M, altitude_u),
             Units::ToUserUnit(VRANGE_STEP_M, altitude_u),
             false,
             UnitGroup::ALTITUDE,
             double(settings.vrange));

    AddBoolean(_("Ignore SafeSky relays"),
               _("SafeSky users are also relayed to OGN under another "
                 "id. Ignore these copies when SafeSky traffic is "
                 "received directly through the SafeSky port."),
               settings.ignore_safesky);
  }

  bool Save(bool &_changed) noexcept override
  {
    bool changed = false;

    double glider_m = double(settings.glider_range);
    if (SaveValue(GLIDER_RANGE, UnitGroup::DISTANCE, glider_m)) {
      settings.glider_range = static_cast<unsigned>(std::clamp(
        std::lround(glider_m), long(OGNSettings::MIN_GLIDER_RANGE_M),
        long(OGNSettings::MAX_RANGE_M)));
      changed = true;
    }

    double other_m = double(settings.other_range);
    if (SaveValue(OTHER_RANGE, UnitGroup::DISTANCE, other_m)) {
      settings.other_range = static_cast<unsigned>(std::clamp(
        std::lround(other_m), 0L, long(OGNSettings::MAX_RANGE_M)));
      changed = true;
    }

    double v_m = double(settings.vrange);
    if (SaveValue(VRANGE, UnitGroup::ALTITUDE, v_m)) {
      settings.vrange = static_cast<unsigned>(std::clamp(
        std::lround(v_m), 0L, long(OGNSettings::MAX_VRANGE_M)));
      changed = true;
    }

    changed |= SaveValue(IGNORE_SAFESKY, settings.ignore_safesky);

    if (changed) {
      SaveToProfile(settings);
      Profile::Save();

      /* applied to the running OGN port immediately; a new range
         filter is sent to the server */
      ogn_settings = settings;
    }

    _changed |= changed;
    return true;
  }
};

void
ManageOGNDialog() noexcept
{
  const DialogLook &look = UIGlobals::GetDialogLook();

  WidgetDialog dialog(WidgetDialog::Auto{}, UIGlobals::GetMainWindow(),
                      look, C_("Menu", "OGN Setup"),
                      new OGNConfigurationWidget(look));

  dialog.AddButton(_("OK"), mrOK);
  dialog.AddButton(_("Cancel"), mrCancel);
  dialog.ShowModal();
}

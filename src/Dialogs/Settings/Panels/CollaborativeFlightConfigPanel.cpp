// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "CollaborativeFlightConfigPanel.hpp"
#include "CollaborativeFlight/Core.hpp"
#include "CollaborativeFlight/Settings.hpp"
#include "Profile/Keys.hpp"
#include "Profile/Profile.hpp"
#include "Language/Language.hpp"
#include "Widget/RowFormWidget.hpp"
#include "Form/DataField/Boolean.hpp"
#include "Form/DataField/Listener.hpp"
#include "Interface.hpp"
#include "UIGlobals.hpp"
#include "Dialogs/Message.hpp"
#include "LogFile.hpp"
#include "Units/Units.hpp"

enum ControlIndex {
  ENABLED, FOLDER, URL, TOKEN, IDENTITY, INTERVAL, DEBUG,
  CUP_URL, DISPLAY_INTERVAL, MAX_AGE, MIN_CLIMB_RATE,
};

class CollaborativeFlightConfigPanel final
  : public RowFormWidget, DataFieldListener {
  Unit climb_rate_unit = Unit::METER_PER_SECOND;

public:
  CollaborativeFlightConfigPanel() noexcept
    :RowFormWidget(UIGlobals::GetDialogLook()) {}

  void Prepare(ContainerWindow &parent, const PixelRect &rc) noexcept override;
  bool Save(bool &changed) noexcept override;

private:
  void EnableRows(bool enabled) noexcept {
    for (unsigned i = FOLDER; i <= MIN_CLIMB_RATE; ++i)
      SetRowEnabled(i, enabled);
  }

  void OnModified(DataField &df) noexcept override {
    if (IsDataField(ENABLED, df))
      EnableRows(static_cast<const DataFieldBoolean &>(df).GetValue());
  }
};

void
CollaborativeFlightConfigPanel::Prepare(ContainerWindow &parent,
                                       const PixelRect &rc) noexcept
{
  const auto &s = CommonInterface::GetComputerSettings().collaborative_flight;
  RowFormWidget::Prepare(parent, rc);
  AddBoolean(_("Collaborative flight"),
             _("Share your detected climbs and display the group's climbs "
               "independently of Recent climbs. Locations, UTC times, "
               "altitudes, climb rates and your public identity are shared, "
               "not full tracks. "
               "Offline summaries are retained for 24 hours."),
             s.enabled, this);
  AddText(_("Shared folder link"),
          _("Google Drive folder share link. Deploy the supplied Google "
            "Apps Script receiver for this folder before enabling."),
          s.folder_url.c_str());
  AddText(_("Upload service URL"),
          _("Deployed Google Apps Script web app HTTPS URL ending in /exec."),
          s.upload_url.c_str());
  AddPassword(_("Upload secret"),
              _("Shared receiver secret, 32 to 256 characters. Stored "
                "in your profile; do not share the profile or the secret."),
              s.token.c_str());
  AddText(_("Public identity"),
          _("Name shared with the group, 1 to 10 Unicode characters. "
            "Initially your pilot name and glider registration, truncated "
            "to 10 characters. Climb names add a five-character hash. "
            "Changing this affects only newly detected climbs."),
          s.public_identity.c_str());
  AddDuration(_("Upload interval"),
              _("Minimum time between upload attempts, including retries."),
              CollaborativeFlightSettings::MIN_INTERVAL,
              CollaborativeFlightSettings::MAX_INTERVAL,
              CollaborativeFlightSettings::INTERVAL_STEP, s.interval);
  AddBoolean(_("Log transfers"),
             _("Log upload counts, acknowledgements and CPU deferrals. "
               "Errors are always logged. Secrets are never logged."),
             s.debug);
  AddText(_("CUP file link"),
          _("Public file share link of the collaborative group's climbs.cup, "
            "not the folder link. Leave blank to share without displaying. "
            "The Recent climbs service keeps its own separate file link."),
          s.display.url.c_str());
  AddDuration(_("Update interval"),
              _("How often the CUP file is downloaded."),
              RecentClimbsSettings::MIN_INTERVAL,
              RecentClimbsSettings::MAX_INTERVAL,
              RecentClimbsSettings::INTERVAL_STEP, s.display.interval);
  AddInteger(_("Maximum age"),
             _("Maximum climb age in minutes. Climbs without a valid "
               "observation time are hidden."),
             _("%d min"), "%d",
             RecentClimbsSettings::MIN_MAX_AGE,
             RecentClimbsSettings::MAX_MAX_AGE,
             RecentClimbsSettings::MAX_AGE_STEP, s.display.max_age);
  const bool feet = Units::GetUserAltitudeUnit() == Unit::FEET;
  climb_rate_unit = feet ? Unit::FEET_PER_MINUTE : Unit::METER_PER_SECOND;
  AddFloat(_("Minimum climb rate"),
           _("Minimum average climb rate to display. Zero disables this "
             "filter. With a positive minimum, climbs without a valid "
             "climb rate are hidden. Units follow the altitude setting."),
           feet ? "%.0f ft/min" : "%.1f m/s", feet ? "%.0f" : "%.1f",
           0, feet ? 4000 : 20, feet ? 20 : 0.1, false,
           Units::ToUserUnit(s.display.min_climb_rate, climb_rate_unit));
  EnableRows(s.enabled);
}

bool
CollaborativeFlightConfigPanel::Save(bool &_changed) noexcept
{
  auto &settings =
    CommonInterface::SetComputerSettings().collaborative_flight;
  auto next = settings;
  SaveValue(ENABLED, next.enabled);
  SaveValue(FOLDER, next.folder_url);
  SaveValue(URL, next.upload_url);
  SaveValue(TOKEN, next.token);
  SaveValue(IDENTITY, next.public_identity);
  SaveValue(INTERVAL, next.interval);
  SaveValue(DEBUG, next.debug);
  try {
    CollaborativeFlight::ValidatePublicIdentity(next.public_identity);
  } catch (...) {
    LogError(std::current_exception(),
             "Collaborative flight: invalid public identity");
    ShowMessageBox(_("Enter a public identity of 1 to 10 Unicode characters "
                     "without control characters."),
                   _("Collaborative flight"), MB_OK);
    return false;
  }
  if (next.enabled) {
    try {
      CollaborativeFlight::ValidateUploadUrl(next.upload_url);
      CollaborativeFlight::FolderId(next.folder_url);
      if (next.token.size() < 32 || next.token.size() > 256)
        throw std::runtime_error("Upload secret must be 32-256 characters");
    } catch (...) {
      LogError(std::current_exception(),
               "Collaborative flight: invalid configuration");
      ShowMessageBox(_("Enter a valid Google Drive folder link, Apps Script "
                       "/exec URL and an upload secret of 32 to 256 characters."),
                     _("Collaborative flight"), MB_OK);
      return false;
    }
  }

  bool changed = false;
  changed |= SaveValue(ENABLED, ProfileKeys::CollaborativeFlightEnabled,
                       settings.enabled);
  changed |= SaveValue(FOLDER, ProfileKeys::CollaborativeFlightFolder,
                       settings.folder_url);
  changed |= SaveValue(URL, ProfileKeys::CollaborativeFlightURL,
                       settings.upload_url);
  changed |= SaveValue(TOKEN, ProfileKeys::CollaborativeFlightToken,
                       settings.token);
  changed |= SaveValue(IDENTITY, ProfileKeys::CollaborativeFlightIdentity,
                       settings.public_identity);
  if (Profile::Get(ProfileKeys::CollaborativeFlightIdentity) == nullptr) {
    Profile::Set(ProfileKeys::CollaborativeFlightIdentity,
                 settings.public_identity);
    changed = true;
  }
  changed |= SaveValue(INTERVAL, ProfileKeys::CollaborativeFlightInterval,
                       settings.interval);
  changed |= SaveValue(DEBUG, ProfileKeys::CollaborativeFlightDebug,
                       settings.debug);
  changed |= SaveValue(CUP_URL, ProfileKeys::CollaborativeClimbsURL,
                       settings.display.url);
  changed |= SaveValue(DISPLAY_INTERVAL,
                       ProfileKeys::CollaborativeClimbsInterval,
                       settings.display.interval);
  changed |= SaveValueInteger(MAX_AGE, ProfileKeys::CollaborativeClimbsMaxAge,
                              settings.display.max_age);
  double rate = Units::ToUserUnit(settings.display.min_climb_rate,
                                  climb_rate_unit);
  if (SaveValue(MIN_CLIMB_RATE, rate)) {
    settings.display.min_climb_rate = Units::ToSysUnit(rate, climb_rate_unit);
    Profile::Set(ProfileKeys::CollaborativeClimbsMinClimbRate,
                 settings.display.min_climb_rate);
    changed = true;
  }
  _changed |= changed;
  return true;
}

std::unique_ptr<Widget>
CreateCollaborativeFlightConfigPanel() noexcept
{
  return std::make_unique<CollaborativeFlightConfigPanel>();
}

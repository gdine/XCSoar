// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Device/Driver/OGN.hpp"
#include "Device.hpp"
#include "Settings.hpp"

static Device *
OGNCreateOnPort([[maybe_unused]] const DeviceConfig &config, Port &com_port)
{
  LoadFromProfile(ogn_settings);
  return new OGNDevice(com_port);
}

const struct DeviceRegister ogn_driver = {
  "OGN",
  "OGN APRS",
  DeviceRegister::MANAGE | DeviceRegister::RAW_GPS_DATA |
  DeviceRegister::PERIODIC_UPDATE,
  OGNCreateOnPort,
};

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Device/Driver/GDL90.hpp"
#include "Driver.hpp"
#include "Device/Config.hpp"

static Device *
GDL90CreateOnPort(const DeviceConfig &config,
                  [[maybe_unused]] Port &com_port)
{
  LoadFromProfile(gdl90_settings);

  const bool safesky =
    config.port_type == DeviceConfig::PortType::SAFESKY_GDL90;
  if (safesky)
    LoadFromProfile(safesky_settings);

  return new GDL90Device(safesky);
}

const struct DeviceRegister gdl90_driver = {
  "GDL90",
  "GDL90",
  DeviceRegister::MANAGE | DeviceRegister::RAW_GPS_DATA,
  GDL90CreateOnPort,
};


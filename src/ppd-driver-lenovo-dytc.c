/*
 * Copyright (c) 2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include <gudev/gudev.h>
#include <gio/gio.h>

#include "ppd-driver-lenovo-dytc.h"
#include "ppd-utils.h"
#include "up-input.h"
#ifndef SW_LAP_PROXIMITY
#include "input-event-codes.h"
#endif

#define PERFMODE_SYSFS_NAME "dytc_perfmode"

struct _PpdDriverLenovoDytc
{
  PpdDriver  parent_instance;

  GUdevDevice *device;
  gboolean lapmode;
  PpdProfile perfmode;
  UpInput *lapmode_mon;
  GFileMonitor *perfmode_mon;
  guint perfmode_changed_id;
};

G_DEFINE_TYPE (PpdDriverLenovoDytc, ppd_driver_lenovo_dytc, PPD_TYPE_DRIVER)

static GObject*
ppd_driver_lenovo_dytc_constructor (GType                  type,
                                    guint                  n_construct_params,
                                    GObjectConstructParam *construct_params)
{
  GObject *object;

  object = G_OBJECT_CLASS (ppd_driver_lenovo_dytc_parent_class)->constructor (type,
                                                                              n_construct_params,
                                                                              construct_params);
  g_object_set (object,
                "driver-name", "lenovo_dytc",
                "profiles", PPD_PROFILE_PERFORMANCE | PPD_PROFILE_BALANCED | PPD_PROFILE_POWER_SAVER,
                NULL);

  return object;
}

static const char *
profile_to_perfmode_value (PpdProfile profile)
{
  switch (profile) {
  case PPD_PROFILE_POWER_SAVER:
    return "L";
  case PPD_PROFILE_BALANCED:
    return "M";
  case PPD_PROFILE_PERFORMANCE:
    return "H";
  }

  g_assert_not_reached ();
}

static PpdProfile
perfmode_value_to_profile (const char *str)
{
  if (str == NULL)
    return PPD_PROFILE_UNSET;

  switch (str[0]) {
  case 'L':
    return PPD_PROFILE_POWER_SAVER;
  case 'M':
    return PPD_PROFILE_BALANCED;
  case 'H':
    return PPD_PROFILE_PERFORMANCE;
  default:
    g_debug ("Got unsupported perfmode value '%s'", str);
  }

  return PPD_PROFILE_UNSET;
}

static void
update_dytc_lapmode_state (PpdDriverLenovoDytc *dytc)
{
  gboolean new_lapmode;

  new_lapmode = up_input_get_switch_value (dytc->lapmode_mon);
  if (new_lapmode == dytc->lapmode)
    return;

  dytc->lapmode = new_lapmode;
  g_debug ("dytc_lapmode is now %s, so profile is %s",
           dytc->lapmode ? "on" : "off",
           dytc->lapmode ? "inhibited" : "uninhibited");
  g_object_set (G_OBJECT (dytc),
                "performance-inhibited", dytc->lapmode ? "lap-detected" : NULL,
                NULL);
}

static void
update_dytc_perfmode_state (PpdDriverLenovoDytc *dytc)
{
  const char *new_profile_str;
  PpdProfile new_profile;

  new_profile_str = g_udev_device_get_sysfs_attr_uncached (dytc->device, PERFMODE_SYSFS_NAME);
  if (!new_profile_str)
    return;

  new_profile = perfmode_value_to_profile (new_profile_str);
  if (new_profile == PPD_PROFILE_UNSET ||
      new_profile == dytc->perfmode)
    return;

  g_debug ("dytc_perfmode is now %c, so profile is %s",
           new_profile_str[0],
           ppd_profile_to_str (new_profile));
  dytc->perfmode = new_profile;
  ppd_driver_emit_profile_changed (PPD_DRIVER (dytc), new_profile);
}

static void
lapmode_changed (UpInput  *lapmode_mon,
                 gboolean  new_state,
                 gpointer  user_data)
{
  PpdDriverLenovoDytc *dytc = user_data;
  g_debug ("Lapmode switch changed");
  update_dytc_lapmode_state (dytc);
}

static void
perfmode_changed (GFileMonitor      *monitor,
                  GFile             *file,
                  GFile             *other_file,
                  GFileMonitorEvent  event_type,
                  gpointer           user_data)
{
  PpdDriverLenovoDytc *dytc = user_data;
  g_debug (PERFMODE_SYSFS_NAME " attribute changed");
  update_dytc_perfmode_state (dytc);
}

static gboolean
ppd_driver_lenovo_dytc_activate_profile (PpdDriver   *driver,
                                         PpdProfile   profile,
                                         GError     **error)
{
  PpdDriverLenovoDytc *dytc = PPD_DRIVER_LENOVO_DYTC (driver);

  g_return_val_if_fail (dytc->device, FALSE);

  if (dytc->perfmode == profile) {
    g_debug ("Can't switch to %s mode, already there",
             ppd_profile_to_str (profile));
    return TRUE;
  }

  if (profile == PPD_PROFILE_PERFORMANCE &&
      dytc->lapmode) {
    g_debug ("Can't switch to performance mode, lapmode is detected");
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Mode is inhibited");
    return FALSE;
  }

  g_signal_handler_block (G_OBJECT (dytc->perfmode_mon), dytc->perfmode_changed_id);
  if (!ppd_utils_write_sysfs (dytc->device, PERFMODE_SYSFS_NAME, profile_to_perfmode_value (profile), error)) {
    g_debug ("Failed to write to perfmode: %s", (* error)->message);
    g_signal_handler_unblock (G_OBJECT (dytc->perfmode_mon), dytc->perfmode_changed_id);
    return FALSE;
  }
  g_signal_handler_unblock (G_OBJECT (dytc->perfmode_mon), dytc->perfmode_changed_id);

  g_debug ("Successfully switched to profile %s", ppd_profile_to_str (profile));
  dytc->perfmode = profile;
  return TRUE;
}

static int
find_dytc (GUdevDevice *dev,
           gpointer     user_data)
{
  if (g_strcmp0 (g_udev_device_get_name (dev), "thinkpad_acpi") != 0)
    return 1;

  if (!g_udev_device_get_sysfs_attr (dev, PERFMODE_SYSFS_NAME))
    return 1;

  return 0;
}

static int
find_lap_prox_switch (GUdevDevice *dev,
                      gpointer     user_data)
{
  g_autoptr(GUdevDevice) parent = NULL;
  parent = g_udev_device_get_parent (dev);
  if (!parent)
    return -1;
  return g_strcmp0 (g_udev_device_get_property (parent, "NAME"), "\"Thinkpad proximity switches\"");
}

static gboolean
ppd_driver_lenovo_dytc_probe (PpdDriver *driver)
{
  PpdDriverLenovoDytc *dytc = PPD_DRIVER_LENOVO_DYTC (driver);
  g_autoptr(GUdevDevice) lap_prox_switch = NULL;

  g_return_val_if_fail (!dytc->device, FALSE);

  lap_prox_switch = ppd_utils_find_device ("input",
                                           (GCompareFunc) find_lap_prox_switch,
                                           NULL);
  if (!lap_prox_switch) {
    g_debug ("Could not find lap proximity switch");
    goto out;
  }

  dytc->device = ppd_utils_find_device ("platform",
                                        (GCompareFunc) find_dytc,
                                        NULL);
  if (!dytc->device) {
    g_debug ("Could not find perfmode sysfs attribute");
    goto out;
  }

  dytc->lapmode_mon = up_input_new_for_switch (SW_LAP_PROXIMITY);
  if (!up_input_coldplug (dytc->lapmode_mon, lap_prox_switch)) {
    g_debug ("Could not monitor lap proximity switch");
    g_clear_object (&dytc->device);
    goto out;
  }
  g_signal_connect (G_OBJECT (dytc->lapmode_mon), "switch-changed",
                    G_CALLBACK (lapmode_changed), dytc);

  dytc->perfmode_mon = ppd_utils_monitor_sysfs_attr (dytc->device,
                                                    PERFMODE_SYSFS_NAME,
                                                    NULL);
  dytc->perfmode_changed_id = g_signal_connect (G_OBJECT (dytc->perfmode_mon), "changed",
                                                G_CALLBACK (perfmode_changed), dytc);
  update_dytc_lapmode_state (dytc);
  update_dytc_perfmode_state (dytc);

out:
  g_debug ("%s a lap proximity input device, or perfmode sysfs attribute",
           dytc->device ? "Found" : "Didn't find");
  return (dytc->device != NULL);
}

static void
ppd_driver_lenovo_dytc_finalize (GObject *object)
{
  PpdDriverLenovoDytc *driver;

  driver = PPD_DRIVER_LENOVO_DYTC (object);
  g_clear_object (&driver->device);
  g_clear_object (&driver->lapmode_mon);
  g_clear_object (&driver->perfmode_mon);
  G_OBJECT_CLASS (ppd_driver_lenovo_dytc_parent_class)->finalize (object);
}

static void
ppd_driver_lenovo_dytc_class_init (PpdDriverLenovoDytcClass *klass)
{
  GObjectClass *object_class;
  PpdDriverClass *driver_class;

  object_class = G_OBJECT_CLASS(klass);
  object_class->constructor = ppd_driver_lenovo_dytc_constructor;
  object_class->finalize = ppd_driver_lenovo_dytc_finalize;

  driver_class = PPD_DRIVER_CLASS(klass);
  driver_class->probe = ppd_driver_lenovo_dytc_probe;
  driver_class->activate_profile = ppd_driver_lenovo_dytc_activate_profile;
}

static void
ppd_driver_lenovo_dytc_init (PpdDriverLenovoDytc *self)
{
}

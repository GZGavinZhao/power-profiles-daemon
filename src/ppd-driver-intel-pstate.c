/*
 * Copyright (c) 2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include <upower.h>

#include "ppd-utils.h"
#include "ppd-driver-intel-pstate.h"

#define CPUFREQ_POLICY_DIR "/devices/system/cpu/cpufreq/"

struct _PpdDriverIntelPstate
{
  PpdDriver  parent_instance;

  UpClient *client;
  PpdProfile activated_profile;
  gboolean on_battery;
  GList *devices; /* GList of paths */
};

G_DEFINE_TYPE (PpdDriverIntelPstate, ppd_driver_intel_pstate, PPD_TYPE_DRIVER)

static gboolean ppd_driver_intel_pstate_activate_profile (PpdDriver   *driver,
                                                          PpdProfile   profile,
                                                          GError     **error);

static GObject*
ppd_driver_intel_pstate_constructor (GType                  type,
                                    guint                  n_construct_params,
                                    GObjectConstructParam *construct_params)
{
  GObject *object;

  object = G_OBJECT_CLASS (ppd_driver_intel_pstate_parent_class)->constructor (type,
                                                                              n_construct_params,
                                                                              construct_params);
  g_object_set (object,
                "driver-name", "intel_pstate",
                "profiles", PPD_PROFILE_PERFORMANCE | PPD_PROFILE_BALANCED | PPD_PROFILE_POWER_SAVER,
                NULL);

  return object;
}

static void
on_battery_changed (GObject    *gobject,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PpdDriverIntelPstate *pstate = user_data;
  gboolean old_on_battery;

  old_on_battery = pstate->on_battery;
  pstate->on_battery = up_client_get_on_battery (pstate->client);

  if (pstate->activated_profile == PPD_PROFILE_BALANCED) {
    ppd_driver_intel_pstate_activate_profile (PPD_DRIVER (pstate),
                                              pstate->activated_profile,
                                              NULL);
  }

  g_debug ("Battery status changed from %s to %s",
           old_on_battery ? "on battery" : "on mains",
           pstate->on_battery ? "on battery" : "on mains");
}

static char *
get_policy_dir (void)
{
  const char *root;
  g_autofree char *dir = NULL;

  root = g_getenv ("UMOCKDEV_DIR");
  if (!root || *root == '\0')
    root = "/sys";

  return g_build_filename (root, CPUFREQ_POLICY_DIR, NULL);
}

static GDir *
open_policy_dir (void)
{
  g_autofree char *dir = NULL;
  dir = get_policy_dir ();
  return g_dir_open (dir, 0, NULL);
}

static gboolean
ppd_driver_intel_pstate_probe (PpdDriver *driver)
{
  PpdDriverIntelPstate *pstate = PPD_DRIVER_INTEL_PSTATE (driver);
  g_autoptr(GDir) dir = NULL;
  g_autofree char *policy_dir = NULL;
  const char *dirname;
  gboolean ret = FALSE;

  dir = open_policy_dir ();
  if (!dir)
    goto out;

  policy_dir = get_policy_dir ();
  while ((dirname = g_dir_read_name (dir)) != NULL) {
    g_autofree char *path = NULL;

    path = g_build_filename (policy_dir,
                             dirname,
                             "energy_performance_preference",
                             NULL);
    if (!g_file_test (path, G_FILE_TEST_EXISTS))
      continue;

    pstate->devices = g_list_prepend (pstate->devices, g_steal_pointer (&path));
    ret = TRUE;
  }

  if (ret)
    pstate->client = up_client_new ();

  if (pstate->client) {
    g_signal_connect (G_OBJECT (pstate->client), "notify::on-battery",
                      G_CALLBACK (on_battery_changed), pstate);
    pstate->on_battery = up_client_get_on_battery (pstate->client);
  }

out:
  g_debug ("%s p-state settings",
           ret ? "Found" : "Didn't find");
  return ret;
}

static const char *
profile_to_pref (PpdProfile profile,
                 gboolean   on_battery)
{
  /* Note that we don't check "energy_performance_available_preferences"
   * as all the values are always available */
  switch (profile) {
  case PPD_PROFILE_POWER_SAVER:
    return "power";
  case PPD_PROFILE_BALANCED:
    if (on_battery)
      return "balance_power";
    return "balance_performance";
  case PPD_PROFILE_PERFORMANCE:
    return "performance";
  }

  g_assert_not_reached ();
}

static gboolean
ppd_driver_intel_pstate_activate_profile (PpdDriver   *driver,
                                          PpdProfile   profile,
                                          GError     **error)
{
  PpdDriverIntelPstate *pstate = PPD_DRIVER_INTEL_PSTATE (driver);
  gboolean ret = TRUE;
  const char *pref;
  GList *l;

  g_return_val_if_fail (pstate->devices != NULL, FALSE);

  pref = profile_to_pref (profile, pstate->on_battery);

  for (l = pstate->devices; l != NULL; l = l->next) {
    const char *path = l->data;

    ret = ppd_utils_write (path, pref, error);
    if (!ret)
      break;
  }

  if (ret)
    pstate->activated_profile = profile;

  return ret;
}

static void
ppd_driver_intel_pstate_finalize (GObject *object)
{
  PpdDriverIntelPstate *driver;

  driver = PPD_DRIVER_INTEL_PSTATE (object);
  g_clear_list (&driver->devices, g_free);
  g_clear_object (&driver->client);
  G_OBJECT_CLASS (ppd_driver_intel_pstate_parent_class)->finalize (object);
}

static void
ppd_driver_intel_pstate_class_init (PpdDriverIntelPstateClass *klass)
{
  GObjectClass *object_class;
  PpdDriverClass *driver_class;

  object_class = G_OBJECT_CLASS(klass);
  object_class->constructor = ppd_driver_intel_pstate_constructor;
  object_class->finalize = ppd_driver_intel_pstate_finalize;

  driver_class = PPD_DRIVER_CLASS(klass);
  driver_class->probe = ppd_driver_intel_pstate_probe;
  driver_class->activate_profile = ppd_driver_intel_pstate_activate_profile;
}

static void
ppd_driver_intel_pstate_init (PpdDriverIntelPstate *self)
{
}
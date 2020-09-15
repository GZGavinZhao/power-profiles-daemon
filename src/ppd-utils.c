/*
 * Copyright (c) 2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include "ppd-utils.h"
#include <gio/gio.h>
#include <stdio.h>
#include <errno.h>

gboolean ppd_utils_write (const char  *filename,
                          const char  *value,
                          GError     **error)
{
  FILE *sysfsfp;
  int ret;

  g_return_val_if_fail (filename, FALSE);
  g_return_val_if_fail (value, FALSE);

  sysfsfp = fopen (filename, "w");
  if (sysfsfp == NULL) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Could not open '%s' for writing", filename);
    g_debug ("Could not open for writing '%s'", filename);
    return FALSE;
  }
  ret = fprintf (sysfsfp, "%s", value);
  if (ret < 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Error writing '%s'", filename);
    g_debug ("Error writing '%s'", filename);
    return FALSE;
  }
  fclose (sysfsfp);
  return TRUE;
}

gboolean ppd_utils_write_sysfs (GUdevDevice  *device,
                                const char   *attribute,
                                const char   *value,
                                GError      **error)
{
  g_autofree char *filename = NULL;

  g_return_val_if_fail (G_UDEV_IS_DEVICE (device), FALSE);
  g_return_val_if_fail (attribute, FALSE);
  g_return_val_if_fail (value, FALSE);

  filename = g_build_filename (g_udev_device_get_sysfs_path (device), attribute, NULL);
  return ppd_utils_write (filename, value, error);
}

GFileMonitor *
ppd_utils_monitor_sysfs_attr (GUdevDevice  *device,
                              const char   *attribute,
                              GError      **error)
{
  g_autofree char *path = NULL;
  g_autoptr(GFile) file = NULL;

  path = g_build_filename (g_udev_device_get_sysfs_path (device), attribute, NULL);
  file = g_file_new_for_path (path);
  return g_file_monitor_file (file,
                              G_FILE_MONITOR_NONE,
                              NULL,
                              error);
}

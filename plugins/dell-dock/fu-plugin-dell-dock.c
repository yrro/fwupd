/*
 * Copyright (C) 2018 Dell Inc.
 * All rights reserved.
 *
 * This software and associated documentation (if any) is furnished
 * under a license and may only be used or copied in accordance
 * with the terms of the license.
 *
 * This file is provided under a dual MIT/LGPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 * Dell Chooses the MIT license part of Dual MIT/LGPLv2 license agreement.
 *
 * SPDX-License-Identifier: LGPL-2.1+ OR MIT
 */

#include "config.h"

#include "fu-device.h"
#include "fwupd-error.h"
#include "fu-plugin-vfuncs.h"
#include "fu-hash.h"

#include "fu-dell-dock-common.h"

void
fu_plugin_init (FuPlugin *plugin)
{
	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);

	/* allow these to be built by quirks */
	g_type_ensure (FU_TYPE_DELL_DOCK_STATUS);
	g_type_ensure (FU_TYPE_DELL_DOCK_MST);

	/* currently slower performance, but more reliable in corner cases */
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_BETTER_THAN, "synaptics_mst");
}

static gboolean
fu_plugin_dell_dock_create_node (FuPlugin *plugin,
				 FuDevice *device,
				 GError **error)
{
	g_autoptr(FuDeviceLocker) locker = NULL;

	fu_device_set_quirks (device, fu_plugin_get_quirks (plugin));
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	return TRUE;
}

gboolean
fu_plugin_usb_device_added (FuPlugin *plugin,
			    FuUsbDevice *device,
			    GError **error)
{
	g_autoptr(FuDevice) hub_device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* create a new opened device based on the generic USB device */
	hub_device = FU_DEVICE (fu_dell_dock_hub_new (device));
	locker = fu_device_locker_new (hub_device, error);
	if (locker == NULL)
		return FALSE;
	fu_plugin_device_add (plugin, hub_device);

	/* create EC parent */
	if (fu_device_has_custom_flag (hub_device, "has-bridge")) {
		const gchar *hub_id = NULL;
		g_autoptr(FuDevice) ec_device = NULL;
		g_autoptr(GError) error_local = NULL;

		/* does the hub already exist? */
		hub_id = fu_device_get_id (hub_device);
		if (fu_plugin_cache_lookup (plugin, hub_id) != NULL) {
			g_warning ("ignoring already added device %s", hub_id);
			return TRUE;
		}

		/* create all virtual devices */
		ec_device = FU_DEVICE (fu_dell_dock_ec_new (hub_device));
		if (!fu_plugin_dell_dock_create_node (plugin, ec_device, &error_local)) {
			g_warning ("failed to probe bridged devices for %s: %s",
				   hub_id, error_local->message);
			return TRUE;
		}

		/* create TBT endpoint if Thunderbolt SKU and Thunderbolt link inactive */
		if (fu_dell_dock_ec_needs_tbt (ec_device)) {
			g_autoptr(FuDevice) tbt_device = FU_DEVICE (fu_dell_dock_tbt_new ());
			fu_device_add_child (ec_device, tbt_device);
			if (!fu_plugin_dell_dock_create_node (plugin, tbt_device, &error_local)) {
				g_warning ("failed to probe TBT device for %s: %s",
					   hub_id, error_local->message);
			}
		}

		/* allow getting the EC device from the HUB */
		fu_plugin_cache_add (plugin, hub_id, ec_device);
		fu_plugin_device_add (plugin, ec_device);
	}

	/* clear updatable flag if parent doesn't have it */
	fu_dell_dock_clone_updatable (hub_device);

	return TRUE;
}

gboolean
fu_plugin_device_removed (FuPlugin *plugin, FuDevice *device, GError **error)
{
	const gchar *device_key = fu_device_get_id (device);
	FuDevice *ec_device;

	/* get the parent EC device from the just-removed HUB device ID */
	ec_device = fu_plugin_cache_lookup (plugin, device_key);
	if (ec_device == NULL)
		return TRUE;

	/* remove virtual parent and also remove child devices */
	g_debug ("removing virtual EC for %s (%s)",
		 fu_device_get_name (ec_device),
		 fu_device_get_id (ec_device));
	fu_plugin_device_remove (plugin, ec_device);
	fu_plugin_cache_remove (plugin, device_key);
	return TRUE;
}

/* get the virtual EC for any device in the array */
static FuDevice *
fu_plugin_dell_dock_get_ec (FuPlugin *plugin, GPtrArray *devices)
{
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		FuDevice *dev_tmp;
		if (FU_IS_DELL_DOCK_EC (dev))
			return dev;
		dev_tmp = fu_plugin_cache_lookup (plugin, fu_device_get_id (dev));
		if (dev_tmp != NULL)
			return dev_tmp;
	}
	return NULL;
}

gboolean
fu_plugin_composite_prepare (FuPlugin *plugin,
			     GPtrArray *devices,
			     GError **error)
{
	FuDevice *parent = fu_plugin_dell_dock_get_ec (plugin, devices);
	gboolean remaining_replug = FALSE;

	if (parent == NULL)
		return TRUE;

	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		/* if thunderbolt is part of transaction our family is leaving us */
		if (g_strcmp0 (fu_device_get_plugin (dev), "thunderbolt") == 0) {
			if (fu_device_get_parent (dev) != parent)
				continue;
			fu_dell_dock_will_replug (parent);
			/* set all other devices to replug */
			remaining_replug = TRUE;
			continue;
		}
		/* different device */
		if (fu_device_get_parent (dev) != parent)
			continue;
		if (remaining_replug)
			fu_dell_dock_will_replug (dev);
	}

	return TRUE;
}

gboolean
fu_plugin_composite_cleanup (FuPlugin *plugin,
			     GPtrArray *devices,
			     GError **error)
{
	FuDevice *parent = fu_plugin_dell_dock_get_ec (plugin, devices);
	g_autoptr(FuDeviceLocker) locker = NULL;

	if (parent == NULL)
		return TRUE;

	locker = fu_device_locker_new (parent, error);
	if (locker == NULL)
		return FALSE;

	return fu_dell_dock_ec_reboot_dock (parent, error);
}

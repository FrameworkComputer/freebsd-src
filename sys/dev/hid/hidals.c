/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Daniel Schaefer <dhs@frame.work>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
/*
 * HID spec: https://usb.org/sites/default/files/hut1_5.pdf
 * First proposed in HUTRR39: https://www.usb.org/sites/default/files/hutrr39b_0.pdf
 */

#include "opt_hid.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include <dev/hid/hid.h>
#include <dev/hid/hidbus.h>
#include <dev/hid/hidmap.h>
#include <dev/hid/hidquirk.h>
#include <dev/hid/hidrdesc.h>

static const uint8_t hidals_boot_desc[] = { HID_MOUSE_BOOTPROTO_DESCR() };

enum {
	HIDALS_ILL,
};

#define HIDALS_MAP_ABS(usage, code)	\
	{ HIDMAP_ABS(HUP_SENSORS, usage, code) }

static const struct hidmap_item hidals_map[] = {
	/**
	 * Let's keep it as evdev ABS_X right now.
	 * Later we can change it to a different API.
	 */
	[HIDALS_ILL]	= HIDALS_MAP_ABS(HUS_ILLUMINATION,		ABS_X),
};

/* A match on these entries will load hidals */
static const struct hid_device_id hidals_devs[] = {
	{ HID_TLC(HUP_SENSORS, HUS_AMBIENT_LIGHT) },
};

struct hidals_softc {
	struct hidmap		hm;
};

static void
hidals_identify(driver_t *driver, device_t parent)
{
	const struct hid_device_info *hw = hid_get_device_info(parent);

	// Only needed if we find a device that needs fixing up
}

static int
hidals_probe(device_t dev)
{
	struct hidals_softc *sc = device_get_softc(dev);
	int error;

	error = HIDBUS_LOOKUP_DRIVER_INFO(dev, hidals_devs);
	if (error != 0)
		return (error);

	hidmap_set_dev(&sc->hm, dev);

	/* Check if report descriptor belongs to an ALS sensor */
	error = HIDMAP_ADD_MAP(&sc->hm, hidals_map, NULL);
	if (error != 0)
		return (error);

	/* There should be at least one X or Y axis */
	if (hidmap_test_cap(sc->caps, HIDALS_ILL))
		return (ENXIO);

	hidbus_set_desc(dev, "ALS Sensor");

	return (BUS_PROBE_GENERIC);
}

static int
hidals_attach(device_t dev)
{
	struct hidals_softc *sc = device_get_softc(dev);
	const struct hid_device_info *hw = hid_get_device_info(dev);
	struct hidmap_hid_item *hi;
	HIDMAP_CAPS(cap_wheel, hidals_map_wheel);
	void *d_ptr;
	hid_size_t d_len;
	bool set_report_proto;
	int error, nbuttons = 0;

	/*
	 * Set the report (non-boot) protocol if report descriptor has not been
	 * overloaded with boot protocol report descriptor.
	 *
	 * Mice without boot protocol support may choose not to implement
	 * Set_Protocol at all; Ignore any error.
	 */
	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	set_report_proto = !(error == 0 && d_len == sizeof(hidals_boot_desc) &&
	    memcmp(d_ptr, hidals_boot_desc, sizeof(hidals_boot_desc)) == 0);
	(void)hid_set_protocol(dev, set_report_proto ? 1 : 0);

	if (hid_test_quirk(hw, HQ_MS_REVZ))
		HIDMAP_ADD_MAP(&sc->hm, hidals_map_wheel_rev, cap_wheel);
	else
		HIDMAP_ADD_MAP(&sc->hm, hidals_map_wheel, cap_wheel);

	if (hid_test_quirk(hw, HQ_MS_VENDOR_BTN))
		HIDMAP_ADD_MAP(&sc->hm, hidals_map_kensington_slimblade, NULL);

	error = hidmap_attach(&sc->hm);
	if (error)
		return (error);

	/* Count number of input usages of variable type mapped to buttons */
	for (hi = sc->hm.hid_items;
	     hi < sc->hm.hid_items + sc->hm.nhid_items;
	     hi++)
		if (hi->type == HIDMAP_TYPE_VARIABLE && hi->evtype == EV_KEY)
			nbuttons++;

	/* announce information about the mouse */
	device_printf(dev, "%d buttons and [%s%s%s%s%s] coordinates ID=%u\n",
	    nbuttons,
	    (hidmap_test_cap(sc->caps, HMS_REL_X) ||
	     hidmap_test_cap(sc->caps, HMS_ABS_X)) ? "X" : "",
	    (hidmap_test_cap(sc->caps, HMS_REL_Y) ||
	     hidmap_test_cap(sc->caps, HMS_ABS_Y)) ? "Y" : "",
	    (hidmap_test_cap(sc->caps, HMS_REL_Z) ||
	     hidmap_test_cap(sc->caps, HMS_ABS_Z)) ? "Z" : "",
	    hidmap_test_cap(cap_wheel, 0) ? "W" : "",
	    hidmap_test_cap(sc->caps, HMS_HWHEEL) ? "H" : "",
	    sc->hm.hid_items[0].id);

	return (0);
}

static int
hidals_detach(device_t dev)
{
	struct hidals_softc *sc = device_get_softc(dev);
	int error;

	error = hidmap_detach(&sc->hm);
	return (error);
}

static device_method_t hidals_methods[] = {
	DEVMETHOD(device_identify,	hidals_identify),
	DEVMETHOD(device_probe,		hidals_probe),
	DEVMETHOD(device_attach,	hidals_attach),
	DEVMETHOD(device_detach,	hidals_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(hidals, hidals_driver, hidals_methods, sizeof(struct hidals_softc));
DRIVER_MODULE(hidals, hidbus, hidals_driver, NULL, NULL);
MODULE_DEPEND(hidals, hid, 1, 1, 1);
MODULE_DEPEND(hidals, hidbus, 1, 1, 1);
MODULE_DEPEND(hidals, hidmap, 1, 1, 1);
MODULE_DEPEND(hidals, evdev, 1, 1, 1);
MODULE_VERSION(hidals, 1);
HID_PNP_INFO(hidals_devs);

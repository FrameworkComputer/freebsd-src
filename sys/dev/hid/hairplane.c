/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2024 Framework Computer Inc
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
 * General Desktop/System Controls usage page driver
 * https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
 */

#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include <dev/hid/hid.h>
#include <dev/hid/hidbus.h>
#include <dev/hid/hidmap.h>

#define HUG_RADIO_CONTROL       0x000c
#define HUG_RADIO_BUTTON        0x00c6

#define	HRADIO_MAP(usage, code)	\
	{ HIDMAP_KEY(HUP_GENERIC_DESKTOP, HUG_RADIO_##usage, code) }

static const struct hidmap_item hairplane_map[] = {
	// https://github.com/torvalds/linux/blob/d67978318827d06f1c0fa4c31343a279e9df6fde/drivers/hid/hid-input.c#L924
	// TODO: Need to simulate press up
	// https://github.com/torvalds/linux/blob/d67978318827d06f1c0fa4c31343a279e9df6fde/drivers/hid/hid-input.c#L1704
	HRADIO_MAP(BUTTON, 		KEY_MUTE),
};

static const struct hid_device_id hairplane_devs[] = {
	//{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_SYSTEM_CONTROL) },
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_RADIO_CONTROL) },
};

static int
hairplane_probe(device_t dev)
{
	int res = (HIDMAP_PROBE(device_get_softc(dev), dev,
	    hairplane_devs, hairplane_map, "Radio Control"));

	// BUS_PROBE_DEFAULT (-20)
	// This seems to be good, if I don't have HUG_RADIO_CONTROL correct,
	// I get 3x res:6, otherwise 2x res:6 and 2x res:-20
	uprintf("Probing airplane: %d\n", res);
	return res;
}

static int
hairplane_attach(device_t dev)
{
	int res = (hidmap_attach(device_get_softc(dev)));
	uprintf("Attaching airplane: %d\n", res);
	return res;
}

static int
hairplane_detach(device_t dev)
{
	return (hidmap_detach(device_get_softc(dev)));
}

static device_method_t hairplane_methods[] = {
	DEVMETHOD(device_probe,		hairplane_probe),
	DEVMETHOD(device_attach,	hairplane_attach),
	DEVMETHOD(device_detach,	hairplane_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(hairplane, hairplane_driver, hairplane_methods, sizeof(struct hidmap));
DRIVER_MODULE(hairplane, hidbus, hairplane_driver, NULL, NULL);
MODULE_DEPEND(hairplane, hid, 1, 1, 1);
MODULE_DEPEND(hairplane, hidbus, 1, 1, 1);
MODULE_DEPEND(hairplane, hidmap, 1, 1, 1);
MODULE_DEPEND(hairplane, evdev, 1, 1, 1);
MODULE_VERSION(hairplane, 1);
HID_PNP_INFO(hairplane_devs);

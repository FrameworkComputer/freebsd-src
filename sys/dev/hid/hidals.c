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
 * HID Ambient Light Sensor driver.
 *
 * Exposes illuminance (and optionally color temperature) from HID sensor
 * devices via sysctl.  Configurable feature reports (reporting state,
 * power state, report interval) follow the hconf.c pattern.
 *
 * HID spec: https://usb.org/sites/default/files/hut1_5.pdf
 * First proposed in HUTRR39: https://www.usb.org/sites/default/files/hutrr39b_0.pdf
 */

#include "opt_hid.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/sx.h>

#define	HID_DEBUG_VAR	hidals_debug
#include <dev/hid/hid.h>
#include <dev/hid/hidbus.h>

#ifdef HID_DEBUG
static int hidals_debug = 0;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hidals, CTLFLAG_RW, 0,
    "HID Ambient Light Sensor");
SYSCTL_INT(_hw_hid_hidals, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hidals_debug, 1, "Debug level");
#endif

enum hidals_feature {
	HIDALS_FEAT_REPORTING_STATE = 0,
	HIDALS_FEAT_POWER_STATE,
	HIDALS_FEAT_REPORT_INTERVAL,
	HIDALS_FEAT_COUNT
};

struct hidals_feature_descr {
	const char	*name;
	const char	*descr;
	uint16_t	usage;
	u_int		value;
};

static const struct hidals_feature_descr hidals_feature_descrs[] = {
	[HIDALS_FEAT_REPORTING_STATE] = {
		.name = "reporting_state",
		.descr = "Reporting state: 0=no events, 1=all events, "
		    "2=threshold events",
		.usage = HUS_REPORTING_STATE,
		.value = 1,	/* All events */
	},
	[HIDALS_FEAT_POWER_STATE] = {
		.name = "power_state",
		.descr = "Power state: 1=D0 (full power), 2=D1 (low power), "
		    "3=D2, 4=D3, 5=D4 (off)",
		.usage = HUS_POWER_STATE,
		.value = 1,	/* D0 full power */
	},
	[HIDALS_FEAT_REPORT_INTERVAL] = {
		.name = "report_interval",
		.descr = "Report interval in milliseconds",
		.usage = HUS_REPORT_INTERVAL,
		.value = 0,	/* Device default */
	},
};

struct hidals_feature_control {
	u_int			val;
	struct hid_location	loc;
	hid_size_t		rlen;
	uint8_t			rid;
};

struct hidals_softc {
	device_t		dev;
	struct sx		lock;

	/* Illuminance input report field */
	struct hid_location	ill_loc;
	uint8_t			ill_rid;
	hid_size_t		ill_rlen;

	/* Color temperature input report field (optional) */
	struct hid_location	ct_loc;
	uint8_t			ct_rid;
	bool			has_ct;

	/* Feature report controls */
	struct hidals_feature_control features[HIDALS_FEAT_COUNT];

	/* Current sensor readings */
	int			illuminance;
	int			color_temperature;
};

static const struct hid_device_id hidals_devs[] = {
	{ HID_TLC(HUP_SENSORS, HUS_AMBIENT_LIGHT) },
};

static int
hidals_set_feature(struct hidals_softc *sc, int feat_id, u_int val)
{
	struct hidals_feature_control *fc;
	uint8_t *fbuf;
	int error;
	int i;

	KASSERT(feat_id >= 0 && feat_id < HIDALS_FEAT_COUNT,
	    ("impossible feat id %d", feat_id));
	fc = &sc->features[feat_id];
	if (fc->rlen <= 1)
		return (ENXIO);

	fbuf = malloc(fc->rlen, M_TEMP, M_WAITOK | M_ZERO);
	sx_xlock(&sc->lock);

	/* Set all features sharing this report ID. */
	bzero(fbuf + 1, fc->rlen - 1);
	for (i = 0; i < nitems(sc->features); i++) {
		struct hidals_feature_control *ofc = &sc->features[i];

		if (ofc->rid != fc->rid)
			continue;
		KASSERT(fc->rlen == ofc->rlen,
		    ("different lengths for report %d: %d vs %d\n",
		    fc->rid, fc->rlen, ofc->rlen));
		hid_put_udata(fbuf + 1, ofc->rlen - 1, &ofc->loc,
		    i == feat_id ? val : ofc->val);
	}

	fbuf[0] = fc->rid;

	error = hid_set_report(sc->dev, fbuf, fc->rlen,
	    HID_FEATURE_REPORT, fc->rid);
	if (error == 0)
		fc->val = val;

	sx_unlock(&sc->lock);
	free(fbuf, M_TEMP);

	return (error);
}

static int
hidals_feature_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct hidals_softc *sc = arg1;
	int feat_id = arg2;
	struct hidals_feature_control *fc;
	u_int value;
	int error;

	if (feat_id < 0 || feat_id >= HIDALS_FEAT_COUNT)
		return (ENXIO);

	fc = &sc->features[feat_id];
	value = fc->val;
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	error = hidals_set_feature(sc, feat_id, value);
	if (error != 0) {
		DPRINTF("Failed to set %s: %d\n",
		    hidals_feature_descrs[feat_id].name, error);
	}
	return (0);
}

static int
hidals_ill_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct hidals_softc *sc = arg1;
	int val;

	val = sc->illuminance;
	return (sysctl_handle_int(oidp, &val, 0, req));
}

static int
hidals_ct_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct hidals_softc *sc = arg1;
	int val;

	val = sc->color_temperature;
	return (sysctl_handle_int(oidp, &val, 0, req));
}

static void
hidals_intr(void *context, void *buf, hid_size_t len)
{
	struct hidals_softc *sc = context;
	uint8_t *data = buf;

	if (sc->ill_rid != 0) {
		if (len < 1 || data[0] != sc->ill_rid)
			return;
		sc->illuminance = hid_get_udata(data + 1, len - 1,
		    &sc->ill_loc);
	} else {
		sc->illuminance = hid_get_udata(data, len, &sc->ill_loc);
	}

	if (sc->has_ct) {
		if (sc->ct_rid != 0)
			sc->color_temperature = hid_get_udata(data + 1,
			    len - 1, &sc->ct_loc);
		else
			sc->color_temperature = hid_get_udata(data, len,
			    &sc->ct_loc);
	}
}

static int
hidals_parse_feature(struct hidals_feature_control *fc, uint8_t tlc_index,
    uint16_t usage, void *d_ptr, hid_size_t d_len)
{
	uint32_t flags;

	if (!hidbus_locate(d_ptr, d_len, HID_USAGE2(HUP_SENSORS, usage),
	    hid_feature, tlc_index, 0, &fc->loc, &flags, &fc->rid, NULL))
		return (ENOENT);

	if ((flags & (HIO_VARIABLE | HIO_RELATIVE)) != HIO_VARIABLE)
		return (EINVAL);

	fc->rlen = hid_report_size(d_ptr, d_len, hid_feature, fc->rid);
	return (0);
}

static int
hidals_probe(device_t dev)
{
	void *d_ptr;
	hid_size_t d_len;
	uint8_t tlc_index;
	int error;

	error = HIDBUS_LOOKUP_DRIVER_INFO(dev, hidals_devs);
	if (error != 0)
		return (error);

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error != 0)
		return (ENXIO);

	tlc_index = hidbus_get_index(dev);

	/* Require illuminance input field in report descriptor. */
	if (!hidbus_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_SENSORS, HUS_ILLUMINATION),
	    hid_input, tlc_index, 0, NULL, NULL, NULL, NULL))
		return (ENXIO);

	hidbus_set_desc(dev, "ALS Sensor");

	return (BUS_PROBE_GENERIC);
}

static int
hidals_attach(device_t dev)
{
	struct hidals_softc *sc = device_get_softc(dev);
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(dev);
	void *d_ptr;
	hid_size_t d_len;
	uint8_t tlc_index;
	int error;
	int i;

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error) {
		device_printf(dev, "could not retrieve report descriptor "
		    "from device: %d\n", error);
		return (ENXIO);
	}

	sc->dev = dev;
	sx_init(&sc->lock, device_get_nameunit(dev));

	tlc_index = hidbus_get_index(dev);

	/* Locate required illuminance input field. */
	if (!hidbus_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_SENSORS, HUS_ILLUMINATION),
	    hid_input, tlc_index, 0, &sc->ill_loc, NULL, &sc->ill_rid,
	    NULL)) {
		device_printf(dev, "could not find illuminance usage\n");
		sx_destroy(&sc->lock);
		return (ENXIO);
	}
	sc->ill_rlen = hid_report_size(d_ptr, d_len, hid_input, sc->ill_rid);

	/* Locate optional color temperature input field. */
	sc->has_ct = hidbus_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_SENSORS, HUS_COLOR_TEMPERATURE),
	    hid_input, tlc_index, 0, &sc->ct_loc, NULL, &sc->ct_rid, NULL);

	/* Parse optional feature report fields. */
	for (i = 0; i < nitems(sc->features); i++) {
		(void)hidals_parse_feature(&sc->features[i], tlc_index,
		    hidals_feature_descrs[i].usage, d_ptr, d_len);
		sc->features[i].val = hidals_feature_descrs[i].value;
	}

	/* Register interrupt handler. */
	hidbus_set_intr(dev, hidals_intr, sc);

	/* Create sysctl nodes for sensor readings. */
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
	    "illuminance", CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
	    sc, 0, hidals_ill_sysctl, "I", "Illuminance in lux");

	if (sc->has_ct) {
		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "color_temperature",
		    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    sc, 0, hidals_ct_sysctl, "I",
		    "Color temperature in Kelvin");
	}

	/* Create sysctl nodes for feature report controls. */
	for (i = 0; i < nitems(sc->features); i++) {
		if (sc->features[i].rlen > 1) {
			SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
			    hidals_feature_descrs[i].name,
			    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE,
			    sc, i, hidals_feature_sysctl, "IU",
			    hidals_feature_descrs[i].descr);
		}
	}

	/* Configure initial sensor state. */
	for (i = 0; i < nitems(sc->features); i++) {
		if (sc->features[i].rlen <= 1)
			continue;
		if (sc->features[i].val == 0)
			continue;
		error = hidals_set_feature(sc, i, sc->features[i].val);
		if (error != 0) {
			DPRINTF("Failed to set initial %s: %d\n",
			    hidals_feature_descrs[i].name, error);
		}
	}

	return (0);
}

static int
hidals_detach(device_t dev)
{
	struct hidals_softc *sc = device_get_softc(dev);

	sx_destroy(&sc->lock);

	return (0);
}

static int
hidals_resume(device_t dev)
{
	struct hidals_softc *sc = device_get_softc(dev);
	int error;
	int i;

	for (i = 0; i < nitems(sc->features); i++) {
		if (sc->features[i].rlen < 2)
			continue;
		if (sc->features[i].val == hidals_feature_descrs[i].value)
			continue;
		error = hidals_set_feature(sc, i, sc->features[i].val);
		if (error != 0) {
			DPRINTF("Failed to restore %s: %d\n",
			    hidals_feature_descrs[i].name, error);
		}
	}

	return (0);
}

static device_method_t hidals_methods[] = {
	DEVMETHOD(device_probe,		hidals_probe),
	DEVMETHOD(device_attach,	hidals_attach),
	DEVMETHOD(device_detach,	hidals_detach),
	DEVMETHOD(device_resume,	hidals_resume),

	DEVMETHOD_END
};

DEFINE_CLASS_0(hidals, hidals_driver, hidals_methods,
    sizeof(struct hidals_softc));
DRIVER_MODULE(hidals, hidbus, hidals_driver, NULL, NULL);
MODULE_DEPEND(hidals, hidbus, 1, 1, 1);
MODULE_DEPEND(hidals, hid, 1, 1, 1);
MODULE_VERSION(hidals, 1);
HID_PNP_INFO(hidals_devs);

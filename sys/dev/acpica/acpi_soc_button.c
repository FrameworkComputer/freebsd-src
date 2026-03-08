/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Framework Computer Inc
 * All rights reserved.
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

/*
 * INT33D3 SoC Button Array driver.
 *
 * Reports tablet mode state via sysctl, devd notifications, and evdev
 * (SW_TABLET_MODE).  Interrupt delivery is handled by the tglgpio driver's
 * per-pin interrupt API since INTRNG is not available on x86 FreeBSD.
 */

#include <sys/cdefs.h>
#include "opt_acpi.h"
#include "opt_evdev.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/gpio/acpi_gpiobusvar.h>
#include <dev/gpio/tglgpio.h>

#ifdef EVDEV_SUPPORT
#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>
#endif

struct soc_button_softc {
	device_t	sc_dev;
	ACPI_HANDLE	sc_handle;
	device_t	sc_gpio_dev;	/* GPIO controller device */
	uint32_t	sc_pin;		/* GPIO pin number */
	uint32_t	sc_intr_mode;	/* interrupt mode flags */
	gpio_pin_t	sc_gpio_pin;	/* GPIO pin handle */
	int		sc_tablet_mode;	/* current state (-1 = unknown) */
	struct task	sc_task;	/* taskqueue task for evdev */
#ifdef EVDEV_SUPPORT
	struct evdev_dev *sc_evdev;
#endif
};

/*
 * Called from the GPIO controller's filter handler (interrupt context).
 * Enqueues a task to read the pin state and report via evdev/sysctl.
 */
static int
soc_button_filter(void *arg)
{
	struct soc_button_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->sc_task);
	return (FILTER_HANDLED);
}

/*
 * Runs in thread context — safe for GPIO reads, evdev, ACPI calls.
 */
static void
soc_button_task(void *arg, int pending __unused)
{
	struct soc_button_softc *sc = arg;
	bool active;

	if (gpio_pin_is_active(sc->sc_gpio_pin, &active) != 0)
		return;

	if ((int)active == sc->sc_tablet_mode)
		return;

	sc->sc_tablet_mode = (int)active;

	device_printf(sc->sc_dev, "tablet mode %s\n",
	    active ? "on" : "off");

	acpi_UserNotify("TabletMode", sc->sc_handle, sc->sc_tablet_mode);

#ifdef EVDEV_SUPPORT
	evdev_push_sw(sc->sc_evdev, SW_TABLET_MODE, active);
	evdev_sync(sc->sc_evdev);
#endif
}

/*
 * Walk _CRS to find GpioInt resource.
 */
struct soc_button_crs_ctx {
	ACPI_HANDLE	gpio_handle;	/* output: GPIO controller handle */
	uint32_t	pin;		/* output: pin number */
	uint32_t	flags;		/* output: converted GPIO flags */
	bool		found;
};

static ACPI_STATUS
soc_button_parse_crs(ACPI_RESOURCE *res, void *context)
{
	struct soc_button_crs_ctx *ctx = context;
	ACPI_RESOURCE_GPIO *gpio_res;
	ACPI_STATUS status;

	if (res->Type != ACPI_RESOURCE_TYPE_GPIO)
		return (AE_OK);
	gpio_res = &res->Data.Gpio;
	if (gpio_res->ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT)
		return (AE_OK);
	if (gpio_res->PinTableLength < 1)
		return (AE_OK);

	/* Resolve the ResourceSource to an ACPI handle */
	status = AcpiGetHandle(ACPI_ROOT_OBJECT,
	    gpio_res->ResourceSource.StringPtr, &ctx->gpio_handle);
	if (ACPI_FAILURE(status))
		return (AE_OK);

	ctx->pin = gpio_res->PinTable[0];
	ctx->flags = acpi_gpiobus_convflags(gpio_res);
	ctx->found = true;

	return (AE_CTRL_TERMINATE);
}

static char *soc_button_ids[] = { "INT33D3", NULL };

static int
soc_button_probe(device_t dev)
{
	int rv;

	if (acpi_disabled("soc_button"))
		return (ENXIO);
	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, soc_button_ids, NULL);
	if (rv <= 0)
		device_set_desc(dev, "SoC Button Array");
	return (rv);
}

static int
soc_button_attach(device_t dev)
{
	struct soc_button_softc *sc;
	struct soc_button_crs_ctx crs_ctx;
	device_t busdev;
	ACPI_STATUS status;
	bool active;
	int error;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_handle = acpi_get_handle(dev);
	sc->sc_tablet_mode = -1;

	/* Parse _CRS to find the GpioInt resource */
	memset(&crs_ctx, 0, sizeof(crs_ctx));
	status = AcpiWalkResources(sc->sc_handle, "_CRS",
	    soc_button_parse_crs, &crs_ctx);
	if (ACPI_FAILURE(status) || !crs_ctx.found) {
		device_printf(dev, "no GpioInt resource in _CRS\n");
		return (ENXIO);
	}

	sc->sc_pin = crs_ctx.pin;
	sc->sc_intr_mode = crs_ctx.flags & GPIO_INTR_MASK;

	/* Find the GPIO controller device from its ACPI handle */
	sc->sc_gpio_dev = acpi_get_device(crs_ctx.gpio_handle);
	if (sc->sc_gpio_dev == NULL) {
		device_printf(dev, "GPIO controller not found for %s\n",
		    acpi_name(crs_ctx.gpio_handle));
		return (ENXIO);
	}

	/* Get the gpiobus and acquire the pin */
	busdev = GPIO_GET_BUS(sc->sc_gpio_dev);
	if (busdev == NULL) {
		device_printf(dev, "no gpiobus on GPIO controller\n");
		return (ENXIO);
	}

	error = gpio_pin_get_by_bus_pinnum(busdev, sc->sc_pin,
	    &sc->sc_gpio_pin);
	if (error != 0) {
		device_printf(dev, "can't acquire GPIO pin %u: error %d\n",
		    sc->sc_pin, error);
		return (error);
	}

	/* Set pin as input */
	error = gpio_pin_setflags(sc->sc_gpio_pin, GPIO_PIN_INPUT);
	if (error != 0) {
		device_printf(dev, "can't set pin flags: error %d\n", error);
		gpio_pin_release(sc->sc_gpio_pin);
		return (error);
	}

	/* Initialize taskqueue task */
	TASK_INIT(&sc->sc_task, 0, soc_button_task, sc);

	/* Register interrupt via tglgpio's per-pin API */
	error = tglgpio_setup_intr(sc->sc_gpio_dev, sc->sc_pin,
	    sc->sc_intr_mode, soc_button_filter, sc);
	if (error != 0) {
		device_printf(dev, "can't setup interrupt: error %d\n", error);
		gpio_pin_release(sc->sc_gpio_pin);
		return (error);
	}

#ifdef EVDEV_SUPPORT
	sc->sc_evdev = evdev_alloc();
	evdev_set_name(sc->sc_evdev, device_get_desc(dev));
	evdev_set_phys(sc->sc_evdev, device_get_nameunit(dev));
	evdev_set_id(sc->sc_evdev, BUS_HOST, 0, 0, 1);
	evdev_support_event(sc->sc_evdev, EV_SYN);
	evdev_support_event(sc->sc_evdev, EV_SW);
	evdev_support_sw(sc->sc_evdev, SW_TABLET_MODE);

	error = evdev_register(sc->sc_evdev);
	if (error != 0) {
		device_printf(dev, "can't register evdev: error %d\n", error);
		tglgpio_teardown_intr(sc->sc_gpio_dev, sc->sc_pin);
		gpio_pin_release(sc->sc_gpio_pin);
		return (ENXIO);
	}
#endif

	/* Read initial state */
	if (gpio_pin_is_active(sc->sc_gpio_pin, &active) == 0) {
		sc->sc_tablet_mode = (int)active;
#ifdef EVDEV_SUPPORT
		evdev_push_sw(sc->sc_evdev, SW_TABLET_MODE, active);
		evdev_sync(sc->sc_evdev);
#endif
	}

	/* Export sysctl */
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tablet_mode", CTLFLAG_RD, &sc->sc_tablet_mode, 0,
	    "Tablet mode state (0 = laptop, 1 = tablet)");

	device_printf(dev, "attached, pin %u, initial tablet_mode=%d\n",
	    sc->sc_pin, sc->sc_tablet_mode);
	return (0);
}

static int
soc_button_resume(device_t dev)
{
	struct soc_button_softc *sc = device_get_softc(dev);

	/* Re-read GPIO state after resume */
	taskqueue_enqueue(taskqueue_thread, &sc->sc_task);
	return (0);
}

static int
soc_button_detach(device_t dev)
{
	struct soc_button_softc *sc = device_get_softc(dev);

	tglgpio_teardown_intr(sc->sc_gpio_dev, sc->sc_pin);
	taskqueue_drain(taskqueue_thread, &sc->sc_task);

#ifdef EVDEV_SUPPORT
	evdev_free(sc->sc_evdev);
#endif

	gpio_pin_release(sc->sc_gpio_pin);
	return (0);
}

static device_method_t soc_button_methods[] = {
	DEVMETHOD(device_probe,		soc_button_probe),
	DEVMETHOD(device_attach,	soc_button_attach),
	DEVMETHOD(device_detach,	soc_button_detach),
	DEVMETHOD(device_resume,	soc_button_resume),

	DEVMETHOD_END
};

static driver_t soc_button_driver = {
	"acpi_soc_button",
	soc_button_methods,
	sizeof(struct soc_button_softc),
};

DRIVER_MODULE(acpi_soc_button, acpi, soc_button_driver, NULL, NULL);
MODULE_DEPEND(acpi_soc_button, acpi, 1, 1, 1);
MODULE_DEPEND(acpi_soc_button, tglgpio, 1, 1, 1);
MODULE_DEPEND(acpi_soc_button, gpiobus, 1, 1, 1);

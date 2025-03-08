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

#include "opt_acpi.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <machine/intr.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>
#include <dev/gpio/gpiobusvar.h>

#include "button_acpi.h"

static char *gpio_ids[] = { "INT33D3", NULL };

static int
button_acpi_acpi_probe(device_t dev)
{
	int rv;

	if (acpi_disabled("gpio"))
		return (ENXIO);

	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, gpio_ids, NULL);

	if (rv <= 0)
		device_set_desc(dev, "Intel GPIO Buttons");

	return (rv);
}

static int
button_acpi_acpi_attach(device_t dev)
{
	int error;

	error = button_acpi_attach(dev);
	if (error != 0)
		return (error);

	if (!intr_pic_register(dev, ACPI_GPIO_XREF)) {
		device_printf(dev, "couldn't register GPIO Buttons\n");
		button_acpi_detach(dev);
		error = ENXIO;
	}

	return (error);
}

static device_method_t button_acpi_acpi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		button_acpi_acpi_probe),
	DEVMETHOD(device_attach,	button_acpi_acpi_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(gpio, button_acpi_acpi_driver, button_acpi_acpi_methods,
    sizeof(struct button_acpi_softc), button_acpi_driver);

EARLY_DRIVER_MODULE(button_acpi, acpi, button_acpi_acpi_driver, NULL, NULL,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);
MODULE_DEPEND(button_acpi, acpi, 1, 1, 1);
MODULE_DEPEND(button_acpi, gpiobus, 1, 1, 1);

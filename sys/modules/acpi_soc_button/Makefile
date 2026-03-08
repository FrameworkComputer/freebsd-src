.PATH:	${SRCTOP}/sys/dev/acpica
KMOD=	acpi_soc_button
SRCS=	acpi_soc_button.c
SRCS+=	acpi_if.h device_if.h bus_if.h gpio_if.h
SRCS+=	opt_acpi.h opt_evdev.h

.include <bsd.kmod.mk>

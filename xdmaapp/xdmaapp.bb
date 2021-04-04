#
# This file is the xdmaapp recipe.
#

SUMMARY = "Simple xdmaapp application"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://xdmaapp.c \
        file://demo.c \
		file://util.c \
		file://util.h \
		file://conversion.h \
	    file://libaxidma.h \
		file://axidma_ioctl.h \
	   file://Makefile \
		  "

S = "${WORKDIR}"

do_compile() {
	     oe_runmake
}

do_install() {
	     install -d ${D}${bindir}
	     install -m 0755 xdmaapp ${D}${bindir}
}

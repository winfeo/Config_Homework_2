##
# Building apk-tools

-include config.mk

PACKAGE := apk-tools
VERSION := $(shell ./get-version.sh "$(FULL_VERSION)" "$(VERSION)")

export PACKAGE VERSION

##
# Default directories

DESTDIR		:=
SBINDIR		:= /sbin
LIBDIR		:= /lib
LIBEXECDIR	:= /usr/libexec
CONFDIR		:= /etc/apk
MANDIR		:= /usr/share/man
DOCDIR		:= /usr/share/doc/apk
INCLUDEDIR	:= /usr/include
PKGCONFIGDIR	:= /usr/lib/pkgconfig

export DESTDIR SBINDIR LIBDIR LIBEXECDIR CONFDIR MANDIR DOCDIR INCLUDEDIR PKGCONFIGDIR

##
# Top-level subdirs

subdirs		:= libfetch/ src/ doc/ lua/

##
# Include all rules and stuff

include Make.rules

##
# Globals

URL_BACKEND	?= libfetch
CRYPTO		?= openssl
export URL_BACKEND CRYPTO

CFLAGS_ALL	+= -DCRYPTO_USE_$(shell echo $(CRYPTO) | tr '[:lower:]' '[:upper:]')
CFLAGS_ALL	+= -DRELATIVE_LIBEXECDIR=\"$(LIBEXECDIR:/%=%)\"

##
# Top-level targets

install:
	$(INSTALLDIR) $(DESTDIR)$(DOCDIR)
	$(INSTALL) README.md $(DESTDIR)$(DOCDIR)

check test: FORCE
	$(Q)$(MAKE) compile
	$(Q)$(MAKE) -C test

static:
	$(Q)$(MAKE) STATIC=y

tag: check
	TAG_VERSION=$$(cat VERSION); \
	git commit . -m "apk-tools-$${TAG_VERSION}"; \
	git tag -s v$${TAG_VERSION} -m "apk-tools-$${TAG_VERSION}"

src/: libfetch/

lua/: src/

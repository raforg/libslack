#
# libslack - http://libslack.org/
#
# Copyright (C) 1999-2001 raf <raf@raf.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
# or visit http://www.gnu.org/copyleft/gpl.html
#

# 20011109 raf <raf@raf.org>

CC := gcc
# CC := cc
AR := ar
RANLIB := ranlib
PREFIX := /usr/local
APP_INSDIR := $(PREFIX)/bin
LIB_INSDIR := $(PREFIX)/lib
MAN_INSDIR := $(PREFIX)/man
HDR_INSDIR := $(PREFIX)/include
DATA_INSDIR := $(PREFIX)/share
APP_MANSECT := 1
LIB_MANSECT := 3
APP_MANDIR := $(MAN_INSDIR)/man$(APP_MANSECT)
LIB_MANDIR := $(MAN_INSDIR)/man$(LIB_MANSECT)
APP_MANSECTNAME := User Commands
LIB_MANSECTNAME := C Library Functions - libslack

CCFLAGS += -O3
CCFLAGS += -Wall -pedantic

# CCFLAGS += -xO4

CLEAN_FILES += tags core Makefile.bak .makefile.bak pod2html-*

SLACK_SRCDIR := .
SLACK_INCDIRS := .
SLACK_LIBDIRS := .
include $(SLACK_SRCDIR)/macros.mk

.PHONY: all ready test check man html install uninstall dist rpm deb pkg

all: ready $(ALL_TARGETS)
ready: $(READY_TARGETS)
check test: all $(TEST_TARGETS)
man: $(MAN_TARGETS)
html: $(HTML_TARGETS)
install: all $(INSTALL_TARGETS)
uninstall: $(UNINSTALL_TARGETS)
dist: $(DIST_TARGETS)
rpm: $(RPM_TARGETS)
deb: $(DEB_TARGETS)
pkg: $(PKG_TARGETS)
obsd: $(OBSD_TARGETS)

.PHONY: help help-macros depend clean clobber distclean

help::
	@echo "This makefile provides the following targets."; \
	echo; \
	echo " help                 -- shows this list of targets"; \
	echo " help-macros          -- shows the values of all make macros"; \
	echo " all                  -- makes $(SLACK_TARGET) (default)"; \
	echo " ready                -- prepares the source directory for compilation"; \
	echo " test                 -- makes and runs library unit tests"; \
	echo " check                -- same as test"; \
	echo " man                  -- generates all manpages"; \
	echo " html                 -- generates all manpages in html"; \
	echo " install              -- installs everything under $(PREFIX)"; \
	echo " uninstall            -- uninstalls everything from $(PREFIX)"; \
	echo " depend               -- generates source dependencies using makedepend"; \
	echo " tags                 -- generates a tags file using ctags"; \
	echo " clean                -- removes object files, tags, core and Makefile.bak"; \
	echo " clobber              -- same as clean but also removes $(SLACK_TARGET) and tests"; \
	echo " distclean            -- same as clobber but also removes source dependencies"; \
	echo " dist                 -- makes the distribution: ../$(SLACK_DIST)"; \
	echo " rpm                  -- makes source and binary rpm packages"; \
	echo " deb                  -- makes binary debian package"; \
	echo " pkg                  -- makes binary solaris package"; \
	echo " obsd                 -- makes binary openbsd package"; \
	echo

help-macros::
	@echo "CC = $(CC)"; \
	echo "PREFIX = $(PREFIX)"; \
	echo "APP_INSDIR = $(APP_INSDIR)"; \
	echo "LIB_INSDIR = $(LIB_INSDIR)"; \
	echo "MAN_INSDIR = $(MAN_INSDIR)"; \
	echo "HDR_INSDIR = $(HDR_INSDIR)"; \
	echo "DATA_INSDIR = $(DATA_INSDIR)"; \
	echo "APP_MANSECT = $(APP_MANSECT)"; \
	echo "LIB_MANSECT = $(LIB_MANSECT)"; \
	echo "APP_MANDIR = $(APP_MANDIR)"; \
	echo "LIB_MANDIR = $(LIB_MANDIR)"; \
	echo "TAG_FILES = $(TAG_FILES)"; \
	echo "DEPEND_CFILES = $(DEPEND_CFILES)"; \
	echo "DEPEND_HFILES = $(DEPEND_HFILES)"; \
	echo "CCFLAGS = $(CCFLAGS)"; \
	echo "READY_TARGETS = $(READY_TARGETS)"; \
	echo "ALL_TARGETS = $(ALL_TARGETS)"; \
	echo "TEST_TARGETS = $(TEST_TARGETS)"; \
	echo "MAN_TARGETS = $(MAN_TARGETS)"; \
	echo "HTML_TARGETS = $(HTML_TARGETS)"; \
	echo "INSTALL_TARGETS = $(INSTALL_TARGETS)"; \
	echo "UNINSTALL_TARGETS = $(UNINSTALL_TARGETS)"; \
	echo "CLEAN_FILES = $(CLEAN_FILES)"; \
	echo "CLOBBER_FILES = $(CLOBBER_FILES)"; \
	echo "DIST_TARGETS = $(DIST_TARGETS)"; \
	echo "RPM_TARGETS = $(RPM_TARGETS)"; \
	echo "DEB_TARGETS = $(DEB_TARGETS)"; \
	echo "PKG_TARGETS = $(PKG_TARGETS)"; \
	echo "BSD_TARGETS = $(BSD_TARGETS)"; \
	echo

tags: $(TAG_FILES)
	@ctags $(TAG_FILES)

depend: ready $(DEPEND_CFILES) $(DEPEND_HFILES)
	@makedepend $(SLACK_CPPFLAGS) $(DEPEND_CFILES)

# makedepend $(SLACK_CPPFLAGS) $(DEPEND_CFILES)

clean::
	@rm -rf $(CLEAN_FILES)

clobber::
	@rm -rf $(CLEAN_FILES) $(CLOBBER_FILES)

distclean:: clobber
	@perl -pi -e 'last if /[D]O NOT DELETE/;' $(patsubst %, %/Makefile, $(SLACK_SRCDIR) $(SLACK_SUBDIRS))

include $(SLACK_SRCDIR)/rules.mk


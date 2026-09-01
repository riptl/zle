# ################################################################
# libzle build variables
#
# Derived from libzstd.mk (BSD-3-Clause / GPLv2, Meta Platforms Inc.),
# minimized for a Linux-only, single-threaded, single-source library.
#
# This included Makefile provides the following variables :
#   LIB_SRCDIR, LIB_BINDIR, LIBVER*, ZLE_VERSION, ZLE_FILES,
#   CFLAGS, CPPFLAGS, ASFLAGS, LDFLAGS, FLAGS, HASH_DIR, LN, CP
# ################################################################

# Ensure the file is not included twice
# Note : must be included after setting the default target
ifndef LIBZLE_MK_INCLUDED
LIBZLE_MK_INCLUDED := 1

##################################################################
# Input Variables
##################################################################

# By default, library's directory is same as this included makefile
LIB_SRCDIR ?= $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
LIB_BINDIR ?= $(LIB_SRCDIR)

# Assembly support (no .S files today, kept for future SIMD kernels)
ZLE_NO_ASM ?= 0

##################################################################
# Helpers
##################################################################

VOID ?= /dev/null

# define silent mode as default (verbose mode with V=1 or VERBOSE=1)
# Note : must be defined _after_ the default target
$(V)$(VERBOSE).SILENT:

# Version numbers, extracted from zle.h
LIBVER_SRC := $(LIB_SRCDIR)/zle.h
LIBVER_MAJOR_SCRIPT:=`sed -n '/define ZLE_VERSION_MAJOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < $(LIBVER_SRC)`
LIBVER_MINOR_SCRIPT:=`sed -n '/define ZLE_VERSION_MINOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < $(LIBVER_SRC)`
LIBVER_PATCH_SCRIPT:=`sed -n '/define ZLE_VERSION_RELEASE/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < $(LIBVER_SRC)`
LIBVER_SCRIPT:= $(LIBVER_MAJOR_SCRIPT).$(LIBVER_MINOR_SCRIPT).$(LIBVER_PATCH_SCRIPT)
LIBVER_MAJOR := $(shell echo $(LIBVER_MAJOR_SCRIPT))
LIBVER_MINOR := $(shell echo $(LIBVER_MINOR_SCRIPT))
LIBVER_PATCH := $(shell echo $(LIBVER_PATCH_SCRIPT))
LIBVER       := $(shell echo $(LIBVER_SCRIPT))
ZLE_VERSION  ?= $(LIBVER)

CFLAGS ?= -O3

DEBUGLEVEL ?= 0
CPPFLAGS += -DDEBUGLEVEL=$(DEBUGLEVEL)
DEBUGFLAGS= -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow \
            -Wstrict-aliasing=1 -Wswitch-enum -Wdeclaration-after-statement \
            -Wstrict-prototypes -Wundef -Wpointer-arith \
            -Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings \
            -Wredundant-decls -Wmissing-prototypes -Wc++-compat
CFLAGS   += $(DEBUGFLAGS) $(MOREFLAGS)
ASFLAGS  += $(DEBUGFLAGS) $(MOREFLAGS) $(CFLAGS)
LDFLAGS  += $(MOREFLAGS)
FLAGS     = $(CPPFLAGS) $(CFLAGS) $(ASFLAGS) $(LDFLAGS)

# Mark the stack non-executable, when the toolchain supports it
ifndef ALREADY_APPENDED_NOEXECSTACK
export ALREADY_APPENDED_NOEXECSTACK := 1
ifeq ($(shell echo "int main(void) { return 0; }" | $(CC) $(FLAGS) -z noexecstack -x c -Werror - -o $(VOID) 2>$(VOID) && echo 1 || echo 0),1)
LDFLAGS += -z noexecstack
endif
ifeq ($(shell echo | $(CC) $(FLAGS) -Wa,--noexecstack -x assembler -Werror -c - -o $(VOID) 2>$(VOID) && echo 1 || echo 0),1)
# CFLAGS are also added to ASFLAGS
CFLAGS  += -Wa,--noexecstack
else ifeq ($(shell echo | $(CC) $(FLAGS) -Qunused-arguments -Wa,--noexecstack -x assembler -Werror -c - -o $(VOID) 2>$(VOID) && echo 1 || echo 0),1)
CFLAGS  += -Qunused-arguments -Wa,--noexecstack
endif
endif

ifeq ($(shell echo "int main(void) { return 0; }" | $(CC) $(FLAGS) -z cet-report=error -x c -Werror - -o $(VOID) 2>$(VOID) && echo 1 || echo 0),1)
LDFLAGS += -z cet-report=error
endif

##################################################################
# Source files
##################################################################

ZLE_FILES := $(sort $(wildcard $(LIB_SRCDIR)/*.c))

ifneq ($(ZLE_NO_ASM), 0)
  CPPFLAGS += -DZLE_DISABLE_ASM
else
  # ASM files self-disable via macros, so add them unconditionally
  ZLE_FILES += $(sort $(wildcard $(LIB_SRCDIR)/*.S))
endif

LN ?= ln
CP ?= cp -f
MKDIR ?= mkdir -p

# Build directory is keyed on the compilation flags, so that builds with
# different flags do not clobber each other's objects.
ifndef BUILD_DIR
HASH ?= md5sum
HASH_DIR = conf_$(shell echo $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(ZLE_FILES) | $(HASH) | cut -f 1 -d " " )
HAVE_HASH := $(shell echo 1 | $(HASH) > $(VOID) 2> $(VOID) && echo 1 || echo 0)
ifeq ($(HAVE_HASH),0)
  $(info warning : could not find HASH ($(HASH)), needed to differentiate builds using different flags)
  BUILD_DIR := obj/generic_noconf
endif
endif # BUILD_DIR

vpath %.c $(LIB_SRCDIR)
vpath %.S $(LIB_SRCDIR)

endif # LIBZLE_MK_INCLUDED

# ################################################################
# libzle library Makefile
#
# Derived from lib/Makefile of zstd (BSD-3-Clause / GPLv2, Meta
# Platforms Inc.), minimized: Linux/POSIX only, no Windows, no macOS,
# no multithreading, no legacy formats, no dictionary builder.
# ################################################################

# default target (when running `make` with no argument)
lib-release:

include libzle.mk

ZLE_LOCAL_SRC  := $(notdir $(ZLE_FILES))
ZLE_LOCAL_OBJ0 := $(ZLE_LOCAL_SRC:.c=.o)
ZLE_LOCAL_OBJ  := $(ZLE_LOCAL_OBJ0:.S=.o)

VERSION := $(ZLE_VERSION)

SHARED_EXT       = so
SHARED_EXT_MAJOR = $(SHARED_EXT).$(LIBVER_MAJOR)
SHARED_EXT_VER   = $(SHARED_EXT).$(LIBVER)
SONAME_FLAGS     = -Wl,-soname=libzle.$(SHARED_EXT).$(LIBVER_MAJOR)

.PHONY: all
all: lib

# Re-invoke ourselves with BUILD_DIR derived from the compilation flags
SET_CACHE_DIRECTORY = \
   +$(MAKE) --no-print-directory $@ \
    BUILD_DIR=obj/$(HASH_DIR) \
    CPPFLAGS="$(CPPFLAGS)" \
    CFLAGS="$(CFLAGS)" \
    LDFLAGS="$(LDFLAGS)"

#-----------------------------------------------------------------------------
# static library
#-----------------------------------------------------------------------------

.PHONY: libzle.a  # must be run every time

ifndef BUILD_DIR

libzle.a:
	$(SET_CACHE_DIRECTORY)

else # BUILD_DIR is defined

ZLE_STATICLIB_DIR := $(BUILD_DIR)/static
ZLE_STATICLIB     := $(ZLE_STATICLIB_DIR)/libzle.a
ZLE_STATICLIB_OBJ := $(addprefix $(ZLE_STATICLIB_DIR)/,$(ZLE_LOCAL_OBJ))
$(ZLE_STATICLIB): ARFLAGS = rcs
$(ZLE_STATICLIB): | $(ZLE_STATICLIB_DIR)
$(ZLE_STATICLIB): $(ZLE_STATICLIB_OBJ)
	@echo compiling static library $(LIBVER)
	$(AR) $(ARFLAGS) $@ $^

libzle.a: $(ZLE_STATICLIB)
	$(CP) $< $@

endif

#-----------------------------------------------------------------------------
# shared library
#-----------------------------------------------------------------------------

LIBZLE = libzle.$(SHARED_EXT_VER)
.PHONY: $(LIBZLE)  # must be run every time
$(LIBZLE): CFLAGS  += -fPIC -fvisibility=hidden
$(LIBZLE): LDFLAGS += -shared

ifndef BUILD_DIR

$(LIBZLE):
	$(SET_CACHE_DIRECTORY)

else # BUILD_DIR is defined

ZLE_DYNLIB_DIR := $(BUILD_DIR)/dynamic
ZLE_DYNLIB     := $(ZLE_DYNLIB_DIR)/$(LIBZLE)
ZLE_DYNLIB_OBJ := $(addprefix $(ZLE_DYNLIB_DIR)/,$(ZLE_LOCAL_OBJ))

$(ZLE_DYNLIB): | $(ZLE_DYNLIB_DIR)
$(ZLE_DYNLIB): $(ZLE_DYNLIB_OBJ)
	@echo compiling dynamic library $(LIBVER)
	$(CC) $(FLAGS) $^ $(SONAME_FLAGS) -o $@

$(LIBZLE): $(ZLE_DYNLIB)
	$(CP) $< $@
	@echo creating versioned links
	$(LN) -sf $(LIBZLE) libzle.$(SHARED_EXT_MAJOR)
	$(LN) -sf $(LIBZLE) libzle.$(SHARED_EXT)

endif # ifndef BUILD_DIR

.PHONY: libzle
libzle: $(LIBZLE)

.PHONY: lib
lib: libzle.a libzle

# note : do not define lib-release as .PHONY
# make does not consider implicit pattern rules for .PHONY targets
%-release : DEBUGFLAGS :=
%-release : %
	@echo release build completed

#-----------------------------------------------------------------------------
# object files
#-----------------------------------------------------------------------------

# -MMD: generate .h dependencies as a side-effect of compilation, skipping
#       system headers
# -MP : add a phony target for each dependency other than the main file
DEPFLAGS = -MMD -MP

# the order-only prerequisite ensures the output directory exists
$(ZLE_DYNLIB_DIR)/%.o : %.c | $(ZLE_DYNLIB_DIR)
	@echo CC $@
	$(COMPILE.c) $(DEPFLAGS) $(OUTPUT_OPTION) $<

$(ZLE_STATICLIB_DIR)/%.o : %.c | $(ZLE_STATICLIB_DIR)
	@echo CC $@
	$(COMPILE.c) $(DEPFLAGS) $(OUTPUT_OPTION) $<

$(ZLE_DYNLIB_DIR)/%.o : %.S | $(ZLE_DYNLIB_DIR)
	@echo AS $@
	$(COMPILE.S) $(OUTPUT_OPTION) $<

$(ZLE_STATICLIB_DIR)/%.o : %.S | $(ZLE_STATICLIB_DIR)
	@echo AS $@
	$(COMPILE.S) $(OUTPUT_OPTION) $<

$(BUILD_DIR) $(ZLE_DYNLIB_DIR) $(ZLE_STATICLIB_DIR):
	$(MKDIR) $@

DEPFILES := $(ZLE_DYNLIB_OBJ:.o=.d) $(ZLE_STATICLIB_OBJ:.o=.d)
$(DEPFILES):

# The leading '-' means: do not fail if the include fails (ex: no build yet)
-include $(wildcard $(DEPFILES))

.PHONY: clean
clean:
	$(RM) core *.o *.a *.gcda *.$(SHARED_EXT) *.$(SHARED_EXT).* libzle.pc
	$(RM) -r obj/*
	@echo Cleaning library completed

#-----------------------------------------------------------------------------
# install
#-----------------------------------------------------------------------------

lib: libzle.pc

HAS_EXPLICIT_EXEC_PREFIX := $(if $(or $(EXEC_PREFIX),$(exec_prefix)),1,)

DESTDIR     ?=
# directory variables : GNU conventions prefer lowercase
# see https://www.gnu.org/prep/standards/html_node/Makefile-Conventions.html
# support both lower and uppercase (BSD), use uppercase in script
prefix      ?= /usr/local
PREFIX      ?= $(prefix)
exec_prefix ?= $(PREFIX)
EXEC_PREFIX ?= $(exec_prefix)
libdir      ?= $(EXEC_PREFIX)/lib
LIBDIR      ?= $(libdir)
includedir  ?= $(PREFIX)/include
INCLUDEDIR  ?= $(includedir)

PCINCDIR := $(patsubst $(PREFIX)%,%,$(INCLUDEDIR))
PCLIBDIR := $(patsubst $(EXEC_PREFIX)%,%,$(LIBDIR))

# If we successfully stripped off a prefix, we'll add a reference to the
# relevant pc variable.
PCINCPREFIX := $(if $(findstring $(INCLUDEDIR),$(PCINCDIR)),,$${prefix})
PCLIBPREFIX := $(if $(findstring $(LIBDIR),$(PCLIBDIR)),,$${exec_prefix})

# If no explicit EXEC_PREFIX was set by the caller, write it out as a reference
# to PREFIX, rather than as a resolved value.
PCEXEC_PREFIX := $(if $(HAS_EXPLICIT_EXEC_PREFIX),$(EXEC_PREFIX),$${prefix})

PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

INSTALL         ?= install
INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_DATA    ?= $(INSTALL) -m 644

.PHONY: libzle.pc
libzle.pc: libzle.pc.in
	@echo creating pkgconfig
	@sed \
	        -e 's|@PREFIX@|$(PREFIX)|' \
	        -e 's|@EXEC_PREFIX@|$(PCEXEC_PREFIX)|' \
	        -e 's|@INCLUDEDIR@|$(PCINCPREFIX)$(PCINCDIR)|' \
	        -e 's|@LIBDIR@|$(PCLIBPREFIX)$(PCLIBDIR)|' \
	        -e 's|@VERSION@|$(VERSION)|' \
	        $< >$@

.PHONY: install
install: install-pc install-static install-shared install-includes
	@echo zle static and shared library installed

.PHONY: install-pc
install-pc: libzle.pc
	[ -e $(DESTDIR)$(PKGCONFIGDIR) ] || $(INSTALL) -d -m 755 $(DESTDIR)$(PKGCONFIGDIR)/
	$(INSTALL_DATA) libzle.pc $(DESTDIR)$(PKGCONFIGDIR)/

.PHONY: install-static
install-static:
	# only generate libzle.a if it's not already present
	[ -e libzle.a ] || $(MAKE) libzle.a-release
	[ -e $(DESTDIR)$(LIBDIR) ] || $(INSTALL) -d -m 755 $(DESTDIR)$(LIBDIR)/
	@echo Installing static library
	$(INSTALL_DATA) libzle.a $(DESTDIR)$(LIBDIR)

.PHONY: install-shared
install-shared:
	# only generate libzle.so if it's not already present
	[ -e $(LIBZLE) ] || $(MAKE) libzle-release
	[ -e $(DESTDIR)$(LIBDIR) ] || $(INSTALL) -d -m 755 $(DESTDIR)$(LIBDIR)/
	@echo Installing shared library
	$(INSTALL_PROGRAM) $(LIBZLE) $(DESTDIR)$(LIBDIR)
	$(LN) -sf $(LIBZLE) $(DESTDIR)$(LIBDIR)/libzle.$(SHARED_EXT_MAJOR)
	$(LN) -sf $(LIBZLE) $(DESTDIR)$(LIBDIR)/libzle.$(SHARED_EXT)

.PHONY: install-includes
install-includes:
	[ -e $(DESTDIR)$(INCLUDEDIR) ] || $(INSTALL) -d -m 755 $(DESTDIR)$(INCLUDEDIR)/
	@echo Installing includes
	$(INSTALL_DATA) zle.h $(DESTDIR)$(INCLUDEDIR)

.PHONY: uninstall
uninstall:
	$(RM) $(DESTDIR)$(LIBDIR)/libzle.a
	$(RM) $(DESTDIR)$(LIBDIR)/libzle.$(SHARED_EXT)
	$(RM) $(DESTDIR)$(LIBDIR)/libzle.$(SHARED_EXT_MAJOR)
	$(RM) $(DESTDIR)$(LIBDIR)/$(LIBZLE)
	$(RM) $(DESTDIR)$(PKGCONFIGDIR)/libzle.pc
	$(RM) $(DESTDIR)$(INCLUDEDIR)/zle.h
	@echo zle libraries successfully uninstalled

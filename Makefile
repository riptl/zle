# ################################################################
# zle top-level Makefile
#
# Derived from zstd's top-level Makefile (BSD-3-Clause / GPLv2, Meta
# Platforms Inc.), minimized to Linux/POSIX, library-only.
# ################################################################

# verbose mode (print commands) on V=1 or VERBOSE=1
Q = $(if $(filter 1,$(V) $(VERBOSE)),,@)

ZLEDIR = lib
VOID   = /dev/null

## default: build the library in release mode
.PHONY: default
default: lib-release

## all: alias for `lib`
.PHONY: all
all: lib

## lib: build static + shared libzle (with debug warnings enabled)
## lib-release: build static + shared libzle without debug warnings
.PHONY: lib lib-release
lib lib-release:
	$(Q)$(MAKE) -C $(ZLEDIR) $@

## clean: remove all build artefacts
.PHONY: clean
clean:
	$(Q)$(MAKE) -C $(ZLEDIR) $@ > $(VOID)
	$(Q)$(RM) tmp*
	@echo Cleaning completed

## install: install libzle and its headers under $(PREFIX) (default /usr/local)
.PHONY: install
install:
	$(Q)$(MAKE) -C $(ZLEDIR) $@

## uninstall: remove what `make install` installed
.PHONY: uninstall
uninstall:
	$(Q)$(MAKE) -C $(ZLEDIR) $@

# Print a two column output of targets and their description. To add a target
# description, put a comment in the Makefile with the format
# "## <TARGET>: <DESCRIPTION>".
#
## list: Print all targets and their descriptions (if provided)
.PHONY: list
list:
	$(Q)TARGETS=$$($(MAKE) -pRrq -f $(lastword $(MAKEFILE_LIST)) : 2>$(VOID) \
		| awk -v RS= -F: '/^# File/,/^# Finished Make data base/ {if ($$1 !~ "^[#.]") {print $$1}}' \
		| grep -v -e '^[^[:alnum:]]' | sort); \
	{ \
	    printf 'Target Name\tDescription\n'; \
	    printf -- '----------------\t----------------------------------------\n'; \
	    for target in $$TARGETS; do \
	        line=$$(grep -E "^##[[:space:]]+$$target:" $(lastword $(MAKEFILE_LIST))); \
	        description=$$(echo $$line | awk '{i=index($$0,":"); print substr($$0,i+1)}' | xargs); \
	        printf '%s\t%s\n' "$$target" "$$description"; \
	    done \
	} | column -t -s "$$(printf '\t')"

@SET_MAKE@

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

EXTRA_DIST =								\
	example.gtkrc-2.0						\
	COMPOSITOR							\
	README.md							\
	tests/check-animation-math					\
	tests/test-animation-math.c					\
	tests/check-compositor-regressions				\
	tests/check-compositor-regressions.py				\
	tests/check-compositor-damage					\
	tests/test-compositor-damage.c					\
	tests/cursor-probe.c

EXTRA_DIST += tests/check-peek tests/check-peek.py \
	tests/check-window-lifecycle tests/check-window-lifecycle.py \
	tests/check-minimize tests/check-minimize.py tests/animation-probe.c

check-local:
	CC="$(CC)" $(SHELL) $(srcdir)/tests/check-animation-math
	CC="$(CC)" $(SHELL) $(srcdir)/tests/check-compositor-damage
	@set -e; out=$$(mktemp -d); trap 'rm -rf "$$out"' EXIT HUP INT TERM; \
	$(SHELL) $(srcdir)/tests/check-window-lifecycle "$(abs_builddir)/src/xfwm4-check" xrender "$$out/lifecycle"; \
	$(SHELL) $(srcdir)/tests/check-peek "$(abs_builddir)/src/xfwm4-check" xrender "$$out/peek"; \
	$(SHELL) $(srcdir)/tests/check-minimize "$(abs_builddir)/src/xfwm4-check" xrender "$$out/minimize"

SUBDIRS = 								\
	defaults 							\
	helper-dialog 							\
	icons 								\
	common 								\
	settings-dialogs						\
	src 								\
	themes								\
	po

distclean-local:
	rm -rf *.cache po/.intltool-merge-cache.lock

.PHONY: ChangeLog

ChangeLog: Makefile
	(GIT_DIR=$(top_srcdir)/.git git log > .changelog.tmp \
	&& mv .changelog.tmp ChangeLog; rm -f .changelog.tmp) \
	|| (touch ChangeLog; echo 'Git directory not found: installing possibly empty changelog.' >&2)

dist-hook: ChangeLog

if MAINTAINER_MODE

all-local: compile_flags.txt

compile_flags.txt: Makefile
	rm -f $@
	for i in \
	$(PLATFORM_CPPFLAGS) \
	$(GLIB_CFLAGS)  \
	$(GTK_CFLAGS)  \
	$(COMPOSITOR_CFLAGS) \
	$(EPOXY_CFLAGS) \
	$(LIBSTARTUP_NOTIFICATION_CFLAGS) \
	$(LIBX11_CFLAGS) \
	$(LIBXFCE4KBD_PRIVATE_CFLAGS) \
	$(LIBXFCE4UI_CFLAGS) \
	$(LIBXFCE4UTIL_CFLAGS) \
	$(LIBXFCONF_CFLAGS) \
	$(PRESENT_EXTENSION_CFLAGS) \
	$(RANDR_CFLAGS) \
	$(RENDER_CFLAGS) \
	$(XINERAMA_CFLAGS) \
	-I${top_srcdir} \
	-DHAVE_CONFIG_H=1 \
	-DPACKAGE_LOCALE_DIR=\"$(localedir)\" \
	-DDATADIR=\"$(datadir)\" \
	-DHELPERDIR=\"$(HELPER_PATH_PREFIX)\" \
	-DPACKAGE_DATADIR=\"$(pkgdatadir)\" \
	-DG_LOG_DOMAIN=\"xfwm4\"; \
	do \
		echo "$$i" >> $@ || exit 1; \
	done

endif

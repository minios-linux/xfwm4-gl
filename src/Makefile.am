bin_PROGRAMS = xfwm4

xfwm4_SOURCES =								\
	animation-math.c						\
	animation-math.h						\
	client.c							\
	client.h							\
	compositor.c							\
	compositor-effects.c						\
	compositor-effects.h						\
	compositor-xrender-effects.h					\
	compositor-xrender-effects.c					\
	compositor-damage.h						\
	compositor.h							\
	compositor-gl.c							\
	compositor-gl.h							\
	compositor-priv.h						\
	cycle.c								\
	cycle.h								\
	device.c							\
	device.h							\
	display.c							\
	display.h							\
	event_filter.c							\
	event_filter.h							\
	events.c							\
	events.h							\
	focus.c								\
	focus.h								\
	frame.c								\
	frame.h								\
	hints.c								\
	hints.h								\
	icons.c								\
	xfwm-tabwin-preview.h						\
	xfwm-tabwin-style.h						\
	icons.h								\
	keyboard.c							\
	keyboard.h							\
	main.c								\
	menu.c								\
	menu.h								\
	misc.c								\
	misc.h								\
	moveresize.c							\
	moveresize.h							\
	mypixmap.c							\
	mypixmap.h							\
	mywindow.c							\
	mywindow.h							\
	netwm.c								\
	netwm.h								\
	parserc.c							\
	parserc.h							\
	placement.c							\
	placement.h							\
	poswin.c							\
	poswin.h							\
	screen.c							\
	screen.h							\
	session.c							\
	session.h							\
	settings.c							\
	settings.h							\
	spinning_cursor.c						\
	spinning_cursor.h						\
	stacking.c							\
	stacking.h							\
	startup_notification.c						\
	startup_notification.h						\
	tabwin.c							\
	tabwin.h							\
	terminate.c							\
	terminate.h							\
	transients.c							\
	transients.h							\
	ui_style.c							\
	ui_style.h							\
	wireframe.c							\
	wireframe.h							\
	workspaces.c							\
	workspaces.h							\
	xsync.c								\
	xsync.h								\
	xpm-color-table.h

xfwm4_CFLAGS =								\
	$(GLIB_CFLAGS) 							\
	$(GTK_CFLAGS) 							\
	$(COMPOSITOR_CFLAGS)						\
	$(EPOXY_CFLAGS)							\
	$(LIBSTARTUP_NOTIFICATION_CFLAGS)				\
	$(LIBX11_CFLAGS)						\
	$(LIBXFCE4KBD_PRIVATE_CFLAGS)					\
	$(LIBXFCE4UI_CFLAGS)						\
	$(LIBXFCE4UTIL_CFLAGS)						\
	$(LIBXFCONF_CFLAGS)						\
	$(PRESENT_EXTENSION_CFLAGS)					\
	$(RANDR_CFLAGS)							\
	$(RENDER_CFLAGS)						\
	$(XINERAMA_CFLAGS)						\
	-DPACKAGE_LOCALE_DIR=\"$(localedir)\"				\
	-DDATADIR=\"$(datadir)\"					\
	-DHELPERDIR=\"$(HELPER_PATH_PREFIX)\"				\
	-DPACKAGE_DATADIR=\"$(pkgdatadir)\"				\
	-DG_LOG_DOMAIN=\"xfwm4\"

xfwm4_LDADD =								\
	$(top_builddir)/common/libxfwm-common.la 			\
	$(GLIB_LIBS) 							\
	$(GTK_LIBS) 							\
	$(COMPOSITOR_LIBS)						\
	$(EPOXY_LIBS)							\
	$(LIBSTARTUP_NOTIFICATION_LIBS)					\
	$(LIBX11_LDFLAGS)						\
	$(LIBX11_LIBS)							\
	$(LIBXFCE4KBD_PRIVATE_LIBS)					\
	$(LIBXFCE4UI_LIBS)						\
	$(LIBXFCE4UTIL_LIBS)						\
	$(LIBXFCONF_LIBS)						\
	$(PRESENT_EXTENSION_LIBS)					\
	$(RANDR_LIBS) 							\
	$(RENDER_LIBS)							\
	$(XINERAMA_LIBS)						\
	$(MATH_LIBS)

AM_CPPFLAGS = 								\
	-I${top_srcdir} 						\
	$(PLATFORM_CPPFLAGS)

EXTRA_DIST = 								\
	gen-color-table.pl

# Reuse production objects; only the defaults path differs for isolated checks
# on builders where the package's data files have not been installed yet.
check_PROGRAMS = xfwm4-check
xfwm4_check_SOURCES = settings.c
xfwm4_check_CFLAGS = $(xfwm4_CFLAGS) -UPACKAGE_DATADIR \
	-DPACKAGE_DATADIR=\"$(abs_top_srcdir)/defaults\"
xfwm4_check_LDADD = $(filter-out xfwm4-settings.o,$(xfwm4_OBJECTS)) $(xfwm4_LDADD)
xfwm4_check_DEPENDENCIES = $(filter-out xfwm4-settings.o,$(xfwm4_OBJECTS)) \
	$(top_builddir)/common/libxfwm-common.la

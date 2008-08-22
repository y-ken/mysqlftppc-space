# Copyright (C) 2008 Hiroaki Kawai

pkgplugindir=@MYSQL_PLUGINDIR@
INCLUDES= -I$(top_builddir)/include -I$(top_srcdir)/include @MYSQL_INCLUDES@
#noinst_LTLIBRARIES= mypluglib.la
pkgplugin_LTLIBRARIES= libftspace.la
libftspace_la_SOURCES= ftbool.c plugin_space.c
libftspace_la_LDFLAGS= -module -rpath $(pkgplugindir)
libftspace_la_CFLAGS= -DMYSQL_DYNAMIC_PLUGIN

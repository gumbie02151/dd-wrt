ipsec_PROGRAMS = stroke

stroke_SOURCES = \
stroke.c stroke_msg.h stroke_keywords.c stroke_keywords.h

stroke_LDADD = $(top_builddir)/src/libstrongswan/libstrongswan.la $(SOCKLIB)
INCLUDES = -I$(top_srcdir)/src/libstrongswan
EXTRA_DIST = stroke_keywords.txt Android.mk
BUILT_SOURCES = stroke_keywords.c
MAINTAINERCLEANFILES = stroke_keywords.c
AM_CFLAGS = -DIPSEC_PIDDIR=\"${piddir}\"

stroke_keywords.c:	$(srcdir)/stroke_keywords.txt $(srcdir)/stroke_keywords.h
		$(GPERF) -m 10 -D -C -G -t < $(srcdir)/stroke_keywords.txt > $@

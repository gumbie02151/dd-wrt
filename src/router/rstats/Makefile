include $(TOP)/.config

CFLAGS	+= -I. -I$(TOP)/libutils -I$(SRCBASE)/include.v24 -Wall -I$(SRCBASE)/
CFLAGS	+= $(COPTS) -fpic
LDFLAGS += -L../nvram -L../shared -lnvram -lutils

OBJS := rstats.o base64.o files.o misc.o process.o

all: rstats

clean:
	rm -f *.o rstats

install: all
	install -d $(INSTALLDIR)/usr/sbin
	install -D rstats $(INSTALLDIR)/usr/sbin/rstats
	install -D config/rstats.webservices $(INSTALLDIR)/etc/config/rstats.webservices
	install -D config/rstats.nvramconfig $(INSTALLDIR)/etc/config/rstats.nvramconfig

rstats: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

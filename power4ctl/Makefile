CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
TARGET   = power4ctl
VERSION  = 1.0.0
OBJS     = power4ctl.o sha1.o

BINDIR      = /usr/local/bin
MANDIR      = /usr/local/share/man/man1
SYSTEMDDIR  = /usr/lib/systemd/system
DEFAULTSDIR = /etc/default

DEB_BINDIR  = /usr/bin
DEB_MANDIR  = /usr/share/man/man1

ARCH    := $(shell dpkg --print-architecture 2>/dev/null || \
               uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
PKGROOT  = pkgroot
PKGFILE  = $(TARGET)_$(VERSION)_$(ARCH).deb

.PHONY: all install uninstall deb clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

power4ctl.o: power4ctl.c sha1.h
	$(CC) $(CFLAGS) -c -o $@ power4ctl.c

sha1.o: sha1.c sha1.h
	$(CC) $(CFLAGS) -c -o $@ sha1.c

install: $(TARGET)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(MANDIR)
	gzip -c $(TARGET).1 > $(MANDIR)/$(TARGET).1.gz

uninstall:
	rm -f $(BINDIR)/$(TARGET) $(MANDIR)/$(TARGET).1.gz

deb: $(TARGET)
	rm -rf $(PKGROOT)
	install -d $(PKGROOT)/DEBIAN \
	           $(PKGROOT)/$(DEB_BINDIR) \
	           $(PKGROOT)/$(DEB_MANDIR) \
	           $(PKGROOT)/$(SYSTEMDDIR) \
	           $(PKGROOT)/$(DEFAULTSDIR)
	install -m 755 $(TARGET) $(PKGROOT)/$(DEB_BINDIR)/$(TARGET)
	gzip -c $(TARGET).1 > $(PKGROOT)/$(DEB_MANDIR)/$(TARGET).1.gz
	install -m 644 $(TARGET).service $(PKGROOT)/$(SYSTEMDDIR)/$(TARGET).service
	install -m 644 $(TARGET).default $(PKGROOT)/$(DEFAULTSDIR)/$(TARGET)
	sed -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@ARCH@/$(ARCH)/g' \
	    debian/control.in > $(PKGROOT)/DEBIAN/control
	install -m 755 debian/postinst $(PKGROOT)/DEBIAN/postinst
	install -m 755 debian/prerm    $(PKGROOT)/DEBIAN/prerm
	cp debian/conffiles            $(PKGROOT)/DEBIAN/conffiles
	dpkg-deb --build --root-owner-group $(PKGROOT) $(PKGFILE)
	rm -rf $(PKGROOT)

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf $(PKGROOT) $(TARGET)_*.deb

DOXYGEN ?= doxygen
INSTALL ?= install
SHELL = /bin/sh

outdir = .
prefix = /usr/local
exec_prefix = $(prefix)
includedir = $(prefix)/include
libdir = $(exec_prefix)/lib

CFLAGS := -O2 -g -Wall -Wextra -fPIC $(CFLAGS)

modules = byztime_consumer byztime_provider byztime_common byztime_stamp
sources = $(addsuffix .c, $(modules))
objects = $(addprefix $(outdir)/, $(addsuffix .o, $(modules)))
private_headers = byztime_internal.h
public_headers = byztime.h

all: $(outdir)/libbyztime.a

fmt: $(sources) $(private_headers) $(public_headers)
	clang-format -style=file -i $^

$(outdir)/libbyztime.a: $(objects)
	$(AR) -rc $@ $^

$(objects): $(outdir)/%.o: %.c $(private_headers) $(public_headers)
	$(CC) -std=c11 -o $@ -c $(CFLAGS) $(CPPFLAGS) $<

doc: html

installdirs:
	mkdir -p $(DESTDIR)$(includedir)
	mkdir -p $(DESTDIR)$(libdir)

install: installdirs $(outdir)/libbyztime.a $(public_headers)
	$(INSTALL) -t $(DESTDIR)$(libdir) $(outdir)/libbyztime.a
	$(INSTALL) -m 0644 -t $(DESTDIR)$(includedir) $(public_headers)

uninstall:
	$(RM) $(DESTDIR)$(libdir)/libbyztime.a
	$(RM) $(DESTDIR)$(includedir)/{$(public_headers)}

mostlyclean:
	$(RM) $(objects) $(outdir)/libbyztime.a
	$(RM) -r $(outdir)/doc

clean: mostlyclean
distclean: clean
maintainer-clean: distclean

html: $(sources)
	OUTDIR=$(outdir) $(DOXYGEN)
info:
dvi:
ps:
pdf:
check:
installcheck:

.PHONY: all fmt doc installdirs install uninstall mostlyclean clean distclean maintainer-clean html pdf info dvi ps check installcheck

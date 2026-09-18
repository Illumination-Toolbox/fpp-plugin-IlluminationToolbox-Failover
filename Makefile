SRCDIR ?= /opt/fpp/src
include ${SRCDIR}/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-failover.$(SHLIB_EXT)
debug: all

OBJECTS_fpp_failover_so += src/FPPFailover.o
LIBS_fpp_failover_so += -L${SRCDIR} -lfpp -ljsoncpp
CXXFLAGS_src/FPPFailover.o += -I${SRCDIR}

%.o: %.cpp Makefile
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-failover.$(SHLIB_EXT): $(OBJECTS_fpp_failover_so) ${SRCDIR}/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_failover_so) $(LIBS_fpp_failover_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-failover.so libfpp-failover.dylib $(OBJECTS_fpp_failover_so)

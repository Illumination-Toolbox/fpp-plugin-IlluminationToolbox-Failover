SRCDIR ?= /opt/fpp/src
include ${SRCDIR}/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-plugin-IlluminationToolbox-Failover.$(SHLIB_EXT)
debug: all

OBJECTS_fpp_plugin_IlluminationToolbox_Failover_so += src/FPPFailover.o
LIBS_fpp_plugin_IlluminationToolbox_Failover_so += -L${SRCDIR} -lfpp -ljsoncpp
CXXFLAGS_src/FPPFailover.o += -I${SRCDIR}

%.o: %.cpp Makefile
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-plugin-IlluminationToolbox-Failover.$(SHLIB_EXT): $(OBJECTS_fpp_plugin_IlluminationToolbox_Failover_so) ${SRCDIR}/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_plugin_IlluminationToolbox_Failover_so) $(LIBS_fpp_plugin_IlluminationToolbox_Failover_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-plugin-IlluminationToolbox-Failover.so libfpp-plugin-IlluminationToolbox-Failover.dylib $(OBJECTS_fpp_plugin_IlluminationToolbox_Failover_so)

ifeq '$(ARCH)' 'Raspberry Pi'

OBJECTS_fpp_co_dpi_pixels_so += non-gpl/DPIPixels/DPIPixels.o
LIBS_fpp_co_dpi_pixels_so=-L. -lfpp -ljsoncpp -lfpp_capeutils -Wl,-rpath=$(SRCDIR):.

CXXFLAGS_channeloutput/DPIPixels.o += -Wno-address-of-packed-member

TARGETS += libfpp-co-DPIPixels.$(SHLIB_EXT)
OBJECTS_ALL+=$(OBJECTS_fpp_co_dpi_pixels_so)

libfpp-co-DPIPixels.$(SHLIB_EXT): $(OBJECTS_fpp_co_dpi_pixels_so)  libfpp.$(SHLIB_EXT) libfpp_capeutils.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_co_dpi_pixels_so) $(LIBS_fpp_co_dpi_pixels_so) $(LDFLAGS) $(LDFLAGS_fpp_co_dpi_pixels_so) -o $@

endif

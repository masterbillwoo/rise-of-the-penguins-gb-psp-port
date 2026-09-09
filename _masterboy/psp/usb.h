#include <pspusb.h>
#include <pspusbstor.h>
#include <pspsdk.h>

// MasterBoy used to ship its own USB storage shim (oslInitUsbStorage and friends)
// because the OSLib of the day had none. Current OSLib MOD provides that API itself
// in <oslib/usb.h>, which pspcommon.h already pulls in via oslib.h, so declaring it
// again here collides with it. The implementation in psp/usb.c is likewise dropped
// from the Makefile; OSLib's is used instead.

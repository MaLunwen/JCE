/*
 * jce_macos_iokit_compat.c  Runtime compatibility shim for older macOS.
 *
 * Some third-party static objects may reference kIOMainPortDefault, which is
 * not exported on older macOS releases (for example, macOS 11).  Provide the
 * symbol in the main executable so launch does not fail with dyld.
 */

#if defined(__APPLE__)

#include <mach/mach_port.h>

/*
 * IOKit default port constants are effectively MACH_PORT_NULL in modern code.
 * Defining this symbol keeps binaries launchable on older systems that do not
 * export kIOMainPortDefault from IOKit.
 */
const mach_port_t kIOMainPortDefault = MACH_PORT_NULL;

#endif

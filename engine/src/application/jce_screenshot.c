/*
 * jce_screenshot.c  Frame capture — stub implementation.
 *
 * STATUS: Architecture stub.  All functions return safe defaults
 *         (false) until screenshot capture is implemented.
 */

#include <jce/application/jce_screenshot.h>

#include <stddef.h>

bool jce_screenshot_save(const char *path, JceScreenshotFormat format) { (void)path; (void)format; return false; }
void jce_screenshot_capture(JceScreenshotCallback cb, void *userdata)  { (void)cb; (void)userdata; }
bool jce_screenshot_pending(void)                                       { return false; }

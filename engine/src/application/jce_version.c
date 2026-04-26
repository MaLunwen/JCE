/*
 * jce_version.c  Implementation of jce_api_version() / jce_api_version_string().
 *
 * These are intentionally tiny and live in their own translation unit
 * so that the symbols are always linked into the engine library, even
 * when no other code in the engine references them — language bindings
 * call them as their first JCE entry point.
 */

#include <jce/jce_version.h>

uint32_t jce_api_version(void)
{
    return (uint32_t)JCE_API_VERSION;
}

const char *jce_api_version_string(void)
{
    return JCE_VERSION_STR;
}

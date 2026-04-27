/*
 * jce.h  Canonical single-include umbrella for the JCE engine.
 *
 * This is an alias for <jce/api.h> kept under the conventional library
 * name, so foreign-language bindings (rust-bindgen / cgo / pinvoke)
 * find a header at the predictable path  <jce/jce.h>.
 *
 * Per-layer headers (jce/api_core.h, jce/api_render.h, ...) remain
 * supported for selective inclusion.
 *
 * All public symbols are decorated with  JCE_API  (visibility) and
 * wrapped in  extern "C"  so the whole surface is callable from C,
 * C++, Rust, Go, C#, Python via standard FFI tooling.
 */

#ifndef JCE_JCE_H
#define JCE_JCE_H

#include <jce/api.h>

#endif /* JCE_JCE_H */

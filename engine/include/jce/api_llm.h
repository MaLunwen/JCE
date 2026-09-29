/*
 * api_llm.h  Language-model authoring bridge.
 *
 * Ask a model, from C, without blocking the frame -- for authoring tools
 * (the editor, or something built on the SDK), not for runtime gameplay.
 * The provider is a PROGRAM, which is how "any model" is a claim this can
 * actually make: hosted API, local runner, or somebody's shell script.
 *
 * NOT to be confused with <jce/api_ai_dispatch.h>, which is deterministic
 * in-game content generation, nor with <jce/api_ai.h>, which is behaviour
 * trees and navigation.  Three different things share the same two letters.
 */

#ifndef JCE_API_LLM_H
#define JCE_API_LLM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/llm/jce_llm.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_LLM_H */

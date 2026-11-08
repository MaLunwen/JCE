/* Editor FPS preferences are independent upper bounds. Zero contributes no
 * limit; two positive bounds retain the smaller one. No pacing state here. */
#ifndef JCE_EDITOR_FRAME_RATE_H
#define JCE_EDITOR_FRAME_RATE_H

inline int jce_editor_frame_rate_limit(int base, int limit)
{
    base = base > 0 ? base : 0;
    return limit > 0 ? (base > 0 && base < limit ? base : limit) : base;
}

#endif /* JCE_EDITOR_FRAME_RATE_H */

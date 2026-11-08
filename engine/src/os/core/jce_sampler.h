/*
 * jce_sampler.h  Wall-clock sampling profiler for the main thread.
 *
 * Why this exists: the rdtsc bracketing used everywhere else in this engine has
 * run out of resolution. Instrumenting the submit loop per entity takes sr_loop
 * from 2.12 ms to 7.55 ms -- the probes cost 2.6x the work they measure -- so
 * their percentages are upper bounds and the unattributed remainder is mostly
 * the instrument. Adding more probes makes it worse, not clearer.
 *
 * A sampler does not touch the measured code at all. A background thread
 * suspends the main thread at a fixed rate, reads its instruction pointer, and
 * resumes it; the profile falls out of where those samples land. Cost to the
 * profiled code is a brief suspend, not an inlined serialising instruction on
 * every iteration, and nothing has to be predicted in advance -- the answer
 * includes work nobody thought to bracket.
 *
 * Windows-only, including Release: enabled with JCE_SAMPLER=<hz>, reported to
 * JCE_SAMPLER_OUT (default the log) at shutdown. Off costs one branch at init.
 */

#ifndef JCE_SAMPLER_H
#define JCE_SAMPLER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start sampling the CALLING thread if JCE_SAMPLER is set. Init/shutdown belong
 * to the same host owner. Repeated init is a no-op, including while shutdown
 * awaits a join/recovery retry. JCE_SAMPLER_ALL selects a bounded process sweep:
 * 128 retained thread lifetimes per session, with skipped coverage reported. */
void jce_sampler_init(void);

/* Stop sampling and write the report. True means all sampler ownership was
 * released (also true when disabled). False retains handles, samples and any
 * acquired suspend for an owner retry; callers must defer dependent teardown.
 * Worker join waits at most 2000 ms per call. Native suspend/context/recovery
 * operations and report IO have no local deadline; total shutdown is not a
 * bounded OS operation. No shared state is released before a confirmed join.
 * Failed resume retains its acquired obligation unless a zero-duration wait
 * on the same held thread handle confirms termination. A live/failed query
 * stays retryable; same-host sampling does not promise automatic recovery. */
bool jce_sampler_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SAMPLER_H */

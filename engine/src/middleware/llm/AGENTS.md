# engine/src/middleware/llm — Program-provider transport

jce_llm.c owns one in-flight program-provider request. Tick polls/drains the real
child; timeout/cancel stops it. Public transport, statuses and concurrency remain
unchanged. This transport is separate from local model inference and HTTP/AI
policy owners; its tests do not certify model quality, tokens or GPU inference.

Lifecycle trace uses os/core/jce_trace_scope_internal.h and the common bounded
capture. An accepted request has a unique ASYNC task/span independent of public
handles. Its duration is external request wall lifetime, not CPU work. Submit,
prompt/response IO, process destroy, timeout/cancel, release and shutdown remain
explicit owner phases. DONE, provider/response failure, timeout and cancellation
end the request exactly once after process cleanup. Both begin and terminal end
restore the calling thread's prior current task. No TLS context spans frames.

Idle tick emits no additional trace events. Terminal counters contain only
handle, stdout/log/dropped/response byte counts and numeric outcome: 1 answer,
2 provider exit failure, 3 response rejection, 4 timeout, 5 cancellation. Prompt,
answer, provider arguments, secrets and contents never enter trace event names.
Disabled capture creates no request IDs or additional trace clocks/events.
Capture reset or enable-state changes require quiescent producers, as in the
common trace contract. A request begun outside a capture cannot be retrospectively
certified by enabling trace while it runs.

Tests consume real child-process transport and public trace events in
tests/middleware/llm/test_jce_llm.c. Keep original failure/timeout/stale-handle
controls, current-task restoration, exactly-one terminal, running shutdown and
disabled/idle controls. The old-wire mutation compiles a build-local copy of the
actual owner with only llm_trace_request_begin(submit_scope.id) disconnected;
it must fail request trace assertions without replacing the transport.

The positive and old-wire targets share a CTest working directory and may run
in parallel. Each fixture case passes its own absolute PID/case workspace via
the real request.work_dir; the child echoes the full substituted paths. Keep
stale-answer and prompt-retirement assertions scoped to those same paths.
Fixture teardown shuts down the real child before removing only its owned
directory. Path-name discovery is not an atomic reservation; process identity
keeps concurrently running controls in disjoint candidate namespaces.

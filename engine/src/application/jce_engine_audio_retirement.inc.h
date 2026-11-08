/* Private Engine trace adapter; audio owns quiescence and native retirement. */
static bool engine_audio_master_retire(void)
{
    JceEngineTraceScope scope = engine_trace_begin("engine.audio_master_retire");
    if (scope.id)
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE,
            "audio_master.had_device", jce_audio_master_device_live_internal(), scope.id, scope.id);
    const bool retired = jce_audio_master_try_retire_internal();
    if (scope.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE,
            "audio_master.device_live_after", jce_audio_master_device_live_internal(), scope.id, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE,
            "audio_master.retired", retired, scope.id, scope.id);
    }
    engine_trace_end(scope, retired ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return retired;
}

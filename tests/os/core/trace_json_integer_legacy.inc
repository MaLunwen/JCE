/* Exact pre-integer-args formatting owner, retained only for native controls. */
static bool trace_json_append_format(TraceJsonBuffer *buffer,
                                     const char *format,
                                     unsigned long long a,
                                     unsigned long long b,
                                     unsigned long long c)
{
    char temp[384];
    int length = SDL_snprintf(temp, sizeof(temp), format, a, b, c);

    if (length < 0 || (size_t)length >= sizeof(temp))
        return false;
    return trace_json_append(buffer, temp);
}

static bool trace_export_event(TraceJsonBuffer *json,
                               const JceTraceEvent *event,
                               bool *first)
{
    const char *phase;
    const char *event_name = event->name;
    double timestamp_us = (double)event->timestamp_ns / 1000.0;
    char prefix[256];
    char suffix[256];
    int length;

    if (!*first && !trace_json_append(json, ","))
        return false;
    *first = false;

    if (event->type == JCE_TRACE_EVENT_THREAD_REGISTER) {
        length = SDL_snprintf(
            prefix, sizeof(prefix),
            "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
            "\"tid\":%llu,\"args\":{\"name\":",
            (unsigned long long)event->thread_id);
        if (length < 0 || (size_t)length >= sizeof(prefix) ||
            !trace_json_append(json, prefix) ||
            !trace_json_append_escaped(json, event_name) ||
            !trace_json_append(json, "}}"))
            return false;
        return true;
    }

    switch (event->type) {
    case JCE_TRACE_EVENT_TASK_BEGIN:
    case JCE_TRACE_EVENT_WAIT_BEGIN:
        phase = "B";
        break;
    case JCE_TRACE_EVENT_TASK_END:
    case JCE_TRACE_EVENT_WAIT_END:
        phase = "E";
        break;
    case JCE_TRACE_EVENT_COUNTER:
        phase = "C";
        break;
    default:
        phase = "i";
        break;
    }

    if (!trace_json_append(json, "{\"name\":") ||
        !trace_json_append_escaped(json, event_name))
        return false;
    length = SDL_snprintf(
        suffix, sizeof(suffix),
        ",\"cat\":\"%s\",\"ph\":\"%s\",\"pid\":1,\"tid\":%llu,"
        "\"ts\":%.3f",
        trace_category_name(event->category), phase,
        (unsigned long long)event->thread_id, timestamp_us);
    if (length < 0 || (size_t)length >= sizeof(suffix) ||
        !trace_json_append(json, suffix))
        return false;

    if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
        if (!trace_json_append_format(
                json,
                ",\"s\":\"t\",\"args\":{\"task\":%llu,\"parent\":%llu,"
                "\"kind\":%llu}}",
                (unsigned long long)event->id,
                (unsigned long long)event->parent_id,
                (unsigned long long)event->value_u32[0]))
            return false;
    } else if (event->type == JCE_TRACE_EVENT_COUNTER) {
        length = SDL_snprintf(
            suffix, sizeof(suffix),
            ",\"args\":{\"value\":%lld,\"id\":%llu,\"parent\":%llu}}",
            (long long)(int64_t)event->value_u64,
            (unsigned long long)event->id,
            (unsigned long long)event->parent_id);
        if (length < 0 || (size_t)length >= sizeof(suffix) ||
            !trace_json_append(json, suffix))
            return false;
    } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
        length = SDL_snprintf(
            suffix, sizeof(suffix),
            ",\"args\":{\"id\":%llu,\"parent\":%llu,\"ns\":%llu,"
            "\"state\":%u}}",
            (unsigned long long)event->id,
            (unsigned long long)event->parent_id,
            (unsigned long long)event->value_u64,
            (unsigned)event->value_u32[0]);
        if (length < 0 || (size_t)length >= sizeof(suffix) ||
            !trace_json_append(json, suffix))
            return false;
    } else if (event->type == JCE_TRACE_EVENT_TASK_BEGIN ||
               event->type == JCE_TRACE_EVENT_WAIT_BEGIN ||
               event->type == JCE_TRACE_EVENT_WAIT_END) {
        if (!trace_json_append_format(
                json, ",\"args\":{\"id\":%llu,\"parent\":%llu,\"ns\":%llu}}",
                (unsigned long long)event->id,
                (unsigned long long)event->parent_id,
                (unsigned long long)event->value_u64))
            return false;
    } else {
        if (!trace_json_append_format(
                json, ",\"s\":\"t\",\"args\":{\"id\":%llu,\"value\":%llu,"
                "\"aux\":%llu}}",
                (unsigned long long)event->id,
                (unsigned long long)event->value_u64,
                (unsigned long long)event->value_u32[0]))
            return false;
    }
    return true;
}

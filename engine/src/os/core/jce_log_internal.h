/* Private terminal diagnostic ownership, not a second logger. */
#ifndef JCE_LOG_INTERNAL_H
#define JCE_LOG_INTERNAL_H

typedef enum JceLogFinalClose {
    JCE_LOG_FINAL_CLOSE_UNOBSERVED = 0,
    JCE_LOG_FINAL_CLOSE_NO_FILE,
    JCE_LOG_FINAL_CLOSE_API_OK,
    JCE_LOG_FINAL_CLOSE_API_FAILED
} JceLogFinalClose;
/* Owner-thread observation of the latest final shutdown's flush/close API
 * returns. Reset by init. No file is distinct from success; no durability claim. */
JceLogFinalClose jce_log_final_close_internal(void);

/* Owner-thread only, after every other log producer has joined. Stop and join
 * the backend once, retaining ring storage, file and observer locks. Following
 * writes emit synchronously through the same file/sink path. Idempotent;
 * jce_log_init may restart the retained backend, and jce_log_shutdown performs
 * final release. No concurrent log producer or logger configuration is allowed
 * across this transition, and it must never be called from a log sink. */
void jce_log_quiesce_internal(void);

#endif

/* es_script_probe.h — counted evidence that each language's script ran.
 * See es_script_probe.c for why a transform and not a log line. */
#ifndef ES_SCRIPT_PROBE_H
#define ES_SCRIPT_PROBE_H

#include <jce/api.h>

/* Resolve the probe entities.  Call once, AFTER the startup scene is loaded
 * (they are scene entities) and after jce_runtime_create — an entity that is
 * missing is reported by name here rather than showing up as a zero later. */
void es_script_probe_init(JceScene *scene);

/* Sample every probe; prints a table every 10 s. */
void es_script_probe_update(JceScene *scene, float dt);

/* Print the table now.  `why` labels the line ("periodic", "exit", ...). */
void es_script_probe_report(const char *why);

/* Languages whose script has BOTH started and updated at least once. */
int es_script_probe_live_count(void);

#endif /* ES_SCRIPT_PROBE_H */

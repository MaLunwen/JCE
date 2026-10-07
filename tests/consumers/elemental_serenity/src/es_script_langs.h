/* es_script_langs.h — see es_script_langs.c for the whole contract.
 *
 * Call es_script_langs_register() BEFORE app_init(): the startup scene's
 * Script components are instantiated inside jce_runtime_create(), and a
 * language registered after that point is a language the scene has already
 * been refused by. */
#ifndef ES_SCRIPT_LANGS_H
#define ES_SCRIPT_LANGS_H

void es_script_langs_register(void);
void es_script_langs_shutdown(void);

#endif /* ES_SCRIPT_LANGS_H */

/* Private component patch ownership, included once by the scene JSON TU.
 * Registered serializers transfer an independently owned subtree to their
 * output array. Reference/borrowed subtrees are not this private contract.
 * Void serializers can still omit a field after an internal allocation failure;
 * void parsers can still fail after entry. Neither is made transactional here. */
#ifndef JCE_SCENE_JSON_PATCH_INTERNAL_H
#define JCE_SCENE_JSON_PATCH_INTERNAL_H

#include "jce_component_registry_internal.h"
#include "jce_scene_line_colors_internal.h"
#include <jce/os/core/jce_json.h>
#include <cjson/cJSON.h>
#include <string.h>

/* Reusing an equal primitive only avoids disposable JSON copies. The caller
 * still parses the merged object and invokes its ordinary typed setter. */
static bool rs_json_same_scalar(const cJSON *current, const cJSON *patch)
{
    if (!current || (current->type & 0xff) != (patch->type & 0xff)) return false;
    switch (patch->type & 0xff) {
    case cJSON_NULL:
    case cJSON_False:
    case cJSON_True:
        return true;
    case cJSON_Number:
        /* No epsilon comparison: adjacent doubles and signed zero differ. */
        return current->valueint == patch->valueint &&
            memcmp(&current->valuedouble, &patch->valuedouble,
                   sizeof(current->valuedouble)) == 0;
    case cJSON_String:
        return current->valuestring && patch->valuestring &&
            strcmp(current->valuestring, patch->valuestring) == 0;
    default:
        return false; /* Arrays, objects, raw and invalid nodes never elide. */
    }
}

/* Objects merge recursively; arrays and changed/other values replace.
 * Failure may modify this disposable tree. Callers must discard it before
 * any scene parse/set; a successfully attached node belongs to destination. */
static bool rs_json_merge(cJSON *destination, const cJSON *patch)
{
    if (!cJSON_IsObject(destination) || !cJSON_IsObject(patch)) return false;
    for (const cJSON *item = patch->child; item; item = item->next) {
        cJSON *current = cJSON_GetObjectItemCaseSensitive(destination, item->string);
        if (rs_json_same_scalar(current, item)) continue;
        if (current && cJSON_IsObject(current) && cJSON_IsObject(item)) {
            if (!rs_json_merge(current, item)) return false;
        } else {
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (!copy) return false;
            bool attached = current
                ? cJSON_ReplaceItemInObjectCaseSensitive(destination, item->string, copy)
                : cJSON_AddItemToObject(destination, item->string, copy);
            if (!attached) {
                cJSON_Delete(copy);
                return false;
            }
        }
    }
    return true;
}

/* Match the scene loader's component envelope. Serializers can return either
 * a flat object or a versioned properties object. Patches accept both forms. */
static cJSON *component_patch_payload(cJSON *object)
{
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(object, "properties");
    return cJSON_IsObject(properties) ? properties : object;
}

/* comp_set is a partial overwrite. An absent component uses parse defaults;
 * a present component must construct and merge its current state successfully.
 * Falling back to parse(patch) on failure would reset unmentioned ECS fields. */
bool jce_scene_component_apply_json(JceScene *scene, JceEntity entity,
                                    const char *type, const char *json)
{
    if (!scene || !type || !json) return false;
    int component = jce_component_find(type);
    if (component == JCE_COMP_ID_INVALID) return false;
    const JceComponentDesc *descriptor = jce_component_desc(component);
    if (!descriptor || !descriptor->parse) return false;

    cJSON *patch = cJSON_Parse(json);
    if (!patch) return false;
    if (!cJSON_IsObject(patch)) { cJSON_Delete(patch); return false; }

    const cJSON *patch_fields = component_patch_payload(patch);
    if (!descriptor->has || !descriptor->has(scene, entity)) {
        descriptor->parse(scene, entity, patch_fields);
        cJSON_Delete(patch);
        return true;
    }
    /* Registry resolution preserves the existing component aliases. Only an
     * existing LineRenderer with an unambiguous numeric colour patch can skip
     * the disposable serialization/merge/reparse of its point coordinates. */
    if (strcmp(descriptor->name, "LineRenderer") == 0 &&
        scene_line_try_color_patch(scene, entity, patch_fields)) {
        cJSON_Delete(patch);
        return true;
    }
    if (!descriptor->serialize) { cJSON_Delete(patch); return false; }

    cJSON *array = cJSON_CreateArray();
    if (!array) { cJSON_Delete(patch); return false; }
    descriptor->serialize(scene, entity, array);
    cJSON *first = cJSON_GetArrayItem(array, 0);
    if (!cJSON_IsObject(first)) {
        cJSON_Delete(array);
        cJSON_Delete(patch);
        return false;
    }
    /* Move the owned object; cloning it before deleting the array duplicates
     * every node, key and string just to establish the same ownership. */
    jce_json_detach(array, first);
    cJSON_Delete(array);
    cJSON *fields = component_patch_payload(first);
    if (!rs_json_merge(fields, patch_fields)) {
        cJSON_Delete(first);
        cJSON_Delete(patch);
        return false;
    }
    descriptor->parse(scene, entity, fields);
    cJSON_Delete(first);
    cJSON_Delete(patch);
    return true;
}

#endif

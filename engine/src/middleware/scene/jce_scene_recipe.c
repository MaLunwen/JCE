/* Strict semantic scene-recipe validation and JSON ingestion. */

#include <jce/middleware/scene/jce_scene_recipe.h>

#include <jce/os/core/jce_json.h>

#include <limits.h>
#include <string.h>

#define JCE_JSON_EXACT_INTEGER_MAX 9007199254740991.0

static void recipe_error_clear(JceSceneRecipeError *error)
{
    if (!error)
        return;
    memset(error, 0, sizeof(*error));
    error->status = JCE_SCENE_RECIPE_OK;
    error->item_index = UINT32_MAX;
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    size_t i = 0u;

    if (!dst || capacity == 0u)
        return;
    if (src) {
        while (i + 1u < capacity && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static JceSceneRecipeStatus recipe_error_set(JceSceneRecipeError *error,
                                              JceSceneRecipeStatus status,
                                              uint32_t item_index,
                                              const char *field)
{
    if (error) {
        recipe_error_clear(error);
        error->status = status;
        error->item_index = item_index;
        copy_text(error->field, sizeof(error->field), field);
    }
    return status;
}

static bool string_terminated(const char *text, size_t capacity,
                              size_t *out_length)
{
    size_t i;

    if (!text || capacity == 0u)
        return false;
    for (i = 0u; i < capacity; ++i) {
        if (text[i] == '\0') {
            if (out_length)
                *out_length = i;
            return true;
        }
    }
    return false;
}

static bool token_char_valid(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == ':';
}

static bool token_valid(const char *token, size_t capacity)
{
    size_t length;
    size_t i;

    if (!string_terminated(token, capacity, &length) || length == 0u)
        return false;
    for (i = 0u; i < length; ++i) {
        if (!token_char_valid(token[i]))
            return false;
    }
    return true;
}

static bool optional_token_valid(const char *token, size_t capacity)
{
    size_t length;

    if (!string_terminated(token, capacity, &length))
        return false;
    return length == 0u || token_valid(token, capacity);
}

static bool double_is_finite(double value)
{
    return value == value && value <= 1.7976931348623157e308 &&
           value >= -1.7976931348623157e308;
}

JCE_API bool JCE_CALL jce_scene_recipe_quantize(double value, int32_t scale,
                                                int32_t *out)
{
    double scaled;
    double rounded;

    if (!out || scale <= 0 || !double_is_finite(value))
        return false;
    scaled = value * (double)scale;
    if (!double_is_finite(scaled) ||
        scaled > (double)INT32_MAX - 0.5 ||
        scaled < (double)INT32_MIN + 0.5)
        return false;
    rounded = scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5;
    *out = (int32_t)rounded;
    return true;
}

JCE_API void JCE_CALL jce_scene_recipe_init(JceSceneRecipe *recipe)
{
    if (!recipe)
        return;
    memset(recipe, 0, sizeof(*recipe));
    recipe->schema_version = JCE_SCENE_RECIPE_SCHEMA_VERSION;
    recipe->compiler_version = JCE_SCENE_COMPILER_VERSION;
}

JCE_API void JCE_CALL jce_scene_catalog_init(JceSceneCatalog *catalog)
{
    if (!catalog)
        return;
    memset(catalog, 0, sizeof(*catalog));
    catalog->schema_version = JCE_SCENE_CATALOG_SCHEMA_VERSION;
}

static bool role_numbers_valid(const JceSceneRecipeRole *role)
{
    int32_t ignored;
    uint32_t axis;

    for (axis = 0u; axis < 3u; ++axis) {
        if (!jce_scene_recipe_quantize(role->center[axis], 1000, &ignored) ||
            !jce_scene_recipe_quantize(role->extent[axis], 1000, &ignored) ||
            role->extent[axis] < 0.0)
            return false;
    }
    if (!jce_scene_recipe_quantize(role->min_separation, 1000, &ignored) ||
        role->min_separation < 0.0)
        return false;
    if (!jce_scene_recipe_quantize(role->yaw_min_degrees, 1000, &ignored) ||
        !jce_scene_recipe_quantize(role->yaw_max_degrees, 1000, &ignored) ||
        role->yaw_min_degrees > role->yaw_max_degrees)
        return false;
    if (!jce_scene_recipe_quantize(role->scale_min, 1000, &ignored) ||
        !jce_scene_recipe_quantize(role->scale_max, 1000, &ignored) ||
        role->scale_min <= 0.0 || role->scale_min > role->scale_max)
        return false;
    return true;
}

static int32_t recipe_find_role(const JceSceneRecipe *recipe,
                                const char *stable_role)
{
    uint32_t i;

    for (i = 0u; i < recipe->role_count; ++i) {
        if (strcmp(recipe->roles[i].stable_role, stable_role) == 0)
            return (int32_t)i;
    }
    return -1;
}

static bool hierarchy_visit(const JceSceneRecipe *recipe, uint32_t index,
                            uint8_t *marks, uint32_t *out_cycle_index)
{
    const JceSceneRecipeRole *role = &recipe->roles[index];
    int32_t parent_index;

    if (marks[index] == 2u)
        return true;
    if (marks[index] == 1u) {
        if (out_cycle_index)
            *out_cycle_index = index;
        return false;
    }
    marks[index] = 1u;
    if (role->parent_role[0] != '\0') {
        parent_index = recipe_find_role(recipe, role->parent_role);
        if (parent_index < 0 ||
            !hierarchy_visit(recipe, (uint32_t)parent_index, marks,
                             out_cycle_index))
            return false;
    }
    marks[index] = 2u;
    return true;
}

static JceSceneRecipeStatus validate_hierarchy(
    const JceSceneRecipe *recipe, JceSceneRecipeError *error)
{
    uint8_t marks[JCE_SCENE_RECIPE_MAX_ROLES] = { 0u };
    uint32_t i;

    for (i = 0u; i < recipe->role_count; ++i) {
        const JceSceneRecipeRole *role = &recipe->roles[i];
        int32_t parent_index;

        if (!optional_token_valid(role->parent_role,
                                  sizeof(role->parent_role)))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                    i, "parentRole");
        if (role->parent_role[0] == '\0') {
            if (role->parent_instance != 0u)
                return recipe_error_set(
                    error, JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                    i, "parentInstance");
            continue;
        }
        parent_index = recipe_find_role(recipe, role->parent_role);
        if (parent_index < 0 || (uint32_t)parent_index == i)
            return recipe_error_set(error,
                                    JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                                    i, "parentRole");
        if (role->parent_instance >=
            recipe->roles[parent_index].min_count)
            return recipe_error_set(error,
                                    JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                                    i, "parentInstance");
    }

    for (i = 0u; i < recipe->role_count; ++i) {
        uint32_t cycle_index = i;
        if (!hierarchy_visit(recipe, i, marks, &cycle_index))
            return recipe_error_set(error,
                                    JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                                    cycle_index, "parentRole");
    }
    return JCE_SCENE_RECIPE_OK;
}

JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_recipe_validate(const JceSceneRecipe *recipe,
                          JceSceneRecipeError *error)
{
    uint32_t i;
    uint32_t j;
    uint32_t total = 0u;

    recipe_error_clear(error);
    if (!recipe)
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_ARGUMENT,
                                UINT32_MAX, "recipe");
    if (recipe->schema_version != JCE_SCENE_RECIPE_SCHEMA_VERSION ||
        recipe->compiler_version != JCE_SCENE_COMPILER_VERSION)
        return recipe_error_set(error, JCE_SCENE_RECIPE_UNSUPPORTED_VERSION,
                                UINT32_MAX, "version");
    if (recipe->role_count > JCE_SCENE_RECIPE_MAX_ROLES)
        return recipe_error_set(error, JCE_SCENE_RECIPE_CAPACITY_EXCEEDED,
                                UINT32_MAX, "roles");

    for (i = 0u; i < recipe->role_count; ++i) {
        const JceSceneRecipeRole *role = &recipe->roles[i];

        if (!token_valid(role->stable_role, sizeof(role->stable_role)))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                    i, "stableRole");
        if (!token_valid(role->capability, sizeof(role->capability)))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                    i, "capability");
        if (role->min_count > role->max_count ||
            role->max_count > JCE_SCENE_RECIPE_MAX_INSTANCES_PER_ROLE ||
            (role->required && role->min_count == 0u) ||
            (role->placement == JCE_SCENE_PLACEMENT_SINGLE &&
             role->max_count > 1u))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_RANGE,
                                    i, "count");
        if ((int)role->placement < (int)JCE_SCENE_PLACEMENT_SINGLE ||
            (int)role->placement > (int)JCE_SCENE_PLACEMENT_RING)
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_RANGE,
                                    i, "placement");
        if (!role_numbers_valid(role))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_NUMBER,
                                    i, "transform");
        if (UINT32_MAX - total < role->max_count)
            return recipe_error_set(error, JCE_SCENE_RECIPE_CAPACITY_EXCEEDED,
                                    i, "maxCount");
        total += role->max_count;
        if (total > JCE_SCENE_RECIPE_MAX_TOTAL_INSTANCES)
            return recipe_error_set(error, JCE_SCENE_RECIPE_CAPACITY_EXCEEDED,
                                    i, "maxCount");
        for (j = 0u; j < i; ++j) {
            if (strcmp(role->stable_role, recipe->roles[j].stable_role) == 0)
                return recipe_error_set(error, JCE_SCENE_RECIPE_DUPLICATE_ROLE,
                                        i, role->stable_role);
        }
    }
    return validate_hierarchy(recipe, error);
}

JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_catalog_validate(const JceSceneCatalog *catalog,
                           JceSceneRecipeError *error)
{
    uint32_t i;
    uint32_t j;

    recipe_error_clear(error);
    if (!catalog)
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_ARGUMENT,
                                UINT32_MAX, "catalog");
    if (catalog->schema_version != JCE_SCENE_CATALOG_SCHEMA_VERSION)
        return recipe_error_set(error, JCE_SCENE_RECIPE_UNSUPPORTED_VERSION,
                                UINT32_MAX, "catalogVersion");
    if (catalog->content_hash == 0u ||
        catalog->entry_count > JCE_SCENE_CATALOG_MAX_ENTRIES)
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_CATALOG,
                                UINT32_MAX, "catalog");
    for (i = 0u; i < catalog->entry_count; ++i) {
        const JceSceneCatalogEntry *entry = &catalog->entries[i];

        if (!token_valid(entry->asset_id, sizeof(entry->asset_id)))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                    i, "assetId");
        if (!token_valid(entry->capability, sizeof(entry->capability)))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                    i, "capability");
        if (entry->content_hash == 0u || (entry->enabled && entry->weight == 0u))
            return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_CATALOG,
                                    i, "entry");
        for (j = 0u; j < i; ++j) {
            if (strcmp(entry->asset_id, catalog->entries[j].asset_id) == 0)
                return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_CATALOG,
                                        i, "assetId");
        }
    }
    return JCE_SCENE_RECIPE_OK;
}

static bool key_allowed(const char *key, const char *const *allowed,
                        size_t allowed_count)
{
    size_t i;

    if (!key)
        return false;
    for (i = 0u; i < allowed_count; ++i) {
        if (strcmp(key, allowed[i]) == 0)
            return true;
    }
    return false;
}

static JceSceneRecipeStatus reject_unknown_fields(
    const JceJson *object, const char *const *allowed, size_t allowed_count,
    uint32_t item_index, JceSceneRecipeError *error)
{
    JceJson *child;

    for (child = jce_json_first_child(object); child;
         child = jce_json_next_sibling(child)) {
        const char *key = jce_json_member_key(child);
        if (!key_allowed(key, allowed, allowed_count))
            return recipe_error_set(error, JCE_SCENE_RECIPE_UNKNOWN_FIELD,
                                    item_index, key ? key : "field");
    }
    return JCE_SCENE_RECIPE_OK;
}

static bool parse_u64_text(const char *text, uint64_t *out)
{
    uint64_t value = 0u;
    uint32_t base = 10u;
    size_t i = 0u;
    bool any = false;

    if (!text || !out)
        return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16u;
        i = 2u;
    }
    for (; text[i] != '\0'; ++i) {
        uint32_t digit;
        char c = text[i];

        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (base == 16u && c >= 'a' && c <= 'f')
            digit = 10u + (uint32_t)(c - 'a');
        else if (base == 16u && c >= 'A' && c <= 'F')
            digit = 10u + (uint32_t)(c - 'A');
        else
            return false;
        if (digit >= base || value > (UINT64_MAX - digit) / base)
            return false;
        value = value * base + digit;
        any = true;
    }
    if (!any)
        return false;
    *out = value;
    return true;
}

static bool json_u64(const JceJson *object, const char *key, uint64_t *out)
{
    JceJson *node = jce_json_get(object, key);

    if (!node || !out)
        return false;
    if (jce_json_is_number(node)) {
        double value = jce_json_number_value(node, -1.0);
        uint64_t converted;

        if (!double_is_finite(value) || value < 0.0 ||
            value > JCE_JSON_EXACT_INTEGER_MAX)
            return false;
        converted = (uint64_t)value;
        if ((double)converted != value)
            return false;
        *out = converted;
        return true;
    }
    if (jce_json_is_string(node))
        return parse_u64_text(jce_json_string_value(node, NULL), out);
    return false;
}

static bool json_u32(const JceJson *object, const char *key, uint32_t *out)
{
    uint64_t value;

    if (!json_u64(object, key, &value) || value > UINT32_MAX)
        return false;
    *out = (uint32_t)value;
    return true;
}

static bool json_double(const JceJson *object, const char *key,
                        double default_value, double *out)
{
    JceJson *node = jce_json_get(object, key);
    double value;

    if (!out)
        return false;
    if (!node) {
        *out = default_value;
        return true;
    }
    if (!jce_json_is_number(node))
        return false;
    value = jce_json_number_value(node, default_value);
    if (!double_is_finite(value))
        return false;
    *out = value;
    return true;
}

static bool json_double_array(const JceJson *object, const char *key,
                              double *out, uint32_t count,
                              const double *defaults)
{
    JceJson *array = jce_json_get(object, key);
    uint32_t i;

    if (!array) {
        for (i = 0u; i < count; ++i)
            out[i] = defaults ? defaults[i] : 0.0;
        return true;
    }
    if (!jce_json_is_array(array) ||
        jce_json_array_size(array) != (int)count)
        return false;
    for (i = 0u; i < count; ++i) {
        JceJson *node = jce_json_array_at(array, (int)i);
        if (!jce_json_is_number(node))
            return false;
        out[i] = jce_json_number_value(node, 0.0);
        if (!double_is_finite(out[i]))
            return false;
    }
    return true;
}

static bool json_token(const JceJson *object, const char *key,
                       char *out, size_t capacity)
{
    JceJson *node = jce_json_get(object, key);
    const char *value;
    size_t length;

    if (!node || !jce_json_is_string(node))
        return false;
    value = jce_json_string_value(node, NULL);
    if (!value || !string_terminated(value, capacity, &length) ||
        length + 1u > capacity)
        return false;
    copy_text(out, capacity, value);
    return token_valid(out, capacity);
}

static bool parse_placement(const JceJson *object, JceScenePlacement *out)
{
    JceJson *node = jce_json_get(object, "placement");
    const char *value;

    if (!node || !jce_json_is_string(node) || !out)
        return false;
    value = jce_json_string_value(node, "");
    if (strcmp(value, "single") == 0)
        *out = JCE_SCENE_PLACEMENT_SINGLE;
    else if (strcmp(value, "grid") == 0)
        *out = JCE_SCENE_PLACEMENT_GRID;
    else if (strcmp(value, "scatter") == 0)
        *out = JCE_SCENE_PLACEMENT_SCATTER;
    else if (strcmp(value, "ring") == 0)
        *out = JCE_SCENE_PLACEMENT_RING;
    else
        return false;
    return true;
}

static JceSceneRecipeStatus parse_role(const JceJson *object, uint32_t index,
                                       JceSceneRecipeRole *role,
                                       JceSceneRecipeError *error)
{
    static const char *const allowed[] = {
        "stableRole", "capability", "minCount", "maxCount", "placement",
        "required", "center", "extent", "minSeparation", "yawDegrees",
        "scale", "parentRole", "parentInstance"
    };
    static const double zero3[3] = { 0.0, 0.0, 0.0 };
    static const double zero2[2] = { 0.0, 0.0 };
    static const double one2[2] = { 1.0, 1.0 };
    double yaw[2];
    double scale[2];
    JceJson *required;
    JceSceneRecipeStatus status;

    if (!jce_json_is_object(object))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "role");
    status = reject_unknown_fields(object, allowed,
                                   sizeof(allowed) / sizeof(allowed[0]),
                                   index, error);
    if (status != JCE_SCENE_RECIPE_OK)
        return status;

    memset(role, 0, sizeof(*role));
    role->scale_min = 1.0;
    role->scale_max = 1.0;
    if (!json_token(object, "stableRole", role->stable_role,
                    sizeof(role->stable_role)))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                index, "stableRole");
    if (!json_token(object, "capability", role->capability,
                    sizeof(role->capability)))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                index, "capability");
    if (jce_json_get(object, "parentRole") &&
        !json_token(object, "parentRole", role->parent_role,
                    sizeof(role->parent_role)))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                index, "parentRole");
    if (jce_json_get(object, "parentInstance") &&
        !json_u32(object, "parentInstance", &role->parent_instance))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "parentInstance");
    if (!json_u32(object, "minCount", &role->min_count) ||
        !json_u32(object, "maxCount", &role->max_count))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "count");
    if (!parse_placement(object, &role->placement))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "placement");

    required = jce_json_get(object, "required");
    if (required && !jce_json_is_bool(required))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "required");
    role->required = required ? jce_json_get_bool(object, "required", false)
                              : false;
    if (!json_double_array(object, "center", role->center, 3u, zero3) ||
        !json_double_array(object, "extent", role->extent, 3u, zero3) ||
        !json_double(object, "minSeparation", 0.0,
                     &role->min_separation) ||
        !json_double_array(object, "yawDegrees", yaw, 2u, zero2) ||
        !json_double_array(object, "scale", scale, 2u, one2))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "transform");
    role->yaw_min_degrees = yaw[0];
    role->yaw_max_degrees = yaw[1];
    role->scale_min = scale[0];
    role->scale_max = scale[1];
    return JCE_SCENE_RECIPE_OK;
}

JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_recipe_parse_json(const char *json, size_t length,
                            JceSceneRecipe *out_recipe,
                            JceSceneRecipeError *error)
{
    static const char *const allowed[] = {
        "schemaVersion", "compilerVersion", "requestId", "seed", "roles"
    };
    JceJson *root;
    JceJson *roles;
    JceSceneRecipe parsed;
    JceSceneRecipeStatus status;
    int count;
    uint32_t i;

    recipe_error_clear(error);
    if (!json || !out_recipe)
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_ARGUMENT,
                                UINT32_MAX, "json");
    root = jce_json_parse(json, length);
    if (!root)
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "json");
    if (!jce_json_is_object(root)) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "root");
    }
    status = reject_unknown_fields(root, allowed,
                                   sizeof(allowed) / sizeof(allowed[0]),
                                   UINT32_MAX, error);
    if (status != JCE_SCENE_RECIPE_OK) {
        jce_json_free(root);
        return status;
    }

    jce_scene_recipe_init(&parsed);
    if (!json_u32(root, "schemaVersion", &parsed.schema_version) ||
        !json_u32(root, "compilerVersion", &parsed.compiler_version) ||
        !json_u64(root, "requestId", &parsed.request_id) ||
        !json_u64(root, "seed", &parsed.seed)) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "header");
    }

    roles = jce_json_get(root, "roles");
    if (!roles || !jce_json_is_array(roles)) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "roles");
    }
    count = jce_json_array_size(roles);
    if (count < 0 || (uint32_t)count > JCE_SCENE_RECIPE_MAX_ROLES) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_CAPACITY_EXCEEDED,
                                UINT32_MAX, "roles");
    }
    parsed.role_count = (uint32_t)count;
    for (i = 0u; i < parsed.role_count; ++i) {
        status = parse_role(jce_json_array_at(roles, (int)i), i,
                            &parsed.roles[i], error);
        if (status != JCE_SCENE_RECIPE_OK) {
            jce_json_free(root);
            return status;
        }
    }
    jce_json_free(root);

    status = jce_scene_recipe_validate(&parsed, error);
    if (status == JCE_SCENE_RECIPE_OK)
        *out_recipe = parsed;
    return status;
}

static JceSceneRecipeStatus parse_catalog_entry(
    const JceJson *object, uint32_t index, JceSceneCatalogEntry *entry,
    JceSceneRecipeError *error)
{
    static const char *const allowed[] = {
        "assetId", "capability", "contentHash", "weight", "enabled"
    };
    JceJson *enabled;
    JceSceneRecipeStatus status;

    if (!jce_json_is_object(object))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "entry");
    status = reject_unknown_fields(object, allowed,
                                   sizeof(allowed) / sizeof(allowed[0]),
                                   index, error);
    if (status != JCE_SCENE_RECIPE_OK)
        return status;

    memset(entry, 0, sizeof(*entry));
    if (!json_token(object, "assetId", entry->asset_id,
                    sizeof(entry->asset_id)))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                index, "assetId");
    if (!json_token(object, "capability", entry->capability,
                    sizeof(entry->capability)))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_TOKEN,
                                index, "capability");
    if (!json_u64(object, "contentHash", &entry->content_hash) ||
        !json_u32(object, "weight", &entry->weight))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "entry");
    enabled = jce_json_get(object, "enabled");
    if (!enabled || !jce_json_is_bool(enabled))
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                index, "enabled");
    entry->enabled = jce_json_get_bool(object, "enabled", false);
    return JCE_SCENE_RECIPE_OK;
}

JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_catalog_parse_json(const char *json, size_t length,
                             JceSceneCatalog *out_catalog,
                             JceSceneRecipeError *error)
{
    static const char *const allowed[] = {
        "schemaVersion", "contentHash", "entries"
    };
    JceJson *root;
    JceJson *entries;
    JceSceneCatalog parsed;
    JceSceneRecipeStatus status;
    int count;
    uint32_t i;

    recipe_error_clear(error);
    if (!json || !out_catalog)
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_ARGUMENT,
                                UINT32_MAX, "json");
    root = jce_json_parse(json, length);
    if (!root || !jce_json_is_object(root)) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "root");
    }
    status = reject_unknown_fields(root, allowed,
                                   sizeof(allowed) / sizeof(allowed[0]),
                                   UINT32_MAX, error);
    if (status != JCE_SCENE_RECIPE_OK) {
        jce_json_free(root);
        return status;
    }

    jce_scene_catalog_init(&parsed);
    if (!json_u32(root, "schemaVersion", &parsed.schema_version) ||
        !json_u64(root, "contentHash", &parsed.content_hash)) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "header");
    }
    entries = jce_json_get(root, "entries");
    if (!entries || !jce_json_is_array(entries)) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_INVALID_JSON,
                                UINT32_MAX, "entries");
    }
    count = jce_json_array_size(entries);
    if (count < 0 || (uint32_t)count > JCE_SCENE_CATALOG_MAX_ENTRIES) {
        jce_json_free(root);
        return recipe_error_set(error, JCE_SCENE_RECIPE_CAPACITY_EXCEEDED,
                                UINT32_MAX, "entries");
    }
    parsed.entry_count = (uint32_t)count;
    for (i = 0u; i < parsed.entry_count; ++i) {
        status = parse_catalog_entry(jce_json_array_at(entries, (int)i), i,
                                     &parsed.entries[i], error);
        if (status != JCE_SCENE_RECIPE_OK) {
            jce_json_free(root);
            return status;
        }
    }
    jce_json_free(root);

    status = jce_scene_catalog_validate(&parsed, error);
    if (status == JCE_SCENE_RECIPE_OK)
        *out_catalog = parsed;
    return status;
}

/* Deterministic SceneRecipe compiler and canonical FrozenPlan wire format. */

#include <jce/middleware/scene/jce_scene_compiler.h>

#include <jce/os/core/jce_rand.h>

#include <limits.h>
#include <string.h>

#define JCE_PLAN_MAGIC 0x5046534au /* "JSFP" in little-endian bytes */
#define JCE_FNV64_OFFSET 14695981039346656037ULL
#define JCE_FNV64_PRIME 1099511628211ULL
#define JCE_DEFAULT_PLACEMENT_ATTEMPTS 64u

typedef struct {
    int32_t center[3];
    int32_t extent[3];
    int32_t min_separation;
    int32_t yaw_min;
    int32_t yaw_max;
    int32_t scale_min;
    int32_t scale_max;
} QuantizedRole;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t offset;
    bool ok;
} PlanWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
    bool ok;
} PlanReader;

static void compile_error_clear(JceSceneCompileError *error)
{
    if (!error)
        return;
    memset(error, 0, sizeof(*error));
    error->status = JCE_SCENE_COMPILE_OK;
    error->role_index = UINT32_MAX;
    error->contract_error.item_index = UINT32_MAX;
}

static JceSceneCompileStatus compile_error_set(
    JceSceneCompileError *error, JceSceneCompileStatus status,
    uint32_t role_index, uint32_t attempt_count,
    const JceSceneRecipeError *contract_error)
{
    if (error) {
        compile_error_clear(error);
        error->status = status;
        error->role_index = role_index;
        error->attempt_count = attempt_count;
        if (contract_error)
            error->contract_error = *contract_error;
    }
    return status;
}

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t i;

    if (!text)
        return capacity;
    for (i = 0u; i < capacity; ++i) {
        if (text[i] == '\0')
            return i;
    }
    return capacity;
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

static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t size)
{
    const uint8_t *src = (const uint8_t *)bytes;
    size_t i;

    for (i = 0u; i < size; ++i) {
        hash ^= (uint64_t)src[i];
        hash *= JCE_FNV64_PRIME;
    }
    return hash;
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xffu);
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    uint8_t bytes[4];
    uint32_t i;
    for (i = 0u; i < 4u; ++i)
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8];
    uint32_t i;
    for (i = 0u; i < 8u; ++i)
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_i32(uint64_t hash, int32_t value)
{
    return hash_u32(hash, (uint32_t)value);
}

static uint64_t hash_text(uint64_t hash, const char *text, size_t capacity)
{
    size_t length = bounded_length(text, capacity);

    if (length >= capacity || length > UINT16_MAX)
        return 0u;
    hash = hash_u16(hash, (uint16_t)length);
    return hash_bytes(hash, text, length);
}

static uint64_t stable_entity_id(const char *stable_role,
                                 uint32_t instance_index)
{
    uint64_t hash = JCE_FNV64_OFFSET;
    size_t length = bounded_length(stable_role, JCE_SCENE_STABLE_ROLE_MAX);

    hash = hash_bytes(hash, stable_role, length);
    hash = hash_u32(hash, instance_index);
    hash = hash_u32(hash, JCE_SCENE_COMPILER_VERSION);
    return hash ? hash : 1u;
}

static void seed_role_rng(JceRng *rng, const JceSceneRecipe *recipe,
                          const JceSceneCatalog *catalog,
                          const JceSceneRecipeRole *role,
                          const QuantizedRole *quantized)
{
    uint64_t state = JCE_FNV64_OFFSET;
    uint64_t stream = JCE_FNV64_OFFSET;
    uint32_t axis;

    state = hash_u64(state, recipe->seed);
    state = hash_u64(state, catalog->content_hash);
    state = hash_u32(state, recipe->compiler_version);
    state = hash_text(state, role->stable_role,
                      sizeof(role->stable_role));
    state = hash_text(state, role->capability,
                      sizeof(role->capability));
    state = hash_u32(state, role->min_count);
    state = hash_u32(state, role->max_count);
    state = hash_u32(state, (uint32_t)role->placement);
    state = hash_u32(state, role->required ? 1u : 0u);
    for (axis = 0u; axis < 3u; ++axis) {
        state = hash_i32(state, quantized->center[axis]);
        state = hash_i32(state, quantized->extent[axis]);
    }
    state = hash_i32(state, quantized->min_separation);
    state = hash_i32(state, quantized->yaw_min);
    state = hash_i32(state, quantized->yaw_max);
    state = hash_i32(state, quantized->scale_min);
    state = hash_i32(state, quantized->scale_max);

    stream = hash_text(stream, role->stable_role,
                       sizeof(role->stable_role));
    stream = hash_u32(stream, recipe->compiler_version);
    stream = hash_u64(stream, catalog->content_hash);
    jce_rng_seed(rng, state ? state : 1u, stream ? stream : 1u);
}

static uint32_t rng_bounded_u32(JceRng *rng, uint32_t bound)
{
    uint32_t threshold;
    uint32_t value;

    if (bound <= 1u)
        return 0u;
    threshold = (uint32_t)(0u - bound) % bound;
    do {
        value = jce_rng_u32(rng);
    } while (value < threshold);
    return value % bound;
}

static uint64_t rng_bounded_u64(JceRng *rng, uint64_t bound)
{
    uint64_t threshold;
    uint64_t value;

    if (bound <= 1u)
        return 0u;
    threshold = (uint64_t)(0u - bound) % bound;
    do {
        value = jce_rng_u64(rng);
    } while (value < threshold);
    return value % bound;
}

static int32_t rng_range_i32(JceRng *rng, int32_t minimum, int32_t maximum)
{
    uint64_t width;
    uint64_t offset;

    if (maximum <= minimum)
        return minimum;
    width = (uint64_t)((int64_t)maximum - (int64_t)minimum) + 1u;
    offset = rng_bounded_u64(rng, width);
    return (int32_t)((int64_t)minimum + (int64_t)offset);
}

static bool quantize_role(const JceSceneRecipeRole *role, QuantizedRole *out)
{
    uint32_t axis;

    memset(out, 0, sizeof(*out));
    for (axis = 0u; axis < 3u; ++axis) {
        int64_t minimum;
        int64_t maximum;
        if (!jce_scene_recipe_quantize(role->center[axis], 1000,
                                       &out->center[axis]) ||
            !jce_scene_recipe_quantize(role->extent[axis], 1000,
                                       &out->extent[axis]))
            return false;
        minimum = (int64_t)out->center[axis] - out->extent[axis];
        maximum = (int64_t)out->center[axis] + out->extent[axis];
        if (minimum < INT32_MIN || maximum > INT32_MAX)
            return false;
    }
    return jce_scene_recipe_quantize(role->min_separation, 1000,
                                     &out->min_separation) &&
           jce_scene_recipe_quantize(role->yaw_min_degrees, 1000,
                                     &out->yaw_min) &&
           jce_scene_recipe_quantize(role->yaw_max_degrees, 1000,
                                     &out->yaw_max) &&
           jce_scene_recipe_quantize(role->scale_min, 1000,
                                     &out->scale_min) &&
           jce_scene_recipe_quantize(role->scale_max, 1000,
                                     &out->scale_max);
}

static void sort_role_indices(const JceSceneRecipe *recipe, uint32_t *indices)
{
    uint32_t i;

    for (i = 0u; i < recipe->role_count; ++i) {
        uint32_t j = i;
        indices[i] = i;
        while (j > 0u &&
               strcmp(recipe->roles[indices[j - 1u]].stable_role,
                      recipe->roles[indices[j]].stable_role) > 0) {
            uint32_t swap = indices[j - 1u];
            indices[j - 1u] = indices[j];
            indices[j] = swap;
            --j;
        }
    }
}

static uint32_t gather_catalog_candidates(
    const JceSceneCatalog *catalog, const char *capability,
    uint32_t *indices, uint64_t *out_total_weight)
{
    uint32_t count = 0u;
    uint32_t i;
    uint64_t total = 0u;

    for (i = 0u; i < catalog->entry_count; ++i) {
        uint32_t j;
        if (!catalog->entries[i].enabled ||
            strcmp(catalog->entries[i].capability, capability) != 0)
            continue;
        indices[count++] = i;
        total += catalog->entries[i].weight;
        j = count - 1u;
        while (j > 0u &&
               strcmp(catalog->entries[indices[j - 1u]].asset_id,
                      catalog->entries[indices[j]].asset_id) > 0) {
            uint32_t swap = indices[j - 1u];
            indices[j - 1u] = indices[j];
            indices[j] = swap;
            --j;
        }
    }
    *out_total_weight = total;
    return count;
}

static const JceSceneCatalogEntry *pick_catalog_entry(
    const JceSceneCatalog *catalog, const uint32_t *indices,
    uint32_t candidate_count, uint64_t total_weight, JceRng *rng)
{
    uint64_t choice = rng_bounded_u64(rng, total_weight);
    uint64_t cursor = 0u;
    uint32_t i;

    for (i = 0u; i < candidate_count; ++i) {
        const JceSceneCatalogEntry *entry = &catalog->entries[indices[i]];
        cursor += entry->weight;
        if (choice < cursor)
            return entry;
    }
    return &catalog->entries[indices[candidate_count - 1u]];
}

static bool position_separated(const JceSceneFrozenPlan *plan,
                               uint32_t role_start,
                               const int32_t position[3], int32_t minimum)
{
    int64_t required = (int64_t)minimum * minimum;
    uint32_t i;

    if (minimum <= 0)
        return true;
    for (i = role_start; i < plan->operation_count; ++i) {
        int64_t dx = (int64_t)position[0] -
                     plan->operations[i].position_mm[0];
        int64_t dz = (int64_t)position[2] -
                     plan->operations[i].position_mm[2];
        if (dx * dx + dz * dz < required)
            return false;
    }
    return true;
}

static bool scatter_position(const QuantizedRole *role,
                             const JceSceneFrozenPlan *plan,
                             uint32_t role_start, uint32_t max_attempts,
                             JceRng *rng, int32_t out_position[3],
                             uint32_t *out_attempts)
{
    uint32_t attempt;

    for (attempt = 1u; attempt <= max_attempts; ++attempt) {
        uint32_t axis;
        for (axis = 0u; axis < 3u; ++axis) {
            int32_t minimum = role->center[axis] - role->extent[axis];
            int32_t maximum = role->center[axis] + role->extent[axis];
            out_position[axis] = rng_range_i32(rng, minimum, maximum);
        }
        if (position_separated(plan, role_start, out_position,
                               role->min_separation)) {
            if (out_attempts)
                *out_attempts += attempt;
            return true;
        }
    }
    if (out_attempts)
        *out_attempts += max_attempts;
    return false;
}

static uint32_t ceil_square_root(uint32_t value)
{
    uint32_t root = 0u;

    while (root * root < value)
        ++root;
    return root ? root : 1u;
}

static void grid_position(const QuantizedRole *role, uint32_t index,
                          uint32_t count, int32_t out_position[3])
{
    uint32_t columns = ceil_square_root(count);
    uint32_t rows = (count + columns - 1u) / columns;
    uint32_t column = index % columns;
    uint32_t row = index / columns;
    int64_t width = (int64_t)role->extent[0] * 2;
    int64_t depth = (int64_t)role->extent[2] * 2;

    out_position[0] = columns <= 1u
        ? role->center[0]
        : (int32_t)((int64_t)role->center[0] - role->extent[0] +
                    width * column / (columns - 1u));
    out_position[1] = role->center[1];
    out_position[2] = rows <= 1u
        ? role->center[2]
        : (int32_t)((int64_t)role->center[2] - role->extent[2] +
                    depth * row / (rows - 1u));
}

/* Integer CORDIC. Angle is milli-degrees, output is Q30. */
static void cordic_sin_cos(int32_t angle_mdeg, int32_t *out_sin,
                           int32_t *out_cos)
{
    static const int32_t angles[] = {
        45000, 26565, 14036, 7125, 3576, 1790, 895, 448,
        224, 112, 56, 28, 14, 7, 4, 2
    };
    int64_t x = 652032874LL;
    int64_t y = 0;
    int32_t z;
    int32_t sign = 1;
    uint32_t i;

    while (angle_mdeg > 180000)
        angle_mdeg -= 360000;
    while (angle_mdeg < -180000)
        angle_mdeg += 360000;
    if (angle_mdeg > 90000) {
        angle_mdeg -= 180000;
        sign = -1;
    } else if (angle_mdeg < -90000) {
        angle_mdeg += 180000;
        sign = -1;
    }
    z = angle_mdeg;
    for (i = 0u; i < sizeof(angles) / sizeof(angles[0]); ++i) {
        int64_t old_x = x;
        if (z >= 0) {
            x -= y >> i;
            y += old_x >> i;
            z -= angles[i];
        } else {
            x += y >> i;
            y -= old_x >> i;
            z += angles[i];
        }
    }
    *out_cos = (int32_t)(x * sign);
    *out_sin = (int32_t)(y * sign);
}

static void ring_position(const QuantizedRole *role, uint32_t index,
                          uint32_t count, int32_t out_position[3])
{
    int32_t sine;
    int32_t cosine;
    int32_t angle = count ? (int32_t)((360000LL * index) / count) : 0;
    int32_t radius = role->extent[0] < role->extent[2]
        ? role->extent[0] : role->extent[2];

    if (radius <= 0)
        radius = role->extent[0] > role->extent[2]
            ? role->extent[0] : role->extent[2];
    cordic_sin_cos(angle, &sine, &cosine);
    out_position[0] = role->center[0] +
        (int32_t)(((int64_t)radius * cosine) >> 30);
    out_position[1] = role->center[1];
    out_position[2] = role->center[2] +
        (int32_t)(((int64_t)radius * sine) >> 30);
}

static void operation_set_transform(JceScenePlanOperation *operation,
                                    const QuantizedRole *role,
                                    const int32_t position[3], JceRng *rng)
{
    int32_t yaw = rng_range_i32(rng, role->yaw_min, role->yaw_max);
    int32_t scale = rng_range_i32(rng, role->scale_min, role->scale_max);

    memcpy(operation->position_mm, position, sizeof(operation->position_mm));
    operation->rotation_mdeg[0] = 0;
    operation->rotation_mdeg[1] = yaw;
    operation->rotation_mdeg[2] = 0;
    operation->scale_milli[0] = scale;
    operation->scale_milli[1] = scale;
    operation->scale_milli[2] = scale;
}

static void sort_operations(JceSceneFrozenPlan *plan)
{
    uint32_t i;

    for (i = 1u; i < plan->operation_count; ++i) {
        JceScenePlanOperation value = plan->operations[i];
        uint32_t j = i;
        while (j > 0u &&
               plan->operations[j - 1u].stable_entity_id >
                   value.stable_entity_id) {
            plan->operations[j] = plan->operations[j - 1u];
            --j;
        }
        plan->operations[j] = value;
    }
}

static int32_t operation_index_by_id(const JceSceneFrozenPlan *plan,
                                     uint64_t stable_entity_id)
{
    uint32_t low = 0u;
    uint32_t high = plan->operation_count;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint64_t current = plan->operations[middle].stable_entity_id;

        if (current < stable_entity_id)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low < plan->operation_count &&
        plan->operations[low].stable_entity_id == stable_entity_id)
        return (int32_t)low;
    return -1;
}

static bool frozen_plan_topological_order(
    const JceSceneFrozenPlan *plan, uint32_t *out_indices,
    uint32_t capacity)
{
    bool emitted[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS] = { false };
    uint32_t emitted_count = 0u;
    uint32_t i;

    if (!plan ||
        plan->format_version != JCE_SCENE_FROZEN_PLAN_FORMAT_VERSION ||
        plan->compiler_version != JCE_SCENE_COMPILER_VERSION ||
        plan->operation_count > JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS ||
        (out_indices && capacity < plan->operation_count))
        return false;

    for (i = 0u; i < plan->operation_count; ++i) {
        const JceScenePlanOperation *operation = &plan->operations[i];

        if (operation->stable_entity_id == 0u ||
            operation->parent_stable_entity_id ==
                operation->stable_entity_id ||
            (i > 0u && plan->operations[i - 1u].stable_entity_id >=
                         operation->stable_entity_id) ||
            (operation->parent_stable_entity_id != 0u &&
             operation_index_by_id(plan,
                                   operation->parent_stable_entity_id) < 0))
            return false;
    }

    while (emitted_count < plan->operation_count) {
        uint32_t candidate = UINT32_MAX;

        for (i = 0u; i < plan->operation_count; ++i) {
            const JceScenePlanOperation *operation;
            int32_t parent_index;

            if (emitted[i])
                continue;
            operation = &plan->operations[i];
            if (operation->parent_stable_entity_id == 0u) {
                candidate = i;
                break;
            }
            parent_index = operation_index_by_id(
                plan, operation->parent_stable_entity_id);
            if (parent_index >= 0 && emitted[parent_index]) {
                candidate = i;
                break;
            }
        }
        if (candidate == UINT32_MAX)
            return false;
        emitted[candidate] = true;
        if (out_indices)
            out_indices[emitted_count] = candidate;
        ++emitted_count;
    }
    return true;
}

static const JceSceneRecipeRole *recipe_role_by_name(
    const JceSceneRecipe *recipe, const char *stable_role)
{
    uint32_t i;

    for (i = 0u; i < recipe->role_count; ++i) {
        if (strcmp(recipe->roles[i].stable_role, stable_role) == 0)
            return &recipe->roles[i];
    }
    return NULL;
}

static bool resolve_parent_references(const JceSceneRecipe *recipe,
                                      JceSceneFrozenPlan *plan)
{
    uint32_t i;

    for (i = 0u; i < plan->operation_count; ++i) {
        JceScenePlanOperation *operation = &plan->operations[i];
        const JceSceneRecipeRole *role = recipe_role_by_name(
            recipe, operation->stable_role);

        if (!role)
            return false;
        operation->parent_stable_entity_id = role->parent_role[0] == '\0'
            ? 0u
            : stable_entity_id(role->parent_role, role->parent_instance);
    }
    return frozen_plan_topological_order(plan, NULL, 0u);
}

JCE_API void JCE_CALL
jce_scene_compile_options_default(JceSceneCompileOptions *options)
{
    if (!options)
        return;
    options->max_placement_attempts = JCE_DEFAULT_PLACEMENT_ATTEMPTS;
}

JCE_API JceSceneCompileStatus JCE_CALL
jce_scene_compile(const JceSceneRecipe *recipe,
                  const JceSceneCatalog *catalog,
                  const JceSceneCompileOptions *options,
                  JceSceneFrozenPlan *out_plan,
                  JceSceneCompileError *error)
{
    JceSceneRecipeError contract_error;
    JceSceneCompileOptions effective;
    uint32_t role_order[JCE_SCENE_RECIPE_MAX_ROLES];
    uint32_t role_cursor;
    JceSceneRecipeStatus recipe_status;

    compile_error_clear(error);
    if (out_plan)
        memset(out_plan, 0, sizeof(*out_plan));
    if (!recipe || !catalog || !out_plan)
        return compile_error_set(error, JCE_SCENE_COMPILE_INVALID_ARGUMENT,
                                 UINT32_MAX, 0u, NULL);

    recipe_status = jce_scene_recipe_validate(recipe, &contract_error);
    if (recipe_status != JCE_SCENE_RECIPE_OK)
        return compile_error_set(error, JCE_SCENE_COMPILE_INVALID_RECIPE,
                                 contract_error.item_index, 0u,
                                 &contract_error);
    recipe_status = jce_scene_catalog_validate(catalog, &contract_error);
    if (recipe_status != JCE_SCENE_RECIPE_OK)
        return compile_error_set(error, JCE_SCENE_COMPILE_INVALID_CATALOG,
                                 contract_error.item_index, 0u,
                                 &contract_error);

    jce_scene_compile_options_default(&effective);
    if (options)
        effective = *options;
    if (effective.max_placement_attempts == 0u)
        effective.max_placement_attempts = 1u;
    if (effective.max_placement_attempts > 4096u)
        effective.max_placement_attempts = 4096u;

    out_plan->format_version = JCE_SCENE_FROZEN_PLAN_FORMAT_VERSION;
    out_plan->compiler_version = recipe->compiler_version;
    out_plan->request_id = recipe->request_id;
    out_plan->seed = recipe->seed;
    out_plan->catalog_hash = catalog->content_hash;

    sort_role_indices(recipe, role_order);

    for (role_cursor = 0u; role_cursor < recipe->role_count; ++role_cursor) {
        uint32_t source_index = role_order[role_cursor];
        const JceSceneRecipeRole *role = &recipe->roles[source_index];
        uint32_t candidates[JCE_SCENE_CATALOG_MAX_ENTRIES];
        uint64_t total_weight = 0u;
        uint32_t candidate_count;
        uint32_t count;
        uint32_t instance;
        uint32_t role_start = out_plan->operation_count;
        QuantizedRole quantized;
        bool scatter_failed = false;
        uint32_t attempts = 0u;
        JceRng role_rng;

        candidate_count = gather_catalog_candidates(catalog, role->capability,
                                                     candidates,
                                                     &total_weight);
        if (candidate_count == 0u || total_weight == 0u)
            return compile_error_set(error,
                                     JCE_SCENE_COMPILE_UNKNOWN_CAPABILITY,
                                     source_index, 0u, NULL);
        if (!quantize_role(role, &quantized))
            return compile_error_set(error, JCE_SCENE_COMPILE_INVALID_RECIPE,
                                     source_index, 0u, NULL);
        seed_role_rng(&role_rng, recipe, catalog, role, &quantized);

        count = role->min_count;
        if (role->max_count > role->min_count) {
            count += rng_bounded_u32(&role_rng,
                                     role->max_count - role->min_count + 1u);
        }
        if (out_plan->operation_count + count >
            JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS)
            return compile_error_set(error,
                                     JCE_SCENE_COMPILE_OPERATION_CAPACITY,
                                     source_index, 0u, NULL);

        for (instance = 0u; instance < count; ++instance) {
            JceScenePlanOperation *operation =
                &out_plan->operations[out_plan->operation_count];
            const JceSceneCatalogEntry *entry;
            int32_t position[3];
            bool placed = true;

            memset(operation, 0, sizeof(*operation));
            if (role->placement == JCE_SCENE_PLACEMENT_SINGLE)
                memcpy(position, quantized.center, sizeof(position));
            else if (role->placement == JCE_SCENE_PLACEMENT_GRID)
                grid_position(&quantized, instance, count, position);
            else if (role->placement == JCE_SCENE_PLACEMENT_RING)
                ring_position(&quantized, instance, count, position);
            else
                placed = scatter_position(&quantized, out_plan, role_start,
                                           effective.max_placement_attempts,
                                           &role_rng, position, &attempts);
            if (!placed) {
                scatter_failed = true;
                break;
            }

            entry = pick_catalog_entry(catalog, candidates, candidate_count,
                                       total_weight, &role_rng);
            operation->stable_entity_id =
                stable_entity_id(role->stable_role, instance);
            operation->asset_content_hash = entry->content_hash;
            copy_text(operation->stable_role,
                      sizeof(operation->stable_role), role->stable_role);
            copy_text(operation->asset_id, sizeof(operation->asset_id),
                      entry->asset_id);
            operation_set_transform(operation, &quantized, position,
                                    &role_rng);
            ++out_plan->operation_count;
        }

        if (scatter_failed) {
            out_plan->operation_count = role_start;
            for (instance = 0u; instance < count; ++instance) {
                JceScenePlanOperation *operation =
                    &out_plan->operations[out_plan->operation_count];
                const JceSceneCatalogEntry *entry;
                int32_t position[3];

                grid_position(&quantized, instance, count, position);
                if (!position_separated(out_plan, role_start, position,
                                        quantized.min_separation))
                    return compile_error_set(
                        error, JCE_SCENE_COMPILE_PLACEMENT_FAILED,
                        source_index, attempts, NULL);
                memset(operation, 0, sizeof(*operation));
                entry = pick_catalog_entry(catalog, candidates,
                                           candidate_count, total_weight,
                                           &role_rng);
                operation->stable_entity_id =
                    stable_entity_id(role->stable_role, instance);
                operation->asset_content_hash = entry->content_hash;
                copy_text(operation->stable_role,
                          sizeof(operation->stable_role), role->stable_role);
                copy_text(operation->asset_id, sizeof(operation->asset_id),
                          entry->asset_id);
                operation_set_transform(operation, &quantized, position,
                                        &role_rng);
                ++out_plan->operation_count;
            }
        }
    }

    sort_operations(out_plan);
    if (!resolve_parent_references(recipe, out_plan)) {
        memset(out_plan, 0, sizeof(*out_plan));
        return compile_error_set(error,
                                 JCE_SCENE_COMPILE_INVALID_HIERARCHY,
                                 UINT32_MAX, 0u, NULL);
    }
    out_plan->plan_hash = jce_scene_frozen_plan_hash(out_plan);
    if (out_plan->plan_hash == 0u) {
        memset(out_plan, 0, sizeof(*out_plan));
        return compile_error_set(error, JCE_SCENE_COMPILE_INVALID_RECIPE,
                                 UINT32_MAX, 0u, NULL);
    }
    return JCE_SCENE_COMPILE_OK;
}

JCE_API uint64_t JCE_CALL
jce_scene_frozen_plan_hash(const JceSceneFrozenPlan *plan)
{
    uint64_t hash = JCE_FNV64_OFFSET;
    uint32_t i;

    if (!frozen_plan_topological_order(plan, NULL, 0u))
        return 0u;
    hash = hash_u32(hash, plan->format_version);
    hash = hash_u32(hash, plan->compiler_version);
    hash = hash_u64(hash, plan->request_id);
    hash = hash_u64(hash, plan->seed);
    hash = hash_u64(hash, plan->catalog_hash);
    hash = hash_u32(hash, plan->operation_count);
    for (i = 0u; i < plan->operation_count; ++i) {
        const JceScenePlanOperation *operation = &plan->operations[i];
        uint32_t axis;

        hash = hash_u64(hash, operation->stable_entity_id);
        hash = hash_u64(hash, operation->parent_stable_entity_id);
        hash = hash_u64(hash, operation->asset_content_hash);
        hash = hash_text(hash, operation->stable_role,
                         sizeof(operation->stable_role));
        if (hash == 0u)
            return 0u;
        hash = hash_text(hash, operation->asset_id,
                         sizeof(operation->asset_id));
        if (hash == 0u)
            return 0u;
        for (axis = 0u; axis < 3u; ++axis)
            hash = hash_i32(hash, operation->position_mm[axis]);
        for (axis = 0u; axis < 3u; ++axis)
            hash = hash_i32(hash, operation->rotation_mdeg[axis]);
        for (axis = 0u; axis < 3u; ++axis)
            hash = hash_i32(hash, operation->scale_milli[axis]);
    }
    return hash ? hash : 1u;
}

JCE_API bool JCE_CALL
jce_scene_frozen_plan_graph_validate(const JceSceneFrozenPlan *plan)
{
    return frozen_plan_topological_order(plan, NULL, 0u);
}

JCE_API uint32_t JCE_CALL
jce_scene_frozen_plan_topological_order(const JceSceneFrozenPlan *plan,
                                        uint32_t *out_indices,
                                        uint32_t capacity)
{
    if (!out_indices ||
        !frozen_plan_topological_order(plan, out_indices, capacity))
        return 0u;
    return plan->operation_count;
}

static size_t frozen_plan_size(const JceSceneFrozenPlan *plan)
{
    size_t size = 48u;
    uint32_t i;

    if (!plan || plan->operation_count >
                     JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS)
        return 0u;
    for (i = 0u; i < plan->operation_count; ++i) {
        size_t role_length = bounded_length(plan->operations[i].stable_role,
                                            JCE_SCENE_STABLE_ROLE_MAX);
        size_t asset_length = bounded_length(plan->operations[i].asset_id,
                                             JCE_SCENE_ASSET_ID_MAX);
        if (role_length == 0u || role_length >= JCE_SCENE_STABLE_ROLE_MAX ||
            asset_length == 0u || asset_length >= JCE_SCENE_ASSET_ID_MAX)
            return 0u;
        size += 64u + role_length + asset_length;
        if (size > JCE_SCENE_FROZEN_PLAN_MAX_BYTES)
            return 0u;
    }
    return size;
}

static void writer_bytes(PlanWriter *writer, const void *bytes, size_t size)
{
    if (!writer->ok || size > writer->capacity - writer->offset) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->offset, bytes, size);
    writer->offset += size;
}

static void writer_u16(PlanWriter *writer, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xffu);
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u32(PlanWriter *writer, uint32_t value)
{
    uint8_t bytes[4];
    uint32_t i;
    for (i = 0u; i < 4u; ++i)
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u64(PlanWriter *writer, uint64_t value)
{
    uint8_t bytes[8];
    uint32_t i;
    for (i = 0u; i < 8u; ++i)
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    writer_bytes(writer, bytes, sizeof(bytes));
}

JCE_API bool JCE_CALL
jce_scene_frozen_plan_write(const JceSceneFrozenPlan *plan,
                            void *buffer, size_t capacity,
                            size_t *out_size)
{
    PlanWriter writer;
    size_t required = frozen_plan_size(plan);
    uint32_t i;

    if (out_size)
        *out_size = required;
    if (required == 0u || !plan ||
        plan->plan_hash != jce_scene_frozen_plan_hash(plan))
        return false;
    if (!buffer)
        return capacity == 0u;
    if (capacity < required)
        return false;

    writer.data = (uint8_t *)buffer;
    writer.capacity = capacity;
    writer.offset = 0u;
    writer.ok = true;
    writer_u32(&writer, JCE_PLAN_MAGIC);
    writer_u32(&writer, plan->format_version);
    writer_u32(&writer, plan->compiler_version);
    writer_u32(&writer, plan->operation_count);
    writer_u64(&writer, plan->request_id);
    writer_u64(&writer, plan->seed);
    writer_u64(&writer, plan->catalog_hash);
    writer_u64(&writer, plan->plan_hash);
    for (i = 0u; i < plan->operation_count; ++i) {
        const JceScenePlanOperation *operation = &plan->operations[i];
        uint16_t role_length = (uint16_t)bounded_length(
            operation->stable_role, sizeof(operation->stable_role));
        uint16_t asset_length = (uint16_t)bounded_length(
            operation->asset_id, sizeof(operation->asset_id));
        uint32_t axis;

        writer_u64(&writer, operation->stable_entity_id);
        writer_u64(&writer, operation->parent_stable_entity_id);
        writer_u64(&writer, operation->asset_content_hash);
        writer_u16(&writer, role_length);
        writer_u16(&writer, asset_length);
        for (axis = 0u; axis < 3u; ++axis)
            writer_u32(&writer, (uint32_t)operation->position_mm[axis]);
        for (axis = 0u; axis < 3u; ++axis)
            writer_u32(&writer, (uint32_t)operation->rotation_mdeg[axis]);
        for (axis = 0u; axis < 3u; ++axis)
            writer_u32(&writer, (uint32_t)operation->scale_milli[axis]);
        writer_bytes(&writer, operation->stable_role, role_length);
        writer_bytes(&writer, operation->asset_id, asset_length);
    }
    if (out_size)
        *out_size = writer.offset;
    return writer.ok && writer.offset == required;
}

static void reader_bytes(PlanReader *reader, void *out, size_t size)
{
    if (!reader->ok || size > reader->size - reader->offset) {
        reader->ok = false;
        return;
    }
    memcpy(out, reader->data + reader->offset, size);
    reader->offset += size;
}

static uint16_t reader_u16(PlanReader *reader)
{
    uint8_t bytes[2] = { 0u, 0u };
    reader_bytes(reader, bytes, sizeof(bytes));
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static uint32_t reader_u32(PlanReader *reader)
{
    uint8_t bytes[4] = { 0u, 0u, 0u, 0u };
    uint32_t value = 0u;
    uint32_t i;
    reader_bytes(reader, bytes, sizeof(bytes));
    for (i = 0u; i < 4u; ++i)
        value |= (uint32_t)bytes[i] << (i * 8u);
    return value;
}

static uint64_t reader_u64(PlanReader *reader)
{
    uint8_t bytes[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    uint64_t value = 0u;
    uint32_t i;
    reader_bytes(reader, bytes, sizeof(bytes));
    for (i = 0u; i < 8u; ++i)
        value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

JCE_API bool JCE_CALL
jce_scene_frozen_plan_read(const void *buffer, size_t size,
                           JceSceneFrozenPlan *out_plan)
{
    PlanReader reader;
    JceSceneFrozenPlan parsed;
    uint32_t magic;
    uint32_t i;

    if (!buffer || !out_plan || size < 48u ||
        size > JCE_SCENE_FROZEN_PLAN_MAX_BYTES)
        return false;
    memset(&parsed, 0, sizeof(parsed));
    reader.data = (const uint8_t *)buffer;
    reader.size = size;
    reader.offset = 0u;
    reader.ok = true;

    magic = reader_u32(&reader);
    parsed.format_version = reader_u32(&reader);
    parsed.compiler_version = reader_u32(&reader);
    parsed.operation_count = reader_u32(&reader);
    parsed.request_id = reader_u64(&reader);
    parsed.seed = reader_u64(&reader);
    parsed.catalog_hash = reader_u64(&reader);
    parsed.plan_hash = reader_u64(&reader);
    if (!reader.ok || magic != JCE_PLAN_MAGIC ||
        parsed.format_version != JCE_SCENE_FROZEN_PLAN_FORMAT_VERSION ||
        parsed.compiler_version != JCE_SCENE_COMPILER_VERSION ||
        parsed.operation_count > JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS)
        return false;

    for (i = 0u; i < parsed.operation_count; ++i) {
        JceScenePlanOperation *operation = &parsed.operations[i];
        uint16_t role_length;
        uint16_t asset_length;
        uint32_t axis;

        operation->stable_entity_id = reader_u64(&reader);
        operation->parent_stable_entity_id = reader_u64(&reader);
        operation->asset_content_hash = reader_u64(&reader);
        role_length = reader_u16(&reader);
        asset_length = reader_u16(&reader);
        for (axis = 0u; axis < 3u; ++axis)
            operation->position_mm[axis] = (int32_t)reader_u32(&reader);
        for (axis = 0u; axis < 3u; ++axis)
            operation->rotation_mdeg[axis] = (int32_t)reader_u32(&reader);
        for (axis = 0u; axis < 3u; ++axis)
            operation->scale_milli[axis] = (int32_t)reader_u32(&reader);
        if (!reader.ok || role_length == 0u ||
            role_length >= sizeof(operation->stable_role) ||
            asset_length == 0u ||
            asset_length >= sizeof(operation->asset_id))
            return false;
        reader_bytes(&reader, operation->stable_role, role_length);
        reader_bytes(&reader, operation->asset_id, asset_length);
        operation->stable_role[role_length] = '\0';
        operation->asset_id[asset_length] = '\0';
        if (!reader.ok || operation->stable_entity_id == 0u ||
            operation->asset_content_hash == 0u ||
            operation->scale_milli[0] <= 0 ||
            operation->scale_milli[1] <= 0 ||
            operation->scale_milli[2] <= 0 ||
            (i > 0u && parsed.operations[i - 1u].stable_entity_id >=
                         operation->stable_entity_id))
            return false;
    }
    if (!reader.ok || reader.offset != size ||
        !jce_scene_frozen_plan_graph_validate(&parsed) ||
        parsed.plan_hash != jce_scene_frozen_plan_hash(&parsed))
        return false;
    *out_plan = parsed;
    return true;
}

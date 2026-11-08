/* Renderer-owned startup shader root. Included after the opaque instance.
 * The device borrows only its admitted instance; callers retain program refs. */
bool jce_renderer_shader_dev_bind_internal(JceRenderer *r, const char *root)
{
    if (!r || !jce_thread_is_main() || s_device.owner != r ||
        !jce_renderer_frame_admission_current_internal(r) || r->is_fallback ||
        r->shader_dev.bound) return false;
    char *copy = NULL;
    if (root && root[0]) {
        const size_t size = strlen(root) + 1u;
        copy = (char *)JCE_MALLOC(size);
        if (!copy) return false;
        memcpy(copy, root, size);
    }
    r->shader_dev.root = copy;
    r->shader_dev.bound = true;
    return true;
}

static void renderer_shader_dev_release(JceRenderer *r)
{
    if (!r) return;
    if (s_device.owner == r) s_device.owner = NULL;
    if (r->shader_dev.root) JCE_FREE(r->shader_dev.root);
    r->shader_dev.root = NULL;
    r->shader_dev.bound = false;
}

JceShaderHandle jce_renderer_shader_load_named_internal(const JcePakArchive *pak,
    const char *vs_base, const char *fs_base)
{
    if (!jce_thread_is_main()) return JCE_INVALID_SHADER;
    const JceRenderer *r = s_device.owner;
    if (r && !jce_renderer_frame_admission_current_internal(r))
        return JCE_INVALID_SHADER;
    const char *root = r && r->shader_dev.bound ? r->shader_dev.root : NULL;
    if (root) return shader_load_program_overlay_named(root, pak, vs_base, fs_base);
    /* No injected owner/root: retain the existing initialized-backend PAK
     * precondition rather than borrow a previous or foreign instance's root. */
    return shader_load_program_named(pak, vs_base, fs_base);
}

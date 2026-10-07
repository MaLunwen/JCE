# Shared offline asset implementations; no renderer/scene layer target.
function(jce_host_asset_sources target)
	target_sources(${target} PRIVATE
		"${JCE_ENGINE_SRC_DIR}/resource/jce_pak_loader.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_writer.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_reader.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_path.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_dict.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_crypto.c"
		"${JCE_TOOLS_DIR}/jce_tex_compress.c"
		"${JCE_TOOLS_DIR}/jce_tex_encode.cpp"
		"${JCE_TOOLS_DIR}/miniaudio_impl_cook.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_atlas_pack.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_cooker.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_writer_internal.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_ext.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_filesystem_host.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_memory.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_alloc.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_model_importer.cpp"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_model_import_settings.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_audio_import_settings.c"
		"${JCE_ENGINE_SRC_DIR}/middleware/physics/jce_collider_cook.cpp"
		"${JCE_ENGINE_SRC_DIR}/middleware/physics/jce_collider_asset.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_bundle_mesh_convert.cpp"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_mesh_lod_cook.c"
		"${JCE_ENGINE_SRC_DIR}/middleware/scene/jce_terrain.c"
		"${JCE_ENGINE_SRC_DIR}/middleware/scene/jce_phacelle.c"
		"${JCE_ENGINE_SRC_DIR}/middleware/scene/jce_horizon.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_json.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_path.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_terrain_format.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_image_decode.c"
		"${JCE_ENGINE_SRC_DIR}/renderer/jce_image.c"
		"${JCE_ENGINE_SRC_DIR}/renderer/jce_stb_image_impl.c"
)
	target_include_directories(${target} PRIVATE
		"${PROJECT_SOURCE_DIR}/engine/include" "${JCE_ENGINE_SRC_DIR}")
	target_link_libraries(${target} PRIVATE
		jce_core jce_platform SDL3_image::SDL3_image sdl::sdl miniaudio::miniaudio
		mimalloc-static zstd::libzstd_static xxHash::xxhash
		bgfx::bimg_encode bgfx::bimg bgfx::bx
		assimp::assimp v-hacd::v-hacd meshoptimizer::meshoptimizer cjson::cjson cgltf::cgltf)
	target_compile_definitions(${target} PRIVATE
		JCE_LOG_NONE JCE_COLLIDER_ASSET_NO_PHYSICS JCE_MODEL_IMPORTER_COOK_ONLY)
endfunction()

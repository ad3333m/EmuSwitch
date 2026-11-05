list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMakeModules")

include(GenerateBuildInfo)
generate_build_info()

# The variable SRC_DIR must be passed into the script (since it uses the current build directory for all values of CMAKE_*_DIR)
set(VIDEO_CORE "${CMAKE_SOURCE_DIR}/src/video_core")
# The files that shape the code of the shaders the disk caches keep. When one changes, every
# player's cache is regenerated, which on the Switch makes the next start of every game compile
# all of its shaders again, so only those files are listed. The Vulkan pipeline cache and disk
# cache code is left out: the cache files have their own format versions (CACHE_FILE_VERSION and
# each entry's EXPECTED_VERSION, bump them when the layout changes) and keep the shader profile
# they were made with, which is compared when they are read; vk_shader_config.cpp holds the part
# of the pipeline cache that goes into the shaders' code.
set(HASH_FILES
    "${VIDEO_CORE}/renderer_opengl/gl_shader_disk_cache.cpp"
    "${VIDEO_CORE}/renderer_opengl/gl_shader_disk_cache.h"
    "${VIDEO_CORE}/renderer_opengl/gl_shader_util.cpp"
    "${VIDEO_CORE}/renderer_opengl/gl_shader_util.h"
    "${VIDEO_CORE}/renderer_vulkan/vk_shader_config.cpp"
    "${VIDEO_CORE}/renderer_vulkan/vk_shader_util.cpp"
    "${VIDEO_CORE}/renderer_vulkan/vk_shader_util.h"
    "${VIDEO_CORE}/shader/generator/glsl_fs_shader_gen.cpp"
    "${VIDEO_CORE}/shader/generator/glsl_fs_shader_gen.h"
    "${VIDEO_CORE}/shader/generator/glsl_shader_decompiler.cpp"
    "${VIDEO_CORE}/shader/generator/glsl_shader_decompiler.h"
    "${VIDEO_CORE}/shader/generator/glsl_shader_gen.cpp"
    "${VIDEO_CORE}/shader/generator/glsl_shader_gen.h"
    "${VIDEO_CORE}/shader/generator/pica_fs_config.cpp"
    "${VIDEO_CORE}/shader/generator/pica_fs_config.h"
    "${VIDEO_CORE}/shader/generator/profile.h"
    "${VIDEO_CORE}/shader/generator/shader_gen.cpp"
    "${VIDEO_CORE}/shader/generator/shader_gen.h"
    "${VIDEO_CORE}/shader/generator/shader_uniforms.cpp"
    "${VIDEO_CORE}/shader/generator/shader_uniforms.h"
    "${VIDEO_CORE}/shader/generator/spv_fs_shader_gen.cpp"
    "${VIDEO_CORE}/shader/generator/spv_fs_shader_gen.h"
    "${VIDEO_CORE}/shader/shader.cpp"
    "${VIDEO_CORE}/shader/shader.h"
    "${VIDEO_CORE}/pica/regs_framebuffer.h"
    "${VIDEO_CORE}/pica/regs_lighting.h"
    "${VIDEO_CORE}/pica/regs_pipeline.h"
    "${VIDEO_CORE}/pica/regs_rasterizer.h"
    "${VIDEO_CORE}/pica/regs_shader.h"
    "${VIDEO_CORE}/pica/regs_texturing.h"
    "${VIDEO_CORE}/pica/regs_internal.cpp"
    "${VIDEO_CORE}/pica/regs_internal.h"
)
set(COMBINED "")
foreach (F IN LISTS HASH_FILES)
    file(READ ${F} TMP)
    set(COMBINED "${COMBINED}${TMP}")
endforeach()
string(MD5 SHADER_CACHE_VERSION "${COMBINED}")
configure_file("${CMAKE_SOURCE_DIR}/src/common/scm_rev.cpp.in" "scm_rev.cpp" @ONLY)

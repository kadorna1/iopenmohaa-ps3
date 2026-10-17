include_guard(GLOBAL)

set(RENDERER_COMMON_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_font.c
    ${SOURCE_DIR}/renderercommon/tr_image_bmp.c
    ${SOURCE_DIR}/renderercommon/tr_image_jpg.c
    ${SOURCE_DIR}/renderercommon/tr_image_pcx.c
    ${SOURCE_DIR}/renderercommon/tr_image_png.c
    ${SOURCE_DIR}/renderercommon/tr_image_pvr.c
    ${SOURCE_DIR}/renderercommon/tr_image_tga.c
    ${SOURCE_DIR}/renderercommon/tr_noise.c
    ${SOURCE_DIR}/renderercommon/puff.c
	${SOURCE_DIR}/tiki/tiki_mesh.cpp
)

set(SDL_RENDERER_SOURCES
    ${SOURCE_DIR}/sdl/sdl_gamma.c
    ${SOURCE_DIR}/sdl/sdl_glimp.c
)

if(CMAKE_SYSTEM_NAME STREQUAL "PS3")
    # PS3 uses the GCM/RSX implementation instead of SDL's OpenGL windowing.
    set(SDL_RENDERER_SOURCES
        ${SOURCE_DIR}/sys/ps3_glimp.c
        ${SOURCE_DIR}/renderergl1/qgl_ps3.c
        ${SOURCE_DIR}/gl/ps3gl_main.c
        ${SOURCE_DIR}/gl/ps3gl_states.c
        ${SOURCE_DIR}/gl/ps3gl_matrices.c
        ${SOURCE_DIR}/gl/ps3gl_textures.c
        ${SOURCE_DIR}/gl/ps3gl_draw.c
        ${SOURCE_DIR}/gl/ps3gl_vertices.c
        ${SOURCE_DIR}/gl/ps3gl_colors.c
        ${SOURCE_DIR}/gl/ps3gl_shaders.c
    )
endif()

set(DYNAMIC_RENDERER_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_subs.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/corepp/str.cpp
)

if(USE_FREETYPE)
    list(APPEND RENDERER_DEFINITIONS BUILD_FREETYPE)
endif()

if(USE_RENDERER_DLOPEN)
    list(APPEND RENDERER_DEFINITIONS USE_RENDERER_DLOPEN)
    list(APPEND RENDERER_DEFINITIONS REF_DLL=1)
elseif(BUILD_RENDERER_GL1 AND BUILD_RENDERER_GL2)
    message(FATAL_ERROR "Multiple static renderers enabled; choose one")
elseif(NOT BUILD_RENDERER_GL1 AND NOT BUILD_RENDERER_GL2)
    message(FATAL_ERROR "Zero static renderers enabled; choose one")
endif()

list(APPEND RENDERER_LIBRARIES ${COMMON_LIBRARIES})


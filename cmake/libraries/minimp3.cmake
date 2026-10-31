if(NOT USE_CODEC_MINIMP3)
    return()
endif()

if(NOT BUILD_CLIENT)
    return()
endif()

# minimp3 is a single-header, public-domain MP3 decoder. The implementation
# is compiled once from snd_codec_mp3.c; no external package or library is
# downloaded, configured, or linked.
list(APPEND CLIENT_INCLUDE_DIRS \${SOURCE_DIR}/thirdparty/minimp3)
list(APPEND CLIENT_DEFINITIONS USE_CODEC_MINIMP3=1)

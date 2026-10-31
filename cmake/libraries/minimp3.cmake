if(NOT USE_CODEC_MINIMP3)
    return()
endif()

if(NOT BUILD_CLIENT)
    return()
endif()

# minimp3 is vendored as a single-header, public-domain MP3 decoder.
# The implementation is compiled once by snd_codec_mp3.c.
list(APPEND CLIENT_INCLUDE_DIRS ${SOURCE_DIR}/thirdparty/minimp3)
list(APPEND CLIENT_DEFINITIONS USE_CODEC_MINIMP3=1)

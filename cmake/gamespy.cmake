# The OpenMoHAA PS3 port keeps its protocol/master-list code in
# gamespy/q_gamespy.c and gamespy/cl_gamespy.c, but the legacy GCD/QR2
# library pulls in netex/libnetctl headers that are not part of PSL1GHT.
# Do not create/link that legacy SDK on PS3.
if(CMAKE_SYSTEM_NAME STREQUAL "PS3")
    return()
endif()

include_directories(${SOURCE_DIR}/gamespy/common)
include_directories(${SOURCE_DIR}/gamespy/gcdkey)

file(GLOB SRCS_common
	${SOURCE_DIR}/gamespy/common/*.c
    ${SOURCE_DIR}/gamespy/darray.c
    ${SOURCE_DIR}/gamespy/gserver.c
    ${SOURCE_DIR}/gamespy/gserverlist.c
	${SOURCE_DIR}/gamespy/hashtable.c
	${SOURCE_DIR}/gamespy/md5c.c
	${SOURCE_DIR}/gamespy/gutil.c
)

file(GLOB SRCS_gcdkey
    ${SOURCE_DIR}/gamespy/gcdkey/*.c
)

file(GLOB SRCS_natneg
    ${SOURCE_DIR}/gamespy/natneg/*.c
)

file(GLOB SRCS_qr2
    ${SOURCE_DIR}/gamespy/qr2/*.c
)

add_library(gcd_common STATIC ${SRCS_common})
add_library(gcd_key STATIC ${SRCS_gcdkey})
add_library(gcd_natneg STATIC ${SRCS_natneg})
add_library(gcd_qr2 STATIC ${SRCS_qr2})
add_library(gcd INTERFACE)

set_property(TARGET gcd_common gcd_key PROPERTY POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_NAME STREQUAL "PS3")
    add_definitions(-D_PS3=1)
elseif(UNIX)
    add_definitions(-D_LINUX=1)
endif()

set(DEPENDENT_LIBS
	gcd_key
#	gcd_gp
#	gcd_gstats
	gcd_natneg
#	gcd_pinger
#	gcd_pt
	gcd_qr2
#	gcd_sake
#	gcd_sc
#	gcd_serverbrowsing
#	gcd_webservices
)

target_link_libraries(gcd INTERFACE ${DEPENDENT_LIBS})

foreach(LIB ${DEPENDENT_LIBS})
	target_link_libraries(${LIB} PUBLIC gcd_common)
endforeach()

target_link_libraries(gcd_key PRIVATE gcd_qr2)
target_link_libraries(gcd_qr2 PRIVATE gcd_natneg)

list(APPEND CLIENT_LIBRARIES gcd)
list(APPEND SERVER_LIBRARIES gcd)

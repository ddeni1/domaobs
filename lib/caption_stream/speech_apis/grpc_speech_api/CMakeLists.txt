project(caption_stream)

set(USE_PKG_CONFIG_GRPC "" CACHE BOOL "Use pkgconfig to automatically find grpc and ")
set(GOOGLEAPIS_DIR "" CACHE STRING "Path containing grpc generated googleapis files")
set(YANDEX_CLOUDAPI_DIR "" CACHE STRING "Path containing grpc generated yandex cloudapi files")
set(USE_OS_CERTS "" CACHE STRING "let GRPC use the OS cert store")
set(GRPC_CMAKE_INCLUDE "" CACHE STRING "cmake toolchain file to use for finding GRPC")

if (USE_PKG_CONFIG_GRPC)
    message("using pkgconfig")
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(Protobuf REQUIRED protobuf)
    pkg_check_modules(GRPCPP REQUIRED grpc++)
    message("FOUND gRPC!")

    SET(GRPC_NAME grpc)
else ()
    message("using find_package")

    if (GRPC_CMAKE_INCLUDE)
        message("including GRPC_CMAKE_INCLUDE: ${GRPC_CMAKE_INCLUDE}")
        include(${GRPC_CMAKE_INCLUDE})
    endif ()

    find_package(gRPC CONFIG REQUIRED)
    find_package(Protobuf CONFIG REQUIRED)
    message("FOUND gRPC!")

    SET(GRPC_NAME gRPC::grpc++)
endif ()

if (WIN32 AND NOT BUILD_SHARED_LIBS)
    message(BUILD_SHARED_LIBS: setting win flags)
    foreach (flag_var
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)

        if (${flag_var} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        endif ()
    endforeach ()
endif ()


if (WIN32)
    set(PLATFORM_LINK_LIBRARIES ws2_32)
    SET(SPEECH_API_TARGET_COMPILE_DEFINITIONS_PRIVATE _WIN32_WINNT=0x0600)
endif ()

# === GOOGLE APIS ===
if (GOOGLEAPIS_DIR)
    set(_SPEECH_FILE "google/cloud/speech/v1/cloud_speech.grpc.pb.h")
    if (EXISTS "${GOOGLEAPIS_DIR}/${_SPEECH_FILE}")
        SET(GOOGLEAPIS_PATH "${GOOGLEAPIS_DIR}")
    elseif (EXISTS "${GOOGLEAPIS_DIR}/gens/${_SPEECH_FILE}")
        SET(GOOGLEAPIS_PATH "${GOOGLEAPIS_DIR}/gens")
    else ()
        message(FATAL_ERROR "GOOGLEAPIS_PATH appears invalid, no ${_SPEECH_FILE} found: ${GOOGLEAPIS_DIR} ${CMAKE_CURRENT_SOURCE_DIR}")
    endif ()
else ()
    message(FATAL_ERROR "no GOOGLEAPIS_DIR")
endif ()

message("using GOOGLEAPIS_PATH: ${GOOGLEAPIS_PATH}")
include_directories(${GOOGLEAPIS_PATH})

file(GLOB_RECURSE GOOGLE_API
        ${GOOGLEAPIS_PATH}/google/api/http*.cc
        ${GOOGLEAPIS_PATH}/google/api/field_behavior*.cc
        ${GOOGLEAPIS_PATH}/google/api/client*.cc
        ${GOOGLEAPIS_PATH}/google/api/annotations*.cc
        ${GOOGLEAPIS_PATH}/google/api/resource*.cc
        )
file(GLOB_RECURSE GOOGLE_RPC ${GOOGLEAPIS_PATH}/google/rpc/*.cc)
file(GLOB_RECURSE GOOGLE_LONGRUNNING ${GOOGLEAPIS_PATH}/google/longrunning/*.cc)
file(GLOB_RECURSE GOOGLE_SPEECH ${GOOGLEAPIS_PATH}/google/cloud/speech/v1/*.cc)

set(_GOOGLE_API_FILES_NAT
        ${GOOGLE_API}
        ${GOOGLE_RPC}
        ${GOOGLE_LONGRUNNING}
        ${GOOGLE_SPEECH}
        )

set(GOOGLE_API_FILES "")
foreach (_a_file_nat ${_GOOGLE_API_FILES_NAT})
    FILE(TO_CMAKE_PATH ${_a_file_nat} _a_file)
    LIST(APPEND GOOGLE_API_FILES "${_a_file}")
endforeach ()

list(LENGTH GOOGLE_API_FILES sources_cnt)
message("Google API sources count: ${sources_cnt}")

# === YANDEX CLOUDAPI ===
if (YANDEX_CLOUDAPI_DIR)
    set(_YANDEX_FILE "yandex/cloud/ai/stt/v3/stt_service.grpc.pb.h")
    if (EXISTS "${YANDEX_CLOUDAPI_DIR}/${_YANDEX_FILE}")
        SET(YANDEX_CLOUDAPI_PATH "${YANDEX_CLOUDAPI_DIR}")
    elseif (EXISTS "${YANDEX_CLOUDAPI_DIR}/gens/${_YANDEX_FILE}")
        SET(YANDEX_CLOUDAPI_PATH "${YANDEX_CLOUDAPI_DIR}/gens")
    else ()
        message(FATAL_ERROR "YANDEX_CLOUDAPI_PATH appears invalid, no ${_YANDEX_FILE} found: ${YANDEX_CLOUDAPI_DIR}")
    endif ()
    message("using YANDEX_CLOUDAPI_PATH: ${YANDEX_CLOUDAPI_PATH}")
    include_directories(${YANDEX_CLOUDAPI_PATH})

    file(GLOB_RECURSE YANDEX_STT ${YANDEX_CLOUDAPI_PATH}/yandex/cloud/ai/stt/v3/*.cc)
    file(GLOB_RECURSE YANDEX_API ${YANDEX_CLOUDAPI_PATH}/yandex/cloud/api/*.cc)
    file(GLOB_RECURSE YANDEX_OPERATION ${YANDEX_CLOUDAPI_PATH}/yandex/cloud/operation/*.cc)
    file(GLOB_RECURSE YANDEX_VALIDATION ${YANDEX_CLOUDAPI_PATH}/yandex/cloud/validation.pb.cc)

    set(_YANDEX_API_FILES_NAT
            ${YANDEX_STT}
            ${YANDEX_API}
            ${YANDEX_OPERATION}
            ${YANDEX_VALIDATION}
            )

    set(YANDEX_API_FILES "")
    foreach (_a_file_nat ${_YANDEX_API_FILES_NAT})
        FILE(TO_CMAKE_PATH ${_a_file_nat} _a_file)
        LIST(APPEND YANDEX_API_FILES "${_a_file}")
    endforeach ()

    list(LENGTH YANDEX_API_FILES yandex_sources_cnt)
    message("Yandex CloudAPI sources count: ${yandex_sources_cnt}")
endif ()

if (USE_OS_CERTS)
    message("grpc: USE_OS_CERTS, attempting OS certs")
else ()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certs/roots.h" AND DEVMODE)
        message("roots.h exists, not recreating")
    else ()
        message("creating roots.h")
        include(certs/bin2h.cmake)
        bin2h(SOURCE_FILE certs/roots.pem HEADER_FILE certs/roots.h VARIABLE_NAME roots_pem)
    endif ()
    message("grpc: using included certs")
    SET(SPEECH_API_COMPILE_DEFINITIONS GRPC_USE_INCLUDED_CERTS=1)
endif ()

set(SPEECH_API_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/CaptionStream.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CaptionStream.h

        ${GOOGLE_API_FILES}
        ${YANDEX_API_FILES}
        )

add_library(speech_api_stream STATIC
        ${SPEECH_API_SOURCES}
        )

if (UNIX AND NOT APPLE)
    target_compile_options(speech_api_stream PRIVATE "-fPIC")
endif ()

target_include_directories(speech_api_stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(speech_api_stream PRIVATE ${Protobuf_INCLUDE_DIRS})

target_link_libraries(speech_api_stream PRIVATE ${GRPC_NAME})
target_link_libraries(speech_api_stream PUBLIC ${PLATFORM_LINK_LIBRARIES})

target_compile_definitions(speech_api_stream PRIVATE
        ${SPEECH_API_COMPILE_DEFINITIONS}
        ${SPEECH_API_TARGET_COMPILE_DEFINITIONS_PRIVATE})

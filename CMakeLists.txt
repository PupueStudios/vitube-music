cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(vitube-music C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "ViTube Music")
set(VITA_TITLEID "YTMV00001")
set(VITA_VERSION "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -std=gnu11 -Wall -Wno-unused-function")

execute_process(
  COMMAND "${VITASDK}/bin/arm-vita-eabi-pkg-config" --dont-define-prefix --static --libs libcurl
  OUTPUT_VARIABLE CURL_LIBS_RAW
  RESULT_VARIABLE CURL_PC_RESULT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)

if(CURL_PC_RESULT EQUAL 0 AND NOT "${CURL_LIBS_RAW}" STREQUAL "")
  string(REGEX REPLACE "-L[^ ]+" "" CURL_LIBS_RAW "${CURL_LIBS_RAW}")
  separate_arguments(CURL_LIBS UNIX_COMMAND "${CURL_LIBS_RAW}")
else()
  set(CURL_LIBS curl ssl crypto z zstd SceNet_stub SceNetCtl_stub SceSysmodule_stub SceRtc_stub)
endif()

add_executable(${PROJECT_NAME}
  src/main.c
  src/util.c
  src/net.c
  src/ytm.c
  src/hls.c
  src/tsdemux.c
  src/aac.c
  src/player.c
  src/jobs.c
  src/dl.c
  src/hlsres.c
  src/thumbs.c
  src/store.c
  src/ui.c
  src/gfx.c
  src/font.c
  third_party/jansson/dump.c
  third_party/jansson/error.c
  third_party/jansson/hashtable.c
  third_party/jansson/hashtable_seed.c
  third_party/jansson/load.c
  third_party/jansson/memory.c
  third_party/jansson/pack_unpack.c
  third_party/jansson/strbuffer.c
  third_party/jansson/strconv.c
  third_party/jansson/utf.c
  third_party/jansson/value.c
  third_party/jansson/version.c
)

target_include_directories(${PROJECT_NAME} BEFORE PRIVATE third_party/jansson)
target_include_directories(${PROJECT_NAME} PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/freetype2")
target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_CONFIG_H=1)

target_link_libraries(${PROJECT_NAME}
  ${CURL_LIBS}
  vita2d
  faad
  freetype
  bz2
  png
  jpeg
  z
  m
  c
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceCommonDialog_stub
  SceIme_stub
  ScePgf_stub
  ScePower_stub
  SceAppMgr_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  SceRtc_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
  SceProcessmgr_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE assets/cacert.pem data/cacert.pem
  FILE assets/PSButtons.ttf assets/PSButtons.ttf
  FILE assets/Logo.png assets/Logo.png
  FILE assets/Heart.png assets/Heart.png
  FILE assets/Heart_s.png assets/Heart_s.png
  FILE assets/Note.png assets/Note.png
  FILE assets/Search.png assets/Search.png
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
  FILE sce_sys/livearea/contents/frame1.png sce_sys/livearea/contents/frame1.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

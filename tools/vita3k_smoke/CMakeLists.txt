# tools/vita3k_smoke: the real game worker's job path as a Vita homebrew app, for Vita3K
# (tools/vita3k_smoke.sh builds and runs it). Not a release artifact; nothing here ships.
#
#   cmake -S tools/vita3k_smoke -B build/vita3k-smoke -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
#   cmake --build build/vita3k-smoke        -> vjo_smoke.vpk
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()
project(vjo_smoke C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VJO capture smoke")
set(VITA_TITLEID "VJOSMK001")
set(VITA_VERSION "01.00")
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(VJO_VOCR_SOURCE_DIR "" CACHE PATH "vita-vn-ocr checkout (runtime/)")
if(NOT EXISTS "${VJO_VOCR_SOURCE_DIR}/runtime/vocr_lines_rows.c")
  message(FATAL_ERROR "VJO_VOCR_SOURCE_DIR must name a vita-vn-ocr checkout with runtime/vocr_lines_rows.c")
endif()
set(RT ${VJO_VOCR_SOURCE_DIR}/runtime)
set(ARCH -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)

# runtime/README.md "Porting notes": C99, -O2, no FP contraction, so every platform computes the same bits
add_library(smoke_vocr_rt STATIC ${RT}/vocr.c ${RT}/vocr_lines.c ${RT}/vocr_lines_rows.c)
target_compile_options(smoke_vocr_rt PRIVATE ${ARCH} -std=c99 -O2 -ffp-contract=off)
add_library(smoke_bearssl STATIC ${ROOT}/third_party/bearssl/src/hash/sha2small.c
  ${ROOT}/third_party/bearssl/src/codec/dec32be.c ${ROOT}/third_party/bearssl/src/codec/enc32be.c)
target_include_directories(smoke_bearssl PRIVATE ${ROOT}/third_party/bearssl/inc ${ROOT}/third_party/bearssl/src)
target_compile_options(smoke_bearssl PRIVATE ${ARCH} -O2 -w)

add_executable(vjo_smoke main.c ${ROOT}/game_ocr/main.c ${ROOT}/core/vocr_ocr.c ${ROOT}/core/ledger.c
  ${ROOT}/core/games.c)
target_include_directories(vjo_smoke PRIVATE ${ROOT}/core ${ROOT}/include ${RT} ${ROOT}/third_party/bearssl/inc)
target_compile_definitions(vjo_smoke PRIVATE VJO_GAME_OCR_NO_MODULE_ENTRY=1 VJO_WITH_VOCR=1)
target_compile_options(vjo_smoke PRIVATE ${ARCH} -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo_smoke smoke_vocr_rt smoke_bearssl m
  SceDisplay_stub SceSysmem_stub SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub ScePower_stub_weak
  SceProcessmgr_stub SceAppMgr_stub)
vita_create_self(eboot.bin vjo_smoke)
vita_create_vpk(vjo_smoke.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME})

# Vita build: kernel plugin (.skprx), SceShell plugin (.suprx), RCO and the
# release zip. Configure with:
#   cmake -S vita -B build/vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaJPOverlay C CXX ASM)
# taihen's TAI_CONTINUE relies on K&R-style `()` casts, which C23 (GCC 15 default) rejects.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VJO_ELF_FLAGS "${VITA_ELF_CREATE_FLAGS}")
set(VJO_SELF_FLAGS "${VITA_MAKE_FSELF_FLAGS}")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(VJO_ROOT ${ROOT})
set(VJO_BUILD_VITA ON)
option(VJO_WITH_NCNN "Build the CPU-only local mobile OCR recognizer" OFF)
option(VJO_WITH_VOCR "Build the vita-vn-ocr local OCR backend (needs its runtime sources)" ON)
set(VJO_VERSION "0.7.12" CACHE STRING "Release archive version")


# ---------------- kernel ----------------
add_executable(VitaJPOverlay_Kernel
  ${ROOT}/kernel/main.c
  ${ROOT}/kernel/boot.c
  ${ROOT}/kernel/ledger.c
  ${ROOT}/kernel/meminfo.c
  ${ROOT}/core/boot_guard.c
  ${ROOT}/core/ledger.c
  ${ROOT}/kernel/log.c
  ${ROOT}/kernel/buffers.c
  ${ROOT}/kernel/capture.c
  ${ROOT}/kernel/capture_request.c
  ${ROOT}/kernel/game_ocr.c
  ${ROOT}/kernel/display.c
  ${ROOT}/kernel/input.c
  ${ROOT}/kernel/lifecycle.c
  ${ROOT}/kernel/foreground.c
  ${ROOT}/kernel/scene.c
  ${ROOT}/kernel/triggers.c)
target_compile_options(VitaJPOverlay_Kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu99 -fno-builtin
  -mgeneral-regs-only)

target_link_options(VitaJPOverlay_Kernel PRIVATE -nostdlib -Wl,-q)
target_link_libraries(VitaJPOverlay_Kernel
  gcc
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceThreadmgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceCpuForDriver_stub
  SceDisplayForDriver_stub
  SceCtrlForDriver_stub
  SceTouchForDriver_stub
  SceIofilemgrForDriver_stub)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET VitaJPOverlay_Kernel POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_kernel_instructions.py
    --objdump ${CMAKE_OBJDUMP} $<TARGET_FILE:VitaJPOverlay_Kernel>
  COMMENT "Checking kernel code contains no FPU or NEON instructions")
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(VitaJPOverlay_Kernel.skprx VitaJPOverlay_Kernel
  UNSAFE
  CONFIG ${ROOT}/kernel/exports.yml)
vita_create_stubs(vjo_kernel_stubs VitaJPOverlay_Kernel ${ROOT}/kernel/exports.yml KERNEL)

# ---------------- shell ----------------
# BearSSL and the portable core, built for SceShell (no newlib: see
# shell/libc_shim.c; no OS entropy/time sources: both are injected).
file(GLOB_RECURSE BEARSSL_SRC ${ROOT}/third_party/bearssl/src/*.c)
add_library(vjo_bearssl STATIC ${BEARSSL_SRC})
target_include_directories(vjo_bearssl PUBLIC ${ROOT}/third_party/bearssl/inc PRIVATE ${ROOT}/third_party/bearssl/src)
target_compile_definitions(vjo_bearssl PRIVATE BR_USE_URANDOM=0 BR_USE_GETENTROPY=0 BR_USE_UNIX_TIME=0
  BR_USE_WIN32_RAND=0 BR_USE_WIN32_TIME=0 BR_RDRAND=0)
target_compile_options(vjo_bearssl PRIVATE -O2 -w -fno-builtin -fshort-wchar -ffunction-sections -fdata-sections)

add_library(vjo_core_vita STATIC
  ${ROOT}/core/arena.c ${ROOT}/core/pb.c ${ROOT}/core/utf.c ${ROOT}/core/text_hit.c ${ROOT}/core/lens.c ${ROOT}/core/json.c
  ${ROOT}/core/jpdb.c ${ROOT}/core/textfilter.c ${ROOT}/core/entries.c ${ROOT}/core/jiten.c
  ${ROOT}/core/config.c ${ROOT}/core/regions.c ${ROOT}/core/http.c ${ROOT}/core/tls.c
  ${ROOT}/core/tls_roots.c ${ROOT}/core/client.c ${ROOT}/core/styled.c ${ROOT}/core/jpegsw.c
  ${ROOT}/core/base64.c ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c ${ROOT}/core/net.c ${ROOT}/core/hachidori.c ${ROOT}/core/local_dict.c
  ${ROOT}/core/ocr_lines.c ${ROOT}/core/games.c ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c
  ${ROOT}/core/ledger.c)
target_include_directories(vjo_core_vita PUBLIC ${ROOT}/core ${ROOT}/include)
target_link_libraries(vjo_core_vita PUBLIC vjo_bearssl)
target_compile_options(vjo_core_vita PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -fno-builtin -fshort-wchar
  -ffunction-sections -fdata-sections)
# Cold core paths (configuration, requests, parsers, queue files): built for
# size, since every page of the Shell module is SceShell's. Lookup, OCR text
# handling, JPEG and TLS stay at -O2.
set_source_files_properties(${ROOT}/core/config.c ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c
  ${ROOT}/core/json.c ${ROOT}/core/hachidori.c ${ROOT}/core/http.c ${ROOT}/core/net.c ${ROOT}/core/jiten.c
  ${ROOT}/core/jpdb.c ${ROOT}/core/lens.c ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c ${ROOT}/core/regions.c
  ${ROOT}/core/games.c ${ROOT}/core/sfo.c ${ROOT}/core/base64.c ${ROOT}/core/pb.c ${ROOT}/core/client.c
  PROPERTIES COMPILE_OPTIONS "-Os")
if(VJO_WITH_NCNN)
  include(${ROOT}/cmake/LocalOCR.cmake)
  target_compile_definitions(vjo_core_vita PUBLIC VJO_WITH_NCNN=1)
  target_link_libraries(vjo_ocr PUBLIC vjo_core_vita)
endif()
if(VJO_WITH_VOCR)
  include(${ROOT}/cmake/VocrOCR.cmake)
  target_compile_definitions(vjo_core_vita PUBLIC VJO_WITH_VOCR=1)
  target_include_directories(vjo_vocr PUBLIC ${ROOT}/core ${ROOT}/include ${ROOT}/third_party/bearssl/inc)
endif()

add_executable(VitaJPOverlay_Shell
  ${ROOT}/shell/boot.c
  ${ROOT}/shell/guard.c
  ${ROOT}/shell/ledger.c
  ${ROOT}/shell/selftest.c
  ${ROOT}/shell/main.cpp
  ${ROOT}/shell/memory.cpp
  ${ROOT}/shell/overlay.cpp
  ${ROOT}/core/region_select.c
  ${ROOT}/shell/strip.cpp
  ${ROOT}/shell/strip_view.c
  ${ROOT}/shell/worker.c
  ${ROOT}/shell/capture.c
  ${ROOT}/shell/game_ocr_client.c
  ${ROOT}/shell/game.c
  ${ROOT}/shell/anki.c
  ${ROOT}/shell/anki_queue.c
  ${ROOT}/vita/games_storage.c
  ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/log_vita.c
  ${ROOT}/shell/files.c
  ${ROOT}/vita/games_storage.c
  ${ROOT}/shell/libc_shim.c)
add_dependencies(VitaJPOverlay_Shell vjo_kernel_stubs)
target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_PAF_ALLOC=1)
# The overlay page (once a frame while it is open; paf's own calls dominate)
# and the region selector it draws: built for size, which gave the region's
# stick cursor its room in the module's pages (#81; overlay.cpp's text 11,608 B at -O2,
# 8,822 B at -Os with the cursor).
set_source_files_properties(${ROOT}/shell/overlay.cpp ${ROOT}/core/region_select.c
  PROPERTIES COMPILE_OPTIONS "-Os")
# Startup runs before Paf is ready; it only needs integer operations. Avoid
# compiler-generated VFP/NEON copies in the early bootstrap and status logger.
set_source_files_properties(${ROOT}/shell/boot.c ${ROOT}/shell/guard.c ${ROOT}/shell/log_vita.c
  PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only;-Os")
# Cold paths (bootstrap, guard, ledger, logging, file access, queue files, the
# worker's job control): every page of this module is SceShell's, so they are
# built for size. worker.c at -Os pays for the word lookup's ranking and log
# within the module's pages (docs/memory-ledger.md).
set_source_files_properties(${ROOT}/shell/ledger.c ${ROOT}/shell/selftest.c ${ROOT}/shell/anki.c
  ${ROOT}/shell/files.c ${ROOT}/shell/game.c ${ROOT}/shell/worker.c
  ${ROOT}/shell/strip_view.c ${ROOT}/shell/capture.c ${ROOT}/shell/game_ocr_client.c
  ${ROOT}/shell/anki_queue.c ${ROOT}/shell/platform_vita.c
  PROPERTIES COMPILE_OPTIONS "-Os")
if(VJO_WITH_NCNN)
  target_link_libraries(VitaJPOverlay_Shell vjo_ocr ncnn)
endif()
# The Shell imports only the bounded job transport. No vocr inference is linked here.

target_compile_options(VitaJPOverlay_Shell PRIVATE -O2 -Wall -Wno-unused-parameter -fno-builtin -fshort-wchar
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-Wno-sign-compare;-Wno-unused-function>"
  "-D__declspec(x)=")
target_link_directories(VitaJPOverlay_Shell PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
target_link_options(VitaJPOverlay_Shell PRIVATE -nostdlib -nostartfiles -Wl,-q -Wl,--gc-sections
  -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning)
target_link_libraries(VitaJPOverlay_Shell
  vjo_core_vita
  VitaJPOverlay_Kernel_stub_weak  # weak: the shell still starts (and reports) without the kernel plugin
  taihen_stub
  ScePaf_stub_weak
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  SceProcessmgr_stub
  SceIofilemgr_stub
  SceVshBridge_stub
  # Weak: SceShell loads these after *main plugins start; the imports are
  # bound once the libraries load (as with ScePaf).
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceRtc_stub_weak
  SceAppMgr_stub_weak
  vjo_bearssl  # again after the core: its integer division comes from libgcc
  gcc)
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(VitaJPOverlay_Shell.suprx VitaJPOverlay_Shell
  UNSAFE
  CONFIG ${ROOT}/shell/exports.yml)
add_custom_target(vjo_syscall_abi ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_vita_syscall_abi.py
    --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
    --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self
  COMMENT "Checking packaged syscall ABI and firmware-independent kernel imports")

# ---------------- game-process vocr worker ----------------
if(VJO_WITH_VOCR)
  add_executable(VitaJPOverlay_OCR
    ${ROOT}/game_ocr/main.c ${ROOT}/core/games.c ${ROOT}/core/ledger.c ${ROOT}/shell/libc_shim.c)
  add_dependencies(VitaJPOverlay_OCR vjo_kernel_stubs)
  target_compile_options(VitaJPOverlay_OCR PRIVATE -O2 -Wall -Wextra -fno-builtin -fshort-wchar
    -ffunction-sections -fdata-sections)
  target_link_options(VitaJPOverlay_OCR PRIVATE -nostdlib -nostartfiles -Wl,-q -Wl,--gc-sections
    -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning)
  target_link_directories(VitaJPOverlay_OCR PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
  target_link_libraries(VitaJPOverlay_OCR vjo_vocr vjo_vocr_rt vjo_bearssl VitaJPOverlay_Kernel_stub_weak
    SceLibKernel_stub SceKernelThreadMgr_stub SceSysmem_stub_weak SceProcessmgr_stub_weak
    SceIofilemgr_stub SceAppMgr_stub_weak SceDisplay_stub_weak ScePower_stub_weak gcc)
  set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
  set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
  vita_create_self(VitaJPOverlay_OCR.suprx VitaJPOverlay_OCR UNSAFE CONFIG ${ROOT}/game_ocr/exports.yml)
  add_custom_target(vjo_game_syscall_abi ALL
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_vita_syscall_abi.py
      --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
      --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
      --worker ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx
    DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self VitaJPOverlay_OCR.suprx-self)
  # The memory ledger (docs/memory-ledger.md): allocation sites, worst cases
  # against the cited limits, and the packaged modules' pages against the
  # declared budget. Its table is this build's record of what we hold.
  add_custom_target(vjo_memory_ledger ALL
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_memory_ledger.py
      --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
      --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
      --worker ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx
    DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self VitaJPOverlay_OCR.suprx-self
    COMMENT "Checking the memory ledger against the packaged modules"
    USES_TERMINAL)
endif()

# ---------------- RCO ----------------
find_program(PSP2CXML psp2cxml-tool HINTS ${ROOT}/.refs/toolchain/psp2cxml-tool/mbuild ${ROOT}/.refs/psp2cxml-tool/mbuild ENV PATH REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/shell/rco/vitajpoverlay.xml ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.xml
  COMMAND ${PSP2CXML} ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.xml
  DEPENDS ${ROOT}/shell/rco/vitajpoverlay.xml
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_custom_target(vjo_rco ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco)

# ---------------- standalone game settings VPK ----------------
include(${ROOT}/cmake/SettingsPNG.cmake)
add_executable(VitaJPOverlay_Settings
  ${ROOT}/settings/main.c ${ROOT}/settings/catalog.c ${ROOT}/settings/preferences.c
  ${ROOT}/core/games.c ${ROOT}/core/sfo.c ${ROOT}/core/arena.c
  ${ROOT}/core/config.c ${ROOT}/core/utf.c ${ROOT}/core/local_dict.c
  ${ROOT}/vita/games_storage.c)
target_compile_definitions(VitaJPOverlay_Settings PRIVATE VJO_STANDALONE=1)
target_compile_options(VitaJPOverlay_Settings PRIVATE -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
target_link_options(VitaJPOverlay_Settings PRIVATE -Wl,-q,--gc-sections)
target_link_libraries(VitaJPOverlay_Settings
  vita2d png_static jpeg z m SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub
  ScePvf_stub ScePgf_stub SceCommonDialog_stub SceSysmodule_stub SceIofilemgr_stub
  SceAppMgr_stub)
add_dependencies(VitaJPOverlay_Settings vjo_settings_png_geometry)
add_custom_command(TARGET VitaJPOverlay_Settings POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_settings_png_abi.py
    --objdump ${CMAKE_OBJDUMP}
    --geometry-object $<TARGET_OBJECTS:vjo_settings_png_geometry>
    $<TARGET_FILE:VitaJPOverlay_Settings>
  COMMENT "Checking linked Settings setjmp/longjmp fit libpng's jump buffer")
# Plugin CONFIG exports must not leak into the standalone application's ELF.
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(eboot.bin VitaJPOverlay_Settings UNSAFE)
# LiveArea title (param.sfo TITLE/STITLE) is "Overlay". The title ID, the VPK file name and every
# plugin, path and config name keep their old names, so installing over JP Overlay Settings is an update.
vita_create_vpk(JPOverlaySettings.vpk VJOSET001 eboot.bin
  VERSION "01.00" NAME "Overlay"
  FILE ${ROOT}/settings/assets/icon0.png sce_sys/icon0.png
       ${ROOT}/settings/assets/bg.png sce_sys/livearea/contents/bg.png
       ${ROOT}/settings/assets/startup.png sce_sys/livearea/contents/startup.png
       ${ROOT}/settings/assets/template.xml sce_sys/livearea/contents/template.xml)
add_custom_target(vjo_settings_assets ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_settings_assets.py
    ${CMAKE_CURRENT_BINARY_DIR}/JPOverlaySettings.vpk
  DEPENDS JPOverlaySettings.vpk-vpk)

# ---------------- release zip ----------------
set(VJO_WORKER_RELEASE_COPY)
set(VJO_WORKER_RELEASE_DEPS)
if(VJO_WITH_VOCR)
  set(VJO_WORKER_RELEASE_COPY COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx ${CMAKE_CURRENT_BINARY_DIR}/release/ur0/tai/)
  set(VJO_WORKER_RELEASE_DEPS VitaJPOverlay_OCR.suprx-self vjo_game_syscall_abi)
endif()
# config.ini sits at the zip's top level (the user edits it and copies it to
# ux0:data/VitaJPOverlay/), so extracting an update over ur0 never overwrites
# the user's API key.
set(STAGE ${CMAKE_CURRENT_BINARY_DIR}/release)
set(VJO_PLAINTEXT_SOURCE "${ROOT}/.refs/plaintext-dicts" CACHE PATH "Frozen private dictionary checkout")
add_custom_target(release
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ur0/tai ${STAGE}/ur0/data/VitaJPOverlay
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx ${STAGE}/ur0/tai/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx ${STAGE}/ur0/tai/
  ${VJO_WORKER_RELEASE_COPY}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco ${STAGE}/ur0/data/VitaJPOverlay/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/README.md ${ROOT}/LICENSE ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/config.example.ini ${STAGE}/config.ini
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/tools ${STAGE}/docs/benchmarks ${STAGE}/third_party
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/tools/convert_dictionary.py ${ROOT}/tools/install_dictionary.py ${ROOT}/tools/prepare_ocr.py ${ROOT}/tools/prepare_jmdict.py ${ROOT}/tools/prepare_plaintext.py ${ROOT}/tools/package_release.py ${ROOT}/tools/install_ftp.sh ${ROOT}/tools/migrate_config.py ${ROOT}/tools/patch_tai_config.py ${ROOT}/tools/analyse_selftest.py ${ROOT}/tools/collect_vita.py ${STAGE}/tools/
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/prepare_plaintext.py --source ${VJO_PLAINTEXT_SOURCE} --stage ${STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/project-lessons.md ${ROOT}/docs/todo-20261011.md ${ROOT}/docs/settings-readability.md ${ROOT}/docs/settings-png-runtime.md ${ROOT}/docs/touch-lookup.md ${ROOT}/docs/anki.md ${ROOT}/docs/local-dictionaries.md ${ROOT}/docs/local-ocr.md ${ROOT}/docs/vita-vn-ocr.md ${ROOT}/docs/lookup-performance.md ${ROOT}/docs/lens-performance.md ${ROOT}/docs/jmdict-source.json ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/benchmarks/local-lookup-jitendex.json ${ROOT}/docs/benchmarks/kernel-startup-20261010.json ${ROOT}/docs/benchmarks/daijirin-v3-selected-20261011.json ${ROOT}/docs/benchmarks/daijirin-import-20261010.json ${ROOT}/docs/benchmarks/lens-connections-20261010.json ${ROOT}/docs/benchmarks/vocr-vs-ncnn-20261010.json ${ROOT}/docs/benchmarks/vocr-release-20261011.json ${ROOT}/docs/benchmarks/vocr2-candidate-20261012.json ${ROOT}/docs/benchmarks/local-result-memory-20261011.json ${ROOT}/docs/benchmarks/local-character-memory-20261011.json ${STAGE}/docs/benchmarks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/README.md ${STAGE}/third_party/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/third_party/licenses ${STAGE}/third_party/licenses
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/bearssl/LICENSE.txt ${STAGE}/third_party/licenses/BearSSL-MIT.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/jsmn/LICENSE ${STAGE}/third_party/licenses/jsmn-MIT.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/JPOverlaySettings.vpk ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E chdir ${STAGE} ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay-${VJO_VERSION}.zip --format=zip ur0 ux0 config.ini README.md LICENSE tools docs third_party JPOverlaySettings.vpk
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self ${VJO_WORKER_RELEASE_DEPS} vjo_rco vjo_syscall_abi vjo_settings_assets
  COMMENT "Packaging VitaJPOverlay-${VJO_VERSION}.zip")

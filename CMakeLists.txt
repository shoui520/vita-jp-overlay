# Host build: portable core, BearSSL, the vjo-cli tool and unit tests.
# The Vita build lives in vita/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
project(vita_jp_overlay_host C CXX)
option(VJO_WITH_NCNN "Build the CPU-only local mobile OCR recognizer" OFF)
option(VJO_WITH_VOCR "Build the vita-vn-ocr local OCR backend (needs its runtime sources)" ON)
set(VJO_VOCR_MODEL_DIR "" CACHE PATH "Directory with vita-vn-ocr weights for the model tests (optional)")
set(VJO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB_RECURSE BEARSSL_SRC ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/*.c)
add_library(bearssl STATIC ${BEARSSL_SRC})
target_include_directories(bearssl PUBLIC third_party/bearssl/inc PRIVATE third_party/bearssl/src)
target_compile_options(bearssl PRIVATE -w)

set(CORE_SRC
  core/arena.c core/pb.c core/utf.c core/text_hit.c core/lens.c core/json.c core/jpdb.c
  core/textfilter.c core/entries.c core/jiten.c core/config.c core/regions.c
  core/http.c core/tls.c core/tls_roots.c core/client.c core/styled.c core/jpegsw.c
  core/base64.c core/anki.c core/anki_queue.c core/net.c core/hachidori.c core/local_dict.c core/ocr_lines.c
  core/games.c core/sfo.c core/furigana.c core/pitch.c core/ledger.c shell/game_ocr_client.c)
add_library(vjo_core STATIC ${CORE_SRC})
target_include_directories(vjo_core PUBLIC core include)
target_link_libraries(vjo_core PUBLIC bearssl)
target_compile_definitions(vjo_core PUBLIC VJO_HOST=1)
target_compile_options(vjo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(VJO_WITH_NCNN)
  include(cmake/LocalOCR.cmake)
  target_compile_definitions(vjo_core PUBLIC VJO_WITH_NCNN=1)
  target_link_libraries(vjo_ocr PUBLIC vjo_core)
endif()
if(VJO_WITH_VOCR)
  include(cmake/VocrOCR.cmake)
  target_compile_definitions(vjo_core PUBLIC VJO_WITH_VOCR=1)
  target_link_libraries(vjo_vocr PUBLIC vjo_core)
endif()

add_library(vjo_host STATIC host/net_posix.c host/render.c host/replay.c)
target_include_directories(vjo_host PUBLIC host)
target_link_libraries(vjo_host PUBLIC vjo_core)
target_compile_options(vjo_host PRIVATE -Wall -Wextra)

add_executable(vjo-cli host/cli.c)
target_link_libraries(vjo-cli vjo_host)

# Runs a local OCR backend on a raw RGBA region, as the Shell would (vjo-ocr --help).
if(VJO_WITH_NCNN OR VJO_WITH_VOCR)
  add_executable(vjo-ocr host/ocr_tool.c)
  target_link_libraries(vjo-ocr vjo_host)
  if(VJO_WITH_NCNN)
    target_link_libraries(vjo-ocr vjo_ocr)
  endif()
  if(VJO_WITH_VOCR)
    target_link_libraries(vjo-ocr vjo_vocr)
    # The stable-text wait's frames and time on a simulated screen (docs/vita-vn-ocr.md).
    add_executable(vjo-settle-bench host/settle_bench.c)
    target_link_libraries(vjo-settle-bench vjo_vocr vjo_host)
  endif()
endif()

# Raw RGBA -> JPEG with the software encoder (tools/test_jpegsw.py).
add_executable(vjo-jpegsw host/jpegsw_tool.c)
target_link_libraries(vjo-jpegsw vjo_host)

# Bounded live Lens-only fresh/kept-connection timing comparison.
add_executable(vjo-lens-bench host/benchmark_lens.c)
target_link_libraries(vjo-lens-bench vjo_host)

enable_testing()
add_executable(vjo-shell-capture-tests host/tests/test_shell_capture.c)
target_include_directories(vjo-shell-capture-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-shell-capture-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-shell-capture-tests vjo_host)
add_test(NAME shell_capture COMMAND vjo-shell-capture-tests)
add_executable(vjo-lens-transport-tests host/tests/test_lens_transport.c)
target_include_directories(vjo-lens-transport-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-lens-transport-tests vjo_host)
target_compile_options(vjo-lens-transport-tests PRIVATE -Wall -Wextra)
add_test(NAME lens_transport COMMAND vjo-lens-transport-tests)
add_executable(vjo-kernel-capture-tests host/tests/test_kernel_capture.c kernel/scene.c kernel/ledger.c core/ledger.c
  core/region_select.c)
target_include_directories(vjo-kernel-capture-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-kernel-capture-tests PRIVATE VJO_HOST=1)
target_compile_options(vjo-kernel-capture-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME kernel_capture COMMAND vjo-kernel-capture-tests)
# kernel/meminfo.c's three routes with the firmware's objects at kernel-range addresses (#74 audit B1).
add_executable(vjo-kernel-meminfo-tests host/tests/test_kernel_meminfo.c core/ledger.c)
target_include_directories(vjo-kernel-meminfo-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-kernel-meminfo-tests PRIVATE VJO_HOST=1)
target_compile_options(vjo-kernel-meminfo-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME kernel_meminfo COMMAND vjo-kernel-meminfo-tests)
add_executable(vjo-kernel-input-tests host/tests/test_kernel_input.c kernel/triggers.c)
target_include_directories(vjo-kernel-input-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-kernel-input-tests PRIVATE VJO_HOST=1)
target_compile_options(vjo-kernel-input-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME kernel_input COMMAND vjo-kernel-input-tests)
# Memory ledger: the counters, the budget table against the real sizing code,
# and the CI check (allocation sites, worst cases against limits).
add_executable(vjo-ledger-tests host/tests/test_ledger.c)
target_include_directories(vjo-ledger-tests PRIVATE third_party/acutest/include)
target_compile_options(vjo-ledger-tests PRIVATE -Wall -Wextra)
target_link_libraries(vjo-ledger-tests vjo_host)
if(VJO_WITH_VOCR)
  target_link_libraries(vjo-ledger-tests vjo_vocr)
endif()
add_test(NAME ledger COMMAND vjo-ledger-tests)
# Boot-safety guard: the record (core) and the real kernel-side code with the
# ur0 file and the clock replaced, boot after simulated boot.
add_executable(vjo-boot-guard-tests host/tests/test_boot_guard.c core/boot_guard.c kernel/boot.c)
target_include_directories(vjo-boot-guard-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-boot-guard-tests PRIVATE VJO_HOST=1)
target_compile_options(vjo-boot-guard-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME boot_guard COMMAND vjo-boot-guard-tests)
# kernel/foreground.c, scene.c, triggers.c: kernel logic without psp2kern.
add_executable(vjo-tests host/tests/test_core.c kernel/foreground.c kernel/scene.c kernel/triggers.c)

target_include_directories(vjo-tests PRIVATE third_party/acutest/include kernel)
target_link_libraries(vjo-tests vjo_host)
target_compile_definitions(vjo-tests PRIVATE VJO_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures")
add_test(NAME core COMMAND vjo-tests)
add_executable(vjo-local-cursor-tests host/tests/test_local_cursor.c)
target_include_directories(vjo-local-cursor-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-local-cursor-tests vjo_host)
target_compile_options(vjo-local-cursor-tests PRIVATE -Wall -Wextra)
add_test(NAME local_cursor COMMAND vjo-local-cursor-tests)

add_executable(vjo-header-highlight-tests host/tests/test_header_highlight.cpp)
target_include_directories(vjo-header-highlight-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-header-highlight-tests vjo_host)
target_compile_features(vjo-header-highlight-tests PRIVATE cxx_std_11)
target_compile_options(vjo-header-highlight-tests PRIVATE -Wall -Wextra)
add_test(NAME header_highlight COMMAND vjo-header-highlight-tests)
add_executable(vjo-word-navigation-tests host/tests/test_word_navigation.cpp)
target_include_directories(vjo-word-navigation-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-word-navigation-tests vjo_host)
target_compile_features(vjo-word-navigation-tests PRIVATE cxx_std_11)
target_compile_options(vjo-word-navigation-tests PRIVATE -Wall -Wextra)
add_test(NAME word_navigation COMMAND vjo-word-navigation-tests)
# The region selector's cursor, corners and touch drags (core/region_select.c,
# drawn by shell/overlay.cpp); vjo_host for region.ini's parser.
add_executable(vjo-region-select-tests host/tests/test_region_select.c core/region_select.c)
target_include_directories(vjo-region-select-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-region-select-tests vjo_host)
target_compile_options(vjo-region-select-tests PRIVATE -Wall -Wextra)
add_test(NAME region_select COMMAND vjo-region-select-tests)

# End-to-end word lookup on real VOCR output with the bundled dictionary
# (#73's frozen plaintext.vjdict). VJO_PLAINTEXT_SOURCE: a plaintext-dicts
# checkout or the frozen archive (default: .refs/plaintext-dicts, as CI checks
# it out); without one the test reports itself skipped.
set(VJO_PLAINTEXT_SOURCE "" CACHE PATH "plaintext-dicts checkout or frozen archive (word-lookup test)")
set(_vjo_plaintext "${VJO_PLAINTEXT_SOURCE}")
if(NOT _vjo_plaintext AND EXISTS ${CMAKE_SOURCE_DIR}/.refs/plaintext-dicts/processed/plaintext.vjdict.zip)
  set(_vjo_plaintext ${CMAKE_SOURCE_DIR}/.refs/plaintext-dicts)
endif()
add_executable(vjo-word-lookup-tests host/tests/test_word_lookup.cpp)
target_include_directories(vjo-word-lookup-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-word-lookup-tests vjo_host)
target_compile_features(vjo-word-lookup-tests PRIVATE cxx_std_11)
target_compile_options(vjo-word-lookup-tests PRIVATE -Wall -Wextra)
target_compile_definitions(vjo-word-lookup-tests PRIVATE VJO_WORD_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures/word_lookup")
add_test(NAME word_lookup COMMAND vjo-word-lookup-tests)
set_tests_properties(word_lookup PROPERTIES SKIP_RETURN_CODE 77)
if(_vjo_plaintext)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME plaintext_dictionary COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/prepare_plaintext.py
    --source ${_vjo_plaintext} -o ${CMAKE_BINARY_DIR}/plaintext/plaintext.vjdict)
  set_tests_properties(plaintext_dictionary PROPERTIES FIXTURES_SETUP plaintext)
  set_tests_properties(word_lookup PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${CMAKE_BINARY_DIR}/plaintext;VJO_REQUIRE_PLAINTEXT=1")
  set(_vjo_plaintext_dir ${CMAKE_BINARY_DIR}/plaintext)
endif()

add_executable(vjo-text-hit-tests host/tests/test_text_hit.c)
target_include_directories(vjo-text-hit-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-text-hit-tests vjo_host)
target_compile_options(vjo-text-hit-tests PRIVATE -Wall -Wextra)
add_test(NAME text_hit COMMAND vjo-text-hit-tests)
add_executable(vjo-anki-tests host/tests/test_anki.c)
target_include_directories(vjo-anki-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-anki-tests vjo_host)
add_test(NAME anki COMMAND vjo-anki-tests)
# Real Shell Anki miner, offline file queue (real files) and screenshot capture;
# Vita SDK substituted. This binary builds the screenshot path (VJO_ANKI_SCREENSHOT 1),
# which the Vita build leaves out (shell/anki.c); vjo-shell-anki-build-tests is the
# shipped, text-only configuration.
add_executable(vjo-shell-anki-tests host/tests/test_shell_anki.c)
target_include_directories(vjo-shell-anki-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-shell-anki-tests PRIVATE VJO_ANKI_SCREENSHOT=1)
target_compile_options(vjo-shell-anki-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-shell-anki-tests vjo_host)
add_test(NAME shell_anki COMMAND vjo-shell-anki-tests)
add_executable(vjo-shell-anki-build-tests host/tests/test_shell_anki.c)
target_include_directories(vjo-shell-anki-build-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-shell-anki-build-tests PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
target_link_libraries(vjo-shell-anki-build-tests vjo_host)
add_test(NAME shell_anki_build COMMAND vjo-shell-anki-build-tests)
add_executable(vjo-hachidori-tests host/tests/test_hachidori.c)
target_include_directories(vjo-hachidori-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-hachidori-tests vjo_host)
add_test(NAME hachidori COMMAND vjo-hachidori-tests)
# Real shell control policy, with host-only substitutes for Vita SDK I/O.
add_executable(vjo-shell-worker-tests host/tests/test_shell_worker.c)
target_include_directories(vjo-shell-worker-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-shell-worker-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-shell-worker-tests vjo_host)
if(VJO_WITH_NCNN)
  target_link_libraries(vjo-shell-worker-tests vjo_ocr)
endif()
if(VJO_WITH_VOCR)
  target_link_libraries(vjo-shell-worker-tests vjo_vocr)
endif()
add_test(NAME shell_worker COMMAND vjo-shell-worker-tests)
add_executable(vjo-shell-worker-paf-tests host/tests/test_shell_worker.c)
target_include_directories(vjo-shell-worker-paf-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-shell-worker-paf-tests PRIVATE VJO_PAF_ALLOC=1)
target_compile_options(vjo-shell-worker-paf-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-shell-worker-paf-tests vjo_host)
if(VJO_WITH_NCNN)
  target_link_libraries(vjo-shell-worker-paf-tests vjo_ocr)
endif()
if(VJO_WITH_VOCR)
  target_link_libraries(vjo-shell-worker-paf-tests vjo_vocr)
endif()
add_test(NAME shell_worker_paf COMMAND vjo-shell-worker-paf-tests)
if(_vjo_plaintext_dir) # the Shell's word lookup with the bundled dictionary too
  set_tests_properties(shell_worker shell_worker_paf PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir}")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
# The memory ledger's CI check: allocation sites and worst cases (docs/memory-ledger.md).
add_test(NAME memory_ledger COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_memory_ledger.py)
# Each step is a fresh process; the AnkiConnect HTTP double survives client restarts.
add_test(NAME anki_restart COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_SOURCE_DIR}/host/tests/test_anki_restart.py $<TARGET_FILE:vjo-shell-anki-tests>)
add_test(NAME anki_restart_build COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_SOURCE_DIR}/host/tests/test_anki_restart.py $<TARGET_FILE:vjo-shell-anki-build-tests>)
set_tests_properties(anki_restart PROPERTIES ENVIRONMENT "VJO_EXPECT_ANKI_SCREENSHOT=1")
set_tests_properties(anki_restart_build PROPERTIES ENVIRONMENT "VJO_EXPECT_ANKI_SCREENSHOT=0")
if(VJO_WITH_NCNN)
  add_executable(vjo-ocr-tests host/tests/test_ocr.c)
  target_include_directories(vjo-ocr-tests PRIVATE third_party/acutest/include)
  target_link_libraries(vjo-ocr-tests vjo_ocr)
  add_test(NAME local_ocr_unit COMMAND vjo-ocr-tests)
  add_test(NAME local_ocr_model COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_local_ocr.py
    --cli $<TARGET_FILE:vjo-ocr> --model ${VJO_OCR_ASSETS})
endif()
if(VJO_WITH_VOCR)
  add_executable(vjo-vocr-tests host/tests/test_vocr.c)
  target_include_directories(vjo-vocr-tests PRIVATE third_party/acutest/include)
  target_link_libraries(vjo-vocr-tests vjo_vocr vjo_host)
  target_compile_options(vjo-vocr-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(vjo-vocr-tests PRIVATE VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  add_test(NAME vocr_unit COMMAND vjo-vocr-tests)
  # The job policy (core/vocr_ocr.h VjoVocrPolicy): the stable-text wait and the
  # blank-line check, frame by frame on a simulated screen, without weights.
  add_executable(vjo-vocr-policy-tests host/tests/test_vocr_policy.c)
  target_include_directories(vjo-vocr-policy-tests PRIVATE third_party/acutest/include)
  target_link_libraries(vjo-vocr-policy-tests vjo_vocr vjo_host)
  target_compile_options(vjo-vocr-policy-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(vjo-vocr-policy-tests PRIVATE VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  add_test(NAME vocr_policy COMMAND vjo-vocr-policy-tests)
  # ocr-speed: parallel lines (VjoVocrPool) and the line cache with real vocr_run
  # calls on a tiny weights blob the test builds and pins itself (no release files).
  add_executable(vjo-vocr-parallel-tests host/tests/test_vocr_parallel.c core/vocr_ocr.c)
  target_include_directories(vjo-vocr-parallel-tests PRIVATE third_party/acutest/include host/tests core include
    third_party/bearssl/inc)
  target_compile_options(vjo-vocr-parallel-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(vjo-vocr-parallel-tests PRIVATE VJO_VOCR_TEST_MODELS="test_parallel_models.h")
  find_package(Threads REQUIRED)
  target_link_libraries(vjo-vocr-parallel-tests vjo_vocr_rt vjo_host bearssl Threads::Threads)
  add_test(NAME vocr_parallel COMMAND vjo-vocr-parallel-tests)
  # ocr-speed: serial against helpers (and the cache) on real regions with real weights (host only).
  add_executable(vjo-vocr-parallel-bench host/vocr_parallel_bench.c)
  target_link_libraries(vjo-vocr-parallel-bench vjo_vocr vjo_host Threads::Threads)
  # #74: API 18's capture (the kernel's A8B8G8R8 copy of the region, the worker's
  # grey plane; 8228c3c, verbatim in host/tests/api18) against API 19's (the
  # display hook's grey rows in the worker's strip): the recognizer gets
  # byte-identical input. tools/capture_parity.py adds the eval crops.
  add_library(vjo_parity_kernel OBJECT host/tests/parity_kernel.c kernel/capture.c kernel/scene.c)
  target_include_directories(vjo_parity_kernel PRIVATE host/tests/vita_stubs)
  target_compile_definitions(vjo_parity_kernel PRIVATE VJO_HOST=1)
  add_executable(vjo-capture-parity-tests host/tests/test_capture_parity.c host/tests/parity_api18.c
    host/tests/parity_api19.c $<TARGET_OBJECTS:vjo_parity_kernel>)
  target_include_directories(vjo-capture-parity-tests PRIVATE third_party/acutest/include)
  target_link_libraries(vjo-capture-parity-tests vjo_vocr_rt vjo_core)
  target_compile_options(vjo-capture-parity-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(vjo-capture-parity-tests PRIVATE VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  add_test(NAME capture_parity COMMAND vjo-capture-parity-tests)
  # Exercise the real adapter with only the finder call replaced by a spy.
  # This verifies dispatch and capture ownership without weights or inference.
  add_executable(vjo-vocr-mode-tests host/tests/test_vocr_mode.c core/vocr_ocr.c)
  target_include_directories(vjo-vocr-mode-tests PRIVATE third_party/acutest/include)
  target_compile_definitions(vjo-vocr-mode-tests PRIVATE vocr_find_lines_rows=vjo_test_find_lines_rows)
  target_link_libraries(vjo-vocr-mode-tests vjo_vocr_rt vjo_host)
  target_compile_options(vjo-vocr-mode-tests PRIVATE -Wall -Wextra)
  add_test(NAME vocr_finder_dispatch COMMAND vjo-vocr-mode-tests)
  # The weights are a release asset, not part of this repository: run the
  # model checks when VJO_VOCR_MODEL_DIR holds them (tools/prepare_ocr.py --backend vocr).
  if(VJO_VOCR_MODEL_DIR)
    add_test(NAME vocr_model COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_local_ocr.py
      --cli $<TARGET_FILE:vjo-ocr> --backend vocr --model ${VJO_VOCR_MODEL_DIR})
  endif()
endif()
add_test(NAME local_dictionary COMMAND ${Python3_EXECUTABLE} -m unittest discover
  -s ${CMAKE_SOURCE_DIR}/tools/tests -v)
# The real self-test writer (shell/selftest.c) on the host: v2 reports for the analyser's tests.
add_executable(vjo-selftest-writer host/tests/selftest_writer.c)
target_include_directories(vjo-selftest-writer PRIVATE host/tests/vita_stubs)
target_compile_definitions(vjo-selftest-writer PRIVATE VJO_HOST=1)
target_link_libraries(vjo-selftest-writer vjo_core)
set(_vjo_tool_env "VJO_CLI=$<TARGET_FILE:vjo-cli>;VJO_BENCH=$<TARGET_FILE:vjo-local-bench>")
string(APPEND _vjo_tool_env ";VJO_SELFTEST_WRITER=$<TARGET_FILE:vjo-selftest-writer>")
if(VJO_WITH_VOCR)
  string(APPEND _vjo_tool_env ";VJO_CAPTURE_PARITY=$<TARGET_FILE:vjo-capture-parity-tests>")
endif()
set_tests_properties(local_dictionary PROPERTIES ENVIRONMENT "${_vjo_tool_env}")


# Counts production file reads and times lookup only (no OCR or process startup).
add_executable(vjo-local-bench host/local_bench.c)
target_link_libraries(vjo-local-bench vjo_host)

add_executable(vjo-games-tests host/tests/test_games.c vita/games_storage.c)
target_include_directories(vjo-games-tests PRIVATE third_party/acutest/include host/tests/games_stubs)
target_link_libraries(vjo-games-tests vjo_host)
target_compile_options(vjo-games-tests PRIVATE -Wall -Wextra)
add_test(NAME games COMMAND vjo-games-tests)
add_executable(vjo-preferences-tests host/tests/test_preferences.c settings/preferences.c shell/files.c)
target_include_directories(vjo-preferences-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_link_libraries(vjo-preferences-tests vjo_host)
target_compile_options(vjo-preferences-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME preferences COMMAND vjo-preferences-tests)
add_executable(vjo-game-input-tests host/tests/test_game_input.c kernel/triggers.c)
target_include_directories(vjo-game-input-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_link_libraries(vjo-game-input-tests vjo_host)
target_compile_options(vjo-game-input-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME game_input COMMAND vjo-game-input-tests)

# Real role-restricted job transport; no simulated inference/accuracy claims.
add_executable(vjo-kernel-game-ocr-tests host/tests/test_kernel_game_ocr.c kernel/scene.c kernel/ledger.c core/ledger.c)
target_include_directories(vjo-kernel-game-ocr-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_definitions(vjo-kernel-game-ocr-tests PRIVATE VJO_HOST=1)
target_compile_options(vjo-kernel-game-ocr-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME kernel_game_ocr COMMAND vjo-kernel-game-ocr-tests)
add_executable(vjo-game-ocr-client-tests host/tests/test_game_ocr_client.c)
target_include_directories(vjo-game-ocr-client-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-game-ocr-client-tests vjo_host)
add_test(NAME game_ocr_client COMMAND vjo-game-ocr-client-tests)

if(VJO_WITH_VOCR)
  add_executable(vjo-game-vocr-tests host/tests/test_game_vocr.c)
  target_include_directories(vjo-game-vocr-tests PRIVATE third_party/acutest/include host/tests/game_stubs)
  target_compile_options(vjo-game-vocr-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(vjo-game-vocr-tests PRIVATE VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  target_link_libraries(vjo-game-vocr-tests vjo_vocr)
  add_test(NAME game_vocr_worker COMMAND vjo-game-vocr-tests)
  # The actual game worker and core adapter with synthetic pinned weights and a
  # spy runtime: the resident-weights lifecycle runs without release files (CI).
  add_executable(vjo-game-resident-tests host/tests/test_game_resident.c core/vocr_ocr.c)
  target_include_directories(vjo-game-resident-tests PRIVATE third_party/acutest/include
    host/tests/game_stubs host/tests)
  target_compile_options(vjo-game-resident-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(vjo-game-resident-tests PRIVATE
    VJO_VOCR_TEST_MODELS="test_resident_models.h"
    vocr_find_lines_rows=test_vocr_find_lines_rows vocr_load=test_vocr_load
    vocr_arena_bytes=test_vocr_arena_bytes vocr_init=test_vocr_init vocr_run=test_vocr_run)
  target_link_libraries(vjo-game-resident-tests vjo_vocr_rt vjo_host)
  add_test(NAME game_resident_weights COMMAND vjo-game-resident-tests)
  add_executable(vjo-game-model-broker-tests host/tests/test_game_model_broker.c
    host/tests/kernel_model_broker_boundary.c kernel/capture_request.c kernel/buffers.c kernel/capture.c
    kernel/scene.c kernel/ledger.c core/ledger.c)
  target_include_directories(vjo-game-model-broker-tests PRIVATE third_party/acutest/include
    host/tests/game_stubs host/tests/vita_stubs)
  target_compile_options(vjo-game-model-broker-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(vjo-game-model-broker-tests PRIVATE VJO_HOST=1 VJO_TEST_MODEL_BROKER=1
    VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  target_link_libraries(vjo-game-model-broker-tests vjo_vocr)
  # #74 preflight: the real worker and kernel under the console's measured pool limits.
  add_executable(vjo-alloc-sim-tests host/tests/test_alloc_sim.c host/tests/kernel_model_broker_boundary.c
    kernel/capture_request.c kernel/buffers.c kernel/capture.c kernel/scene.c kernel/ledger.c core/ledger.c)
  target_include_directories(vjo-alloc-sim-tests PRIVATE third_party/acutest/include host/tests/game_stubs
    host/tests/vita_stubs)
  target_compile_options(vjo-alloc-sim-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(vjo-alloc-sim-tests PRIVATE VJO_HOST=1 VJO_TEST_MODEL_BROKER=1
    VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  target_link_libraries(vjo-alloc-sim-tests vjo_vocr)
  add_test(NAME alloc_sim COMMAND vjo-alloc-sim-tests)
  add_test(NAME game_model_broker COMMAND vjo-game-model-broker-tests)
endif()

# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Muse wake word: the SDK-owned session core, audio rings, LFBE frontend,
# detector, and ExecuTorch runner. models/hey_muse.pte is embedded here;
# its export lives in tools/export_hey_muse.py. With
# MUSE_WAKEWORD_ENABLE=n this registers empty, like the muse component in
# Link-only builds, so default firmware is unchanged.
set(srcs)
# Requirements are collected by ESP-IDF in an early-expansion pass before
# Kconfig values exist, so executorch must be declared unconditionally.
# This does not compile the runtime in by itself: the executorch
# component still registers empty unless ET_ENABLE is selected.
set(priv_requires executorch esp_timer)
set(gen_include_dir)
if(CONFIG_MUSE_WAKEWORD_ENABLE)
    list(APPEND srcs "muse_wakeword_core.c")
    # Exactly one runner implementation: each defines the ww_runner_* API.
    if(CONFIG_MUSE_WAKEWORD_RUNNER_BENCH)
        list(APPEND srcs "ww_runner_bench.c")
    else()
        list(APPEND srcs "ww_detector.c" "ww_frontend.c" "ww_runner_et.cpp")
        # ExecuTorch's Program::load needs the .pte 8-byte aligned and
        # EMBED_FILES only guarantees 4, so generate an aligned array.
        set(pte_c "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.c")
        set(pte_h "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.h")
        add_custom_command(OUTPUT ${pte_c} ${pte_h}
            COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_pte.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte"
                "${CMAKE_CURRENT_BINARY_DIR}" hey_muse_pte
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_pte.py"
            VERBATIM)
        list(APPEND srcs ${pte_c})
        set(gen_include_dir "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" ${gen_include_dir}
    PRIV_REQUIRES ${priv_requires}
)

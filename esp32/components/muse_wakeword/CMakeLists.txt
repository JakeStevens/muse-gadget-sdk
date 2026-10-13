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

# Muse wake word (esp32-design.md §6.1): the SDK-owned session core, audio
# rings, and energy endpointing. The ExecuTorch runner (D1/D2) and "Marvin"
# model (D3) plug in behind include/ww_runner.h when they land: EMBED_FILES
# for models/*.pte and a PRIV_REQUIRES on the executorch component arrive
# then, together, in one vendoring change (§9.2). Until then
# ww_runner_fake.c provides the contract and cannot detect anything.
# With MUSE_WAKEWORD_ENABLE=n this registers empty, like the muse
# component in Link-only builds, so default firmware is unchanged.
set(srcs)
if(CONFIG_MUSE_WAKEWORD_ENABLE)
    list(APPEND srcs "muse_wakeword_core.c" "ww_runner_fake.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
)

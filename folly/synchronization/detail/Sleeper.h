/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#include <folly/portability/Asm.h>

namespace folly {

//////////////////////////////////////////////////////////////////////

namespace detail {

/*
 * A helper object for the contended case. Starts off with eager
 * spinning, and falls back to sleeping for small quantums.
 */
class Sleeper {
  const std::chrono::nanoseconds delta;

  static constexpr uint32_t kMaxActiveSpin = 4096;
  static constexpr bool useBackOff = kIsArchAArch64 && !kIsMobile;

  uint32_t spinCount = 0;

  uint32_t spinCountTarget = 1;

 public:
  static constexpr std::chrono::nanoseconds kMinYieldingSleep =
      std::chrono::microseconds(500);

  constexpr Sleeper() noexcept : delta(kMinYieldingSleep) {}

  explicit Sleeper(std::chrono::nanoseconds d) noexcept : delta(d) {}

  void wait() noexcept {
    bool doSpin = useBackOff
        ? spinCountTarget <= kMaxActiveSpin
        : spinCount < kMaxActiveSpin;
    if (doSpin) {
      if constexpr (useBackOff) {
        do {
          asm_volatile_pause();
        } while (++spinCount < spinCountTarget);
        spinCountTarget <<= 1;
      } else {
        ++spinCount;
        asm_volatile_pause();
      }
    } else {
      /* sleep override */
      std::this_thread::sleep_for(delta);
    }
  }
};

} // namespace detail
} // namespace folly

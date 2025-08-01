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

#include <folly/Fingerprint.h>

#include <folly/Portability.h>
#include <folly/detail/FingerprintPolynomial.h>

#include <utility>

namespace folly {
namespace detail {

namespace {

// The polynomials below were generated by a separate program that requires the
// NTL (Number Theory Library) from http://www.shoup.net/ntl/
//
// Briefly: randomly generate a polynomial of degree D, test for
// irreducibility, repeat until you find an irreducible polynomial
// (roughly 1/D of all polynomials of degree D are irreducible, so
// this will succeed in D/2 tries on average; D is small (64..128) so
// this simple method works well)
//
// DO NOT REPLACE THE POLYNOMIALS USED, EVER, as that would change the value
// of every single fingerprint in existence.
template <size_t Deg>
struct FingerprintTablePoly;
template <>
struct FingerprintTablePoly<63> {
  static constexpr uint64_t data[1] = {0xbf3736b51869e9b7};
};
template <>
struct FingerprintTablePoly<95> {
  static constexpr uint64_t data[2] = {0x51555cb0aa8d39c3, 0xb679ec3700000000};
};
template <>
struct FingerprintTablePoly<127> {
  static constexpr uint64_t data[2] = {0xc91bff9b8768b51b, 0x8c5d5853bd77b0d3};
};

template <typename D, size_t S0, size_t... I0>
constexpr auto copy_table(D const (&table)[S0], std::index_sequence<I0...>) {
  using array = std::array<D, S0>;
  return array{{table[I0]...}};
}
template <typename D, size_t S0>
constexpr auto copy_table(D const (&table)[S0]) {
  return copy_table(table, std::make_index_sequence<S0>{});
}

template <typename D, size_t S0, size_t S1, size_t... I0>
constexpr auto copy_table(
    D const (&table)[S0][S1], std::index_sequence<I0...>) {
  using array = std::array<std::array<D, S1>, S0>;
  return array{{copy_table(table[I0])...}};
}
template <typename D, size_t S0, size_t S1>
constexpr auto copy_table(D const (&table)[S0][S1]) {
  return copy_table(table, std::make_index_sequence<S0>{});
}

template <typename D, size_t S0, size_t S1, size_t S2, size_t... I0>
constexpr auto copy_table(
    D const (&table)[S0][S1][S2], std::index_sequence<I0...>) {
  using array = std::array<std::array<std::array<D, S2>, S1>, S0>;
  return array{{copy_table(table[I0])...}};
}
template <typename D, size_t S0, size_t S1, size_t S2>
constexpr auto copy_table(D const (&table)[S0][S1][S2]) {
  return copy_table(table, std::make_index_sequence<S0>{});
}

template <size_t Deg>
constexpr poly_table<Deg> make_poly_table() {
  FingerprintPolynomial<Deg> poly(FingerprintTablePoly<Deg>::data);
  uint64_t table[8][256][poly_size(Deg)] = {};
  // table[i][q] is Q(X) * X^(k+8*i) mod P(X),
  // where k is the number of bits in the fingerprint (and deg(P)) and
  // Q(X) = q7*X^7 + q6*X^6 + ... + q1*X + q0 is a degree-7 polynomial
  // whose coefficients are the bits of q.
  for (uint16_t x = 0; x < 256; x++) {
    FingerprintPolynomial<Deg> t;
    t.setHigh8Bits(uint8_t(x));
    for (auto& entry : table) {
      t.mulXkmod(8, poly);
      for (size_t j = 0; j < poly_size(Deg); ++j) {
        entry[x][j] = t.get(j);
      }
    }
  }
  return copy_table(table);
}

// private global variables marked constexpr to enforce that make_poly_table is
// really invoked at constexpr time, which would not otherwise be guaranteed
FOLLY_STORAGE_CONSTEXPR auto const poly_table_63 = make_poly_table<63>();
FOLLY_STORAGE_CONSTEXPR auto const poly_table_95 = make_poly_table<95>();
FOLLY_STORAGE_CONSTEXPR auto const poly_table_127 = make_poly_table<127>();

} // namespace

template <>
const uint64_t FingerprintTable<64>::poly[poly_size(64)] = {
    FingerprintTablePoly<63>::data[0]};
template <>
const uint64_t FingerprintTable<96>::poly[poly_size(96)] = {
    FingerprintTablePoly<95>::data[0], FingerprintTablePoly<95>::data[1]};
template <>
const uint64_t FingerprintTable<128>::poly[poly_size(128)] = {
    FingerprintTablePoly<127>::data[0], FingerprintTablePoly<127>::data[1]};

template <>
const poly_table<64> FingerprintTable<64>::table = poly_table_63;
template <>
const poly_table<96> FingerprintTable<96>::table = poly_table_95;
template <>
const poly_table<128> FingerprintTable<128>::table = poly_table_127;

} // namespace detail
} // namespace folly

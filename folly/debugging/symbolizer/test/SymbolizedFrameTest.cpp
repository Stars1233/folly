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

#include <folly/debugging/symbolizer/SymbolizedFrame.h>

#include <glog/logging.h>

#include <folly/portability/GTest.h>

using namespace folly::symbolizer;

void checkPath(
    std::string expectedPath,
    std::string expectedBaseDir,
    std::string expectedSubDir,
    std::string expectedFile,
    std::string rawBaseDir,
    std::string rawSubDir,
    std::string rawFile) {
  Path path(rawBaseDir, rawSubDir, rawFile);

  EXPECT_EQ(expectedBaseDir, path.baseDir())
      << "Path(" << rawBaseDir << ", " << rawSubDir << ", " << rawFile << ")";
  EXPECT_EQ(expectedSubDir, path.subDir())
      << "Path(" << rawBaseDir << ", " << rawSubDir << ", " << rawFile << ")";
  EXPECT_EQ(expectedFile, path.file())
      << "Path(" << rawBaseDir << ", " << rawSubDir << ", " << rawFile << ")";

  EXPECT_EQ(expectedPath, path.toString());

  // Check the `toBuffer` function.
  char buf[1024];
  size_t len;
  len = path.toBuffer(buf, 1024);
  EXPECT_EQ(expectedPath, std::string(buf, len));
}

TEST(SymbolizedFrame, Path) {
  checkPath("hello.cpp", "", "", "hello.cpp", "", "", "hello.cpp");
  checkPath("foo/hello.cpp", "foo", "", "hello.cpp", "foo", "", "hello.cpp");
  checkPath("foo/hello.cpp", "foo", "", "hello.cpp", "", "foo", "hello.cpp");
  checkPath(
      "./////./////hello.cpp",
      "./////",
      "./////",
      "hello.cpp",
      "./////",
      "./////",
      "hello.cpp");
  checkPath(
      "/////./////hello.cpp",
      "/////",
      "./////",
      "hello.cpp",
      "/////",
      "./////",
      "hello.cpp");
  checkPath(
      "/./././././././hello.cpp",
      "/./././././././",
      "",
      "hello.cpp",
      "/./././././././",
      "",
      "hello.cpp");
}

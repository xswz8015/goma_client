// Copyright 2019 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "elf_util.h"

#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "gtest/gtest.h"

namespace devtools_goma {

TEST(ElfDepParserTest, ParseReadElf) {
  static constexpr absl::string_view kExampleOutput =
      "Dynamic section at offset 0x1d91b10 contains 31 entries:\n"
      "  Tag        Type                         Name/Value\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libLLVM-8svn.so]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libc++.so.1]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libc++abi.so.1]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libm.so.6]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libc.so.6]\n"
      " 0x000000000000000f (RPATH)              Library rpath: "
      "[$ORIGIN/../lib64]\n"
      " 0x000000000000000c (INIT)               0x632738\n"
      " 0x000000000000000d (FINI)               0x1c155d4\n"
      " 0x0000000000000019 (INIT_ARRAY)         0x22939d8\n"
      " 0x000000000000001b (INIT_ARRAYSZ)       40 (bytes)\n"
      " 0x000000000000001a (FINI_ARRAY)         0x2293a00\n"
      " 0x000000000000001c (FINI_ARRAYSZ)       8 (bytes)\n"
      " 0x000000006ffffef5 (GNU_HASH)           0x400298\n"
      " 0x0000000000000005 (STRTAB)             0x48ec28\n"
      " 0x0000000000000006 (SYMTAB)             0x41c280\n"
      " 0x000000000000000a (STRSZ)              1561579 (bytes)\n"
      " 0x000000000000000b (SYMENT)             24 (bytes)\n"
      " 0x0000000000000015 (DEBUG)              0x0\n"
      " 0x0000000000000003 (PLTGOT)             0x2391d50\n"
      " 0x0000000000000002 (PLTRELSZ)           144 (bytes)\n"
      " 0x0000000000000014 (PLTREL)             RELA\n"
      " 0x0000000000000017 (JMPREL)             0x6326a8\n"
      " 0x0000000000000007 (RELA)               0x615968\n"
      " 0x0000000000000008 (RELASZ)             118080 (bytes)\n"
      " 0x0000000000000009 (RELAENT)            24 (bytes)\n"
      " 0x0000000000000018 (BIND_NOW)           \n"
      " 0x000000006ffffffb (FLAGS_1)            Flags: NOW\n"
      " 0x000000006ffffffe (VERNEED)            0x6158e8\n"
      " 0x000000006fffffff (VERNEEDNUM)         3\n"
      " 0x000000006ffffff0 (VERSYM)             0x60c014\n"
      " 0x0000000000000000 (NULL)               0x0\n";

  std::vector<absl::string_view> libs;
  std::vector<absl::string_view> rpaths;
  EXPECT_TRUE(ElfDepParser::ParseReadElf(kExampleOutput, &libs, &rpaths));
  std::vector<absl::string_view> expected_libs = {
      "libLLVM-8svn.so", "libc++.so.1", "libc++abi.so.1",
      "libm.so.6",       "libc.so.6",
  };
  std::vector<absl::string_view> expected_rpaths = {
      "$ORIGIN/../lib64",
  };
  EXPECT_EQ(expected_libs, libs);
  EXPECT_EQ(expected_rpaths, rpaths);
}

TEST(ElfDepParserTest, ParseReadElfRunPath) {
  static constexpr absl::string_view kExampleOutput =
      "Dynamic section at offset 0x1e3ca20 contains 33 entries:\n"
      "  Tag        Type                         Name/Value\n"
      " 0x0000000000000003 (PLTGOT)             0x1e3dfe8\n"
      " 0x0000000000000002 (PLTRELSZ)           37584 (bytes)\n"
      " 0x0000000000000017 (JMPREL)             0x3be3f8\n"
      " 0x0000000000000014 (PLTREL)             RELA\n"
      " 0x0000000000000007 (RELA)               0x226e60\n"
      " 0x0000000000000008 (RELASZ)             1668504 (bytes)\n"
      " 0x0000000000000009 (RELAENT)            24 (bytes)\n"
      " 0x000000006ffffff9 (RELACOUNT)          66125\n"
      " 0x0000000000000015 (DEBUG)              0x0\n"
      " 0x0000000000000006 (SYMTAB)             0x298\n"
      " 0x000000000000000b (SYMENT)             24 (bytes)\n"
      " 0x0000000000000005 (STRTAB)             0x84ef8\n"
      " 0x000000000000000a (STRSZ)              1500329 (bytes)\n"
      " 0x000000006ffffef5 (GNU_HASH)           0x1f33a8\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libpthread.so.0]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libLLVM-4.0.so.1]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libjsoncpp.so.1]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libstdc++.so.6]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[libgcc_s.so.1]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]\n"
      " 0x0000000000000001 (NEEDED)             Shared library: "
      "[ld-linux-x86-64.so.2]\n"
      " 0x000000000000000c (INIT)               0x3c76c8\n"
      " 0x000000000000000d (FINI)               0x193dbbc\n"
      " 0x000000000000001a (FINI_ARRAY)         0x1e02c20\n"
      " 0x000000000000001c (FINI_ARRAYSZ)       8 (bytes)\n"
      " 0x0000000000000019 (INIT_ARRAY)         0x1e02c28\n"
      " 0x000000000000001b (INIT_ARRAYSZ)       200 (bytes)\n"
      " 0x000000000000001d (RUNPATH)            Library runpath: "
      "[$ORIGIN/../lib]\n"
      " 0x000000006ffffff0 (VERSYM)             0x21bba4\n"
      " 0x000000006ffffffe (VERNEED)            0x226cac\n"
      " 0x000000006fffffff (VERNEEDNUM)         7\n"
      " 0x0000000000000000 (NULL)               0x0\n";

  std::vector<absl::string_view> libs;
  std::vector<absl::string_view> rpaths;
  EXPECT_TRUE(ElfDepParser::ParseReadElf(kExampleOutput, &libs, &rpaths));
  std::vector<absl::string_view> expected_libs = {
      "libpthread.so.0", "libLLVM-4.0.so.1",
      "libjsoncpp.so.1", "libstdc++.so.6",
      "libm.so.6",       "libgcc_s.so.1",
      "libc.so.6",       "ld-linux-x86-64.so.2",
  };
  std::vector<absl::string_view> expected_rpaths = {
      "$ORIGIN/../lib",
  };
  EXPECT_EQ(expected_libs, libs);
  EXPECT_EQ(expected_rpaths, rpaths);
}

// TODO: write test for GetDeps.
//
TEST(ElfUtilTest, ParseLdSoConf) {
  static constexpr absl::string_view kExampleContent =
      "# ld.so.conf autogenerated by env-update; make all changes to\n"
      "# contents of /etc/env.d directory\n"
      "/lib64\n"
      "/usr/lib64\n"
      "/usr/local/lib64\n"
      "/lib32\n"
      "/usr/lib32\n"
      "/usr/local/lib32\n"
      "/lib\n"
      "/usr/lib\n"
      "/usr/local/lib\n"
      "include ld.so.conf.d/*.conf\n"
      "/usr/lib/gcc/x86_64-pc-linux-gnu/4.9.x/32\n"
      "/usr/lib/gcc/x86_64-pc-linux-gnu/4.9.x\n"
      "/usr/x86_64-pc-linux-gnu/lib\n";
  std::vector<std::string> result = ParseLdSoConf(kExampleContent);
  std::vector<std::string> expected_result = {
      "/lib64",
      "/usr/lib64",
      "/usr/local/lib64",
      "/lib32",
      "/usr/lib32",
      "/usr/local/lib32",
      "/lib",
      "/usr/lib",
      "/usr/local/lib",
      "/usr/lib/gcc/x86_64-pc-linux-gnu/4.9.x/32",
      "/usr/lib/gcc/x86_64-pc-linux-gnu/4.9.x",
      "/usr/x86_64-pc-linux-gnu/lib",
  };
  EXPECT_EQ(expected_result, result);
}

}  // namespace devtools_goma
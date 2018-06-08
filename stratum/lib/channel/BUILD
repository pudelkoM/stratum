#
# Copyright 2018 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

licenses(["notice"])  # Apache v2

exports_files(["LICENSE"])

# This package contains message-passing libraries for Hercules.

load(
    "//stratum/portage:build_defs.bzl",
    "sc_cc_lib",
    "sc_cc_test",
    "STRATUM_INTERNAL",
)

package(
    default_hdrs_check = "strict",
    default_visibility = STRATUM_INTERNAL,
)

sc_cc_lib(
    name = "channel",
    hdrs = [
        "channel.h",
    ],
    deps = [
        "//absl/base:core_headers",
        "//absl/synchronization",
        "//absl/time",
        "//stratum/glue:logging",
        "//stratum/glue/status",
        "//stratum/glue/status:status_macros",
        "//stratum/lib:macros",
        "//stratum/public/lib:error",
    ],
)

sc_cc_lib(
    name = "channel_mock",
    testonly = 1,
    hdrs = [
        "channel_mock.h",
    ],
    deps = [
        ":channel",
        "//testing/base/public:gmock_no_google3",
    ],
)

cc_library(
    name = "test_main",
    testonly = 1,
    srcs = ["test_main.cc"],
    deps = [
        "//base",
        "//testing/base/public:gunit_no_google3",
        "//stratum/glue:init_google",
        "//stratum/glue:logging",
    ],
)

sc_cc_test(
    name = "channel_test",
    srcs = [
        "channel_test.cc",
    ],
    deps = [
        ":channel",
        ":test_main",
        "//testing/base/public:gunit_no_google3",
        "//absl/synchronization",
        "//stratum/glue/status:status_test_util",
    ],
)
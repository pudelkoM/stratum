#
# Copyright 2019 Google LLC
# Copyright 2019-present Open Networking Foundation
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

"""Build rules to create Golang code from an Antlr4 grammar."""

load("@io_bazel_rules_go//go:def.bzl", "go_library")

def go_antlr4_combined(name, src):
  """Generates the Go source coresponding to an antlr4 grammar.

  Args:
    name: The name of the package to use for the go_library.
    src: The antlr4 g4 file containing the lexer and parser grammar.
  """
  if not src.endswith(".g4"):
    fail("Source file must be an Antlr4 grammar ending with .g4.", "src")
  prefix = src[:-3]
  suffixes = ("%s_base_visitor.go", "%s_visitor.go", "%s_base_listener.go", "%s_listener.go")
  outs = ["%s_lexer.go" % _strip_end(prefix, "Lexer").lower()]
  outs += ["%s_parser.go" % _strip_end(prefix, "Parser").lower()]
  outs += [suffix % prefix.lower() for suffix in suffixes]
  native.genrule(
    name = name + "_src",
    srcs = [src],
    outs = outs,
    cmd = (
      "mkdir $$$$.tmp ; " + "cp $(SRCS) $$$$.tmp/ ; " + "cd $$$$.tmp ; " +
      ("../$(location :antlr_tool) " +
          src + " -Dlanguage=Go " +
          " -visitor -listener " + " -package " + name + " ; ") +
      "cd .. ; " + (
      "".join(
          [
            " cp $$$$.tmp/%s $(@D)/ ;" % filepath
            for filepath in outs
          ],
        )
      ) +
      "rm -rf $$$$.tmp"
    ),
    tools = [
      ":antlr_tool",
    ],
  )
  go_library(
    name = name,
    importpath = name,
    srcs = [f for f in outs if f.endswith(".go")],
    deps = [
      "@com_github_antlr//runtime/Go/antlr:go_default_library",
    ],
  )
  native.java_binary(
    name = "antlr_tool",
    jvm_flags = ["-Xmx256m"],
    main_class = "org.antlr.v4.Tool",
    runtime_deps = [
        "@antlr4_tool//jar:jar",
        "@antlr4_runtime//jar:jar",
        "@antlr3_runtime//jar:jar",
        "@stringtemplate4//jar:jar",
        "@javax_json//jar:jar",
    ],
  )

def _strip_end(text, suffix):
  if not text.endswith(suffix):
    return text
  return text[:len(text) - len(suffix)]

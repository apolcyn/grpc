cc_binary(
  name = "dig",
  srcs = glob([
    "bin/dig/**/*.c",
  ]),
  deps = [":dig_lib"],
  visibility = [
      "//visibility:public",
  ],
)

cc_library(
  name = "dig_lib",
  hdrs = glob([
    "libs/**/*.h",
    "config.h",
  ]),
  srcs = glob([
    "libs/**/*.c",
  ]),
)

genrule(
  name = "config_h",
  outs = ["config.h"],
  cmd = ["./$(location configure) > \"$@\""],
)

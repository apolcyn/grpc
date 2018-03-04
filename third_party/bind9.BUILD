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
  ]),
  srcs = glob([
    "libs/**/*.c",
  ]),
)

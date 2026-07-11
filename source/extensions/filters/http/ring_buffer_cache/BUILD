load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
    "envoy_proto_library",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_proto_library(
    name = "config_proto",
    srcs = ["config.proto"],
)

envoy_cc_extension(
    name = "config",
    srcs = [
        "config.cc",
        "ring_buffer_cache.cc",
    ],
    hdrs = [
	    "config.h",
        "ring_buffer_cache.h",
    ],
    deps = [
        ":config_proto_cc_proto",
        "//source/extensions/filters/http/common:factory_base_lib",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "@abseil-cpp//absl/container:flat_hash_map",
    ],
)

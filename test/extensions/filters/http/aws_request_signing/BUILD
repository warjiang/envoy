load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "aws_request_signing_filter_test",
    srcs = ["aws_request_signing_filter_test.cc"],
    extension_names = ["envoy.filters.http.aws_request_signing"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/aws_request_signing:aws_request_signing_filter_lib",
        "//test/extensions/common/aws:aws_mocks",
        "//test/mocks/http:http_mocks",
    ],
)

envoy_extension_cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    extension_names = ["envoy.filters.http.aws_request_signing"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/aws_request_signing:config",
        "//test/mocks/server:factory_context_mocks",
        "@envoy_api//envoy/extensions/filters/http/aws_request_signing/v3:pkg_cc_proto",
    ],
)

envoy_extension_cc_test(
    name = "aws_request_signing_integration_test",
    srcs = ["aws_request_signing_integration_test.cc"],
    extension_names = ["envoy.filters.http.aws_request_signing"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/clusters/logical_dns:logical_dns_cluster_lib",
        "//source/extensions/filters/http/aws_request_signing:aws_request_signing_filter_lib",
        "//source/extensions/filters/http/aws_request_signing:config",
        "//test/extensions/common/aws:aws_mocks",
        "//test/integration:http_integration_lib",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/http/aws_request_signing/v3:pkg_cc_proto",
    ],
)

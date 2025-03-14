#include "source/extensions/filters/http/header_to_metadata/header_to_metadata_filter.h"

#include "envoy/extensions/filters/http/header_to_metadata/v3/header_to_metadata.pb.h"

#include "source/common/common/base64.h"
#include "source/common/common/regex.h"
#include "source/common/config/well_known_names.h"
#include "source/common/http/header_utility.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/extensions/filters/http/well_known_names.h"

#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HeaderToMetadataFilter {

// Extract the value of the header.
absl::optional<std::string> HeaderValueSelector::extract(Http::HeaderMap& map) const {
  const auto header_value = Http::HeaderUtility::getAllOfHeaderAsString(map, header_);
  if (!header_value.result().has_value()) {
    return absl::nullopt;
  }
  // Catch the value in the header before removing.
  absl::optional<std::string> value = std::string(header_value.result().value());
  if (remove_) {
    map.remove(header_);
  }
  return value;
}

// Extract the value of the key from the cookie header.
absl::optional<std::string> CookieValueSelector::extract(Http::HeaderMap& map) const {
  std::string value = Envoy::Http::Utility::parseCookieValue(map, cookie_);
  if (!value.empty()) {
    return {std::move(value)};
  }
  return absl::nullopt;
}

absl::StatusOr<Rule> Rule::create(const ProtoRule& rule, Regex::Engine& regex_engine) {
  absl::Status creation_status = absl::OkStatus();
  auto r = Rule(rule, regex_engine, creation_status);
  RETURN_IF_NOT_OK_REF(creation_status);
  return r;
}

Rule::Rule(const ProtoRule& rule, Regex::Engine& regex_engine, absl::Status& creation_status)
    : rule_(rule) {
  // Ensure only one of header and cookie is specified.
  // TODO(radha13): remove this once we are on v4 and these fields are folded into a oneof.
  if (!rule.cookie().empty() && !rule.header().empty()) {
    creation_status =
        absl::InvalidArgumentError("Cannot specify both header and cookie in the same rule");
    return;
  }

  // Initialize the shared pointer.
  if (!rule.header().empty()) {
    selector_ =
        std::make_shared<HeaderValueSelector>(Http::LowerCaseString(rule.header()), rule.remove());
  } else if (!rule.cookie().empty()) {
    selector_ = std::make_shared<CookieValueSelector>(rule.cookie());
  } else {
    creation_status =
        absl::InvalidArgumentError("One of Cookie or Header option needs to be specified");
    return;
  }

  // Rule must have at least one of the `on_header_*` fields set.
  if (!rule.has_on_header_present() && !rule.has_on_header_missing()) {
    creation_status =
        absl::InvalidArgumentError(fmt::format("header to metadata filter: rule for {} has neither "
                                               "`on_header_present` nor `on_header_missing` set",
                                               selector_->toString()));
    return;
  }

  // Ensure value and regex_value_rewrite are not mixed.
  // TODO(rgs1): remove this once we are on v4 and these fields are folded into a oneof.
  if (!rule.on_header_present().value().empty() &&
      rule.on_header_present().has_regex_value_rewrite()) {
    creation_status =
        absl::InvalidArgumentError("Cannot specify both value and regex_value_rewrite");
    return;
  }

  // Remove field is un-supported for cookie.
  if (!rule.cookie().empty() && rule.remove()) {
    creation_status = absl::InvalidArgumentError("Cannot specify remove for cookie");
    return;
  }

  if (rule.has_on_header_missing() && rule.on_header_missing().value().empty()) {
    creation_status =
        absl::InvalidArgumentError("Cannot specify on_header_missing rule with an empty value");
    return;
  }

  if (rule.on_header_present().has_regex_value_rewrite()) {
    const auto& rewrite_spec = rule.on_header_present().regex_value_rewrite();
    auto regex_rewrite_or = Regex::Utility::parseRegex(rewrite_spec.pattern(), regex_engine);
    SET_AND_RETURN_IF_NOT_OK(regex_rewrite_or.status(), creation_status);
    regex_rewrite_ = std::move(regex_rewrite_or.value());
    regex_rewrite_substitution_ = rewrite_spec.substitution();
  }
}

absl::StatusOr<ConfigSharedPtr>
Config::create(const envoy::extensions::filters::http::header_to_metadata::v3::Config& config,
               Regex::Engine& regex_engine, bool per_route) {
  absl::Status creation_status = absl::OkStatus();
  auto cfg = ConfigSharedPtr(new Config(config, regex_engine, per_route, creation_status));
  RETURN_IF_NOT_OK_REF(creation_status);
  return cfg;
}

Config::Config(const envoy::extensions::filters::http::header_to_metadata::v3::Config config,
               Regex::Engine& regex_engine, const bool per_route, absl::Status& creation_status) {
  absl::StatusOr<bool> request_set_or =
      Config::configToVector(config.request_rules(), request_rules_, regex_engine);
  SET_AND_RETURN_IF_NOT_OK(request_set_or.status(), creation_status);
  request_set_ = request_set_or.value();

  absl::StatusOr<bool> response_set_or =
      Config::configToVector(config.response_rules(), response_rules_, regex_engine);
  SET_AND_RETURN_IF_NOT_OK(response_set_or.status(), creation_status);
  response_set_ = response_set_or.value();

  // Note: empty configs are fine for the global config, which would be the case for enabling
  //       the filter globally without rules and then applying them at the virtual host or
  //       route level. At the virtual or route level, it makes no sense to have an empty
  //       config so we return an error.
  if (per_route && !response_set_ && !request_set_) {
    creation_status = absl::InvalidArgumentError("header_to_metadata_filter: Per filter configs "
                                                 "must at least specify either request or response "
                                                 "rules");
  }
}

absl::StatusOr<bool> Config::configToVector(const ProtobufRepeatedRule& proto_rules,
                                            HeaderToMetadataRules& vector,
                                            Regex::Engine& regex_engine) {
  if (proto_rules.empty()) {
    ENVOY_LOG(debug, "no rules provided");
    return false;
  }

  for (const auto& entry : proto_rules) {
    auto rule = Rule::create(entry, regex_engine);
    RETURN_IF_NOT_OK_REF(rule.status());
    vector.emplace_back(std::move(rule.value()));
  }

  return true;
}

HeaderToMetadataFilter::HeaderToMetadataFilter(const ConfigSharedPtr config) : config_(config) {}

HeaderToMetadataFilter::~HeaderToMetadataFilter() = default;

Http::FilterHeadersStatus HeaderToMetadataFilter::decodeHeaders(Http::RequestHeaderMap& headers,
                                                                bool) {
  const auto* config = getConfig();
  if (config->doRequest()) {
    writeHeaderToMetadata(headers, config->requestRules(), *decoder_callbacks_);
  }

  return Http::FilterHeadersStatus::Continue;
}

void HeaderToMetadataFilter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

Http::FilterHeadersStatus HeaderToMetadataFilter::encodeHeaders(Http::ResponseHeaderMap& headers,
                                                                bool) {
  const auto* config = getConfig();
  if (config->doResponse()) {
    writeHeaderToMetadata(headers, config->responseRules(), *encoder_callbacks_);
  }
  return Http::FilterHeadersStatus::Continue;
}

void HeaderToMetadataFilter::setEncoderFilterCallbacks(
    Http::StreamEncoderFilterCallbacks& callbacks) {
  encoder_callbacks_ = &callbacks;
}

bool HeaderToMetadataFilter::addMetadata(StructMap& struct_map, const std::string& meta_namespace,
                                         const std::string& key, std::string value, ValueType type,
                                         ValueEncode encode) const {
  ProtobufWkt::Value val;

  ASSERT(!value.empty());

  if (value.size() >= MAX_HEADER_VALUE_LEN) {
    // Too long, go away.
    ENVOY_LOG(debug, "metadata value is too long");
    return false;
  }

  if (encode == envoy::extensions::filters::http::header_to_metadata::v3::Config::BASE64) {
    value = Base64::decodeWithoutPadding(value);
    if (value.empty()) {
      ENVOY_LOG(debug, "Base64 decode failed");
      return false;
    }
  }

  // Sane enough, add the key/value.
  switch (type) {
    PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
  case envoy::extensions::filters::http::header_to_metadata::v3::Config::STRING:
    val.set_string_value(std::move(value));
    break;
  case envoy::extensions::filters::http::header_to_metadata::v3::Config::NUMBER: {
    double dval;
    if (absl::SimpleAtod(StringUtil::trim(value), &dval)) {
      val.set_number_value(dval);
    } else {
      ENVOY_LOG(debug, "value to number conversion failed");
      return false;
    }
    break;
  }
  case envoy::extensions::filters::http::header_to_metadata::v3::Config::PROTOBUF_VALUE: {
    if (!val.ParseFromString(value)) {
      ENVOY_LOG(debug, "parse from decoded string failed");
      return false;
    }
    break;
  }
  }

  auto& keyval = struct_map[meta_namespace];
  (*keyval.mutable_fields())[key] = std::move(val);

  return true;
}

const std::string& HeaderToMetadataFilter::decideNamespace(const std::string& nspace) const {
  return nspace.empty() ? HttpFilterNames::get().HeaderToMetadata : nspace;
}

// add metadata['key']= value depending on header present or missing case
void HeaderToMetadataFilter::applyKeyValue(std::string&& value, const Rule& rule,
                                           const KeyValuePair& keyval, StructMap& np) {
  if (!keyval.value().empty()) {
    value = keyval.value();
  } else {
    const auto& matcher = rule.regexRewrite();
    if (matcher != nullptr) {
      value = matcher->replaceAll(value, rule.regexSubstitution());
    }
  }
  if (!value.empty()) {
    const auto& nspace = decideNamespace(keyval.metadata_namespace());
    addMetadata(np, nspace, keyval.key(), value, keyval.type(), keyval.encode());
  } else {
    ENVOY_LOG(debug, "value is empty, not adding metadata");
  }
}

void HeaderToMetadataFilter::writeHeaderToMetadata(Http::HeaderMap& headers,
                                                   const HeaderToMetadataRules& rules,
                                                   Http::StreamFilterCallbacks& callbacks) {
  StructMap structs_by_namespace;

  for (const auto& rule : rules) {
    const auto& proto_rule = rule.rule();
    absl::optional<std::string> value = rule.selector_->extract(headers);

    if (value && proto_rule.has_on_header_present()) {
      applyKeyValue(std::move(value).value_or(""), rule, proto_rule.on_header_present(),
                    structs_by_namespace);
    } else if (!value && proto_rule.has_on_header_missing()) {
      applyKeyValue(std::move(value).value_or(""), rule, proto_rule.on_header_missing(),
                    structs_by_namespace);
    }
  }
  // Any matching rules?
  if (!structs_by_namespace.empty()) {
    for (auto const& entry : structs_by_namespace) {
      callbacks.streamInfo().setDynamicMetadata(entry.first, entry.second);
    }
  }
}

// TODO(rgs1): this belongs in one of the filter interfaces, see issue #10164.
const Config* HeaderToMetadataFilter::getConfig() const {
  // Cached config pointer.
  if (effective_config_) {
    return effective_config_;
  }

  effective_config_ = Http::Utility::resolveMostSpecificPerFilterConfig<Config>(decoder_callbacks_);
  if (effective_config_) {
    return effective_config_;
  }

  effective_config_ = config_.get();
  return effective_config_;
}

} // namespace HeaderToMetadataFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

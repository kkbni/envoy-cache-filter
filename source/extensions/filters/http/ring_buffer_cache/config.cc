#include "source/extensions/filters/http/ring_buffer_cache/config.h"
#include "source/extensions/filters/http/ring_buffer_cache/ring_buffer_cache.h"
#include "source/extensions/filters/http/ring_buffer_cache/config.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RingBufferCache {

absl::StatusOr<Http::FilterFactoryCb> RingBufferCacheConfigFactory::createFilterFactoryFromProto(
    const Protobuf::Message& proto_config,
    const std::string&,
    Server::Configuration::FactoryContext& /* context */) {

  // downcast the generic proto message to the specific configuration
  const auto& config_pb = dynamic_cast<const envoy::extensions::filters::http::ring_buffer_cache::RingBufferCacheConfig&>(proto_config);
  uint32_t size = config_pb.ring_buffer_size();

  if( size == 0 ) {
    return absl::InvalidArgumentError(
        "envoy.filters.http.ring_buffer_cache: ring_buffer_size must be >= 1 (got 0)");
  }

  auto config = std::make_shared<FilterConfig>(size);

  Http::FilterFactoryCb cb = [config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<RingBufferCacheFilter>(config));
  };
  return cb;
}

ProtobufTypes::MessagePtr RingBufferCacheConfigFactory::createEmptyConfigProto() {
  return std::make_unique<envoy::extensions::filters::http::ring_buffer_cache::RingBufferCacheConfig>();
}

std::string RingBufferCacheConfigFactory::name() const {
  return "envoy.filters.http.ring_buffer_cache";
}

REGISTER_FACTORY(RingBufferCacheConfigFactory, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace RingBufferCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
#pragma once

#include "envoy/server/filter_config.h"
#include "envoy/server/factory_context.h"
#include "envoy/http/filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RingBufferCache {

class RingBufferCacheConfigFactory : public Server::Configuration::NamedHttpFilterConfigFactory {
public:
    absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProto(
        const Protobuf::Message& proto_config,
        const std::string& stats_prefix,
        Server::Configuration::FactoryContext& context) override;

    ProtobufTypes::MessagePtr createEmptyConfigProto() override;
    std::string name() const override;
};

} // namespace RingBufferCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
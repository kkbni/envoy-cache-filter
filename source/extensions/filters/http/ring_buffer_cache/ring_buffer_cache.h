#pragma once

#include <string>
#include <vector>
#include <memory>

#include "envoy/http/filter.h"
#include "envoy/event/dispatcher.h"
#include "absl/synchronization/mutex.h"
#include "absl/container/flat_hash_map.h"
#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RingBufferCache {

struct CacheEntry
{
  enum class State { Fetching, Ready };
  State state { State::Fetching };
  absl::Mutex mutex;

  Http::ResponseHeaderMapPtr headers;
  Buffer::OwnedImpl body;

  // follower - request waiting for data
  struct Follower {
    Event::Dispatcher* dispatcher;
    std::shared_ptr<bool> is_active; // active connection to the downstream client
    Http::StreamDecoderFilterCallbacks* callbacks;
  };
  std::vector<Follower> followers;
};

// thread shared cache map
class SharedCacheManager {
public:
  std::shared_ptr<CacheEntry> getOrCreate(const std::string& key, bool& is_leader) {
    absl::MutexLock lock( &mutex_ );
    auto it = map_.find(key);
    if( it != map_.end() ) {
      is_leader = false;
      return it->second;
    }
    auto entry = std::make_shared<CacheEntry>();
    map_[key] = entry;
    is_leader = true;
    return entry;
  }

private:
  absl::Mutex mutex_;
  absl::flat_hash_map<std::string, std::shared_ptr<CacheEntry>> map_;
};

class FilterConfig {
public:
  explicit FilterConfig(uint32_t size) : max_size(size) {}

  SharedCacheManager cache_manager;
  uint32_t max_size;
};

class RingBufferCacheFilter : public Http::PassThroughFilter {
public:
  explicit RingBufferCacheFilter(std::shared_ptr<FilterConfig> config)
      : config_(std::move(config)), is_active_(std::make_shared<bool>(true)) {}

  ~RingBufferCacheFilter() override { *is_active_ = false; }

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_ = &callbacks;
    Http::PassThroughFilter::setDecoderFilterCallbacks(callbacks);
  }
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) override;
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;

private:
  std::shared_ptr<FilterConfig> config_;
  std::shared_ptr<bool> is_active_;
  bool is_leader_ = false;
  std::shared_ptr<CacheEntry> entry_;
  Http::StreamDecoderFilterCallbacks* decoder_callbacks_{};
};

} // namespace RingBufferCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
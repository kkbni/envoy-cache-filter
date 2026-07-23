#pragma once

#include <list>
#include <string>
#include <memory>

#include "envoy/http/filter.h"
#include "envoy/thread_local/thread_local.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/ring_buffer_cache/config.pb.h"
#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RingBufferCache {

struct CachedResponse {
  Http::ResponseHeaderMapPtr headers;
  std::string body;
};

// cache store for each worker thread
class ThreadLocalCache : public ThreadLocal::ThreadLocalObject {
public:
  ThreadLocalCache(uint32_t max_size) : max_size_(max_size) {}

  void insert(const std::string& key, Http::ResponseHeaderMapPtr&& headers, const std::string& body);
  const CachedResponse* get(const std::string& key) const;

private:
  const uint32_t max_size_;
  std::list<std::string> order_;
  absl::flat_hash_map<std::string, CachedResponse> cache_;
};

// wrapper to maintain the life cycle of thread-local storage slots
class FilterConfig {
public:
  FilterConfig(ThreadLocal::SlotPtr tls_slot) : tls_slot_(std::move(tls_slot)) {}
  ThreadLocal::Slot& tlsSlot() { return *tls_slot_; }
private:
  ThreadLocal::SlotPtr tls_slot_;
};

using FilterConfigSharedPtr = std::shared_ptr<FilterConfig>;

// main filter class
class RingBufferCacheFilter : public Http::PassThroughFilter, public Logger::Loggable<Logger::Id::filter> {
public:
  RingBufferCacheFilter(FilterConfigSharedPtr config) : config_(config) {}
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) override;
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;

private:
  FilterConfigSharedPtr config_;
  std::string cache_key_;
  bool is_cache_miss_{false};
  Http::ResponseHeaderMapPtr response_headers_;
  std::string response_body_;
};

} // namespace RingBufferCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
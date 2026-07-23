#include "source/extensions/filters/http/ring_buffer_cache/ring_buffer_cache.h"
#include "source/common/http/header_map_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RingBufferCache {

void ThreadLocalCache::insert(const std::string& key, Http::ResponseHeaderMapPtr&& headers, const std::string& body) {
  auto it = cache_.find(key);
  if( it != cache_.end() ) {
    cache_[key] = {std::move(headers), body};
    return;
  }

  // remove the oldest if buffer capacity is reached
  if( cache_.size() >= max_size_ && !order_.empty() ) {
    std::string oldest_key = order_.front();
    order_.pop_front();
    cache_.erase(oldest_key);
  }
  cache_[key] = {std::move(headers), body};
  order_.push_back(key);
}

const CachedResponse* ThreadLocalCache::get(const std::string& key) const {
  auto it = cache_.find(key);
  if( it != cache_.end() ) {
    return &it->second;
  }
  return nullptr;
}

Http::FilterHeadersStatus RingBufferCacheFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool) {
  const std::string host = std::string(headers.getHostValue());
  const std::string path = std::string(headers.getPathValue());
  cache_key_ = absl::StrCat(host, path);

  auto& cache = config_->tlsSlot().getTyped<ThreadLocalCache>();
  const CachedResponse* cached = cache.get(cache_key_);

  if( cached != nullptr ) {
    ENVOY_LOG(debug, "Ring Buffer Cache Hit for key: {}", cache_key_);

    // lambda to carry over cached response headers
    auto modify_headers = [cached](Http::ResponseHeaderMap& response_headers) {
      cached->headers->iterate(
        [&response_headers](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
          if( header.key().getStringView()[0] != ':' ) {
            response_headers.addCopy(Http::LowerCaseString(header.key().getStringView()), header.value().getStringView());
          }
          return Http::HeaderMap::Iterate::Continue;
        }
      );
    };

    decoder_callbacks_->sendLocalReply(Http::Code::OK, cached->body, modify_headers, absl::nullopt, "ring_buffer_cache_hit");
    return Http::FilterHeadersStatus::StopIteration;
  }

  ENVOY_LOG(debug, "Ring Buffer Cache Miss for key: {}", cache_key_);
  is_cache_miss_ = true;
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus RingBufferCacheFilter::encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) {
  if( is_cache_miss_ && headers.getStatusValue() == "200" ) {
    response_headers_ = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers);
    if( end_stream ) {
      auto& cache = config_->tlsSlot().getTyped<ThreadLocalCache>();
      cache.insert(cache_key_, std::move(response_headers_), "");
    }
  }
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus RingBufferCacheFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  if( is_cache_miss_ && response_headers_ != nullptr ) {
    response_body_.append(data.toString());
    if( end_stream ) {
      auto& cache = config_->tlsSlot().getTyped<ThreadLocalCache>();
      cache.insert(cache_key_, std::move(response_headers_), response_body_);
    }
  }
  return Http::FilterDataStatus::Continue;
}

} // namespace RingBufferCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
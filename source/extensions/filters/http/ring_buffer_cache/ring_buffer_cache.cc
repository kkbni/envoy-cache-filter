#include "source/extensions/filters/http/ring_buffer_cache/ring_buffer_cache.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RingBufferCache {

Http::FilterHeadersStatus RingBufferCacheFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool)
{
  std::string key = std::string(headers.getPathValue());
  bool is_leader;
  entry_ = config_->cache_manager.getOrCreate(key, is_leader);

  if( is_leader ) {
    is_leader_ = true;
    return Http::FilterHeadersStatus::Continue;
  }
  // is follower
  absl::MutexLock lock( &entry_->mutex );
  if( entry_->state == CacheEntry::State::Ready )
  { // downloaded - serve
    decoder_callbacks_->encodeHeaders(Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*entry_->headers), false, "ring_buffer_cache_hit");

    Buffer::OwnedImpl body_copy;
    for( const Buffer::RawSlice& slice : entry_->body.getRawSlices() ) {
      body_copy.add(slice.mem_, slice.len_);
    }
    decoder_callbacks_->encodeData(body_copy, true);

    return Http::FilterHeadersStatus::StopIteration;
  }
  // mid stream
  // if the leader got the headers - send to followers
  if( entry_->headers != nullptr ) {
    decoder_callbacks_->encodeHeaders(Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*entry_->headers), false, "ring_buffer_cache_hit");
  }
  // if the leader got data - send to followers
  if( entry_->body.length() > 0 ) {
    Buffer::OwnedImpl body_copy;
    for( const Buffer::RawSlice& slice : entry_->body.getRawSlices() ) {
      body_copy.add(slice.mem_, slice.len_);
    }
    decoder_callbacks_->encodeData(body_copy, false /* leader isn't done */);
  }

  entry_->followers.push_back({
    &decoder_callbacks_->dispatcher(),
    is_active_,
    decoder_callbacks_
  });
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterHeadersStatus RingBufferCacheFilter::encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) 
{
  if( !is_leader_ ) {
    return Http::FilterHeadersStatus::Continue;
  }
  // is leader
  absl::MutexLock lock( &entry_->mutex );
  entry_->headers = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers);

  // broadcast headers to all followers
  for( auto& follower : entry_->followers ) {
    auto headers_copy = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers);
    std::shared_ptr<bool> active = follower.is_active;
    Http::StreamDecoderFilterCallbacks* cb = follower.callbacks;

    follower.dispatcher->post( [active, cb, h = std::move(headers_copy), end_stream]() mutable {
      if( *active ) { cb->encodeHeaders(std::move(h), end_stream, "ring_buffer_cache_hit"); }
    });
  }
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus RingBufferCacheFilter::encodeData(Buffer::Instance& data, bool end_stream) 
{
  if( !is_leader_ ) {
    return Http::FilterDataStatus::Continue;
  }

  // is leader
  absl::MutexLock lock( &entry_->mutex );
  for( const Buffer::RawSlice& slice : data.getRawSlices() ) {
    entry_->body.add(slice.mem_, slice.len_);
  }

  if( end_stream ) {
    entry_->state = CacheEntry::State::Ready;
  }

  // stream chunk of data to all followers
  for( auto& follower : entry_->followers ) {
    auto data_copy = std::make_shared<Buffer::OwnedImpl>();
    for( const Buffer::RawSlice& slice : data.getRawSlices() ) {
      data_copy->add(slice.mem_, slice.len_);
    }

    std::shared_ptr<bool> active = follower.is_active;
    Http::StreamDecoderFilterCallbacks* cb = follower.callbacks;

    follower.dispatcher->post([active, cb, data_copy, end_stream]() {
      if( *active ) {
        cb->encodeData(*data_copy, end_stream);
      }
    });
  }
  return Http::FilterDataStatus::Continue;
}

} // namespace RingBufferCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
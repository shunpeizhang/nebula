/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#ifndef COMMON_BASE_CONCURRENTLRUCACHE_H_
#define COMMON_BASE_CONCURRENTLRUCACHE_H_

#include "base/Base.h"
#include "base/StatusOr.h"
#include <list>
#include <utility>
#include <boost/optional.hpp>
#include <gtest/gtest_prod.h>

namespace nebula {

template<class Key, class Value>
class LRU;

template<typename K, typename V>
class ConcurrentLRUCache final {
    FRIEND_TEST(ConcurrentLRUCacheTest, SimpleTest);

public:
    explicit ConcurrentLRUCache(size_t capacity, uint32_t bucketsExp = 4)
        : bucketsNum_(1 << bucketsExp)
        , bucketsExp_(bucketsExp) {
        CHECK(capacity > bucketsNum_ && bucketsNum_ > 0);
        auto capPerBucket = capacity >> bucketsExp;
        auto left = capacity;
        for (uint32_t i = 0; i < bucketsNum_ - 1; i++) {
            buckets_.emplace_back(capPerBucket);
            left -= capPerBucket;
        }
        CHECK_GT(left, 0);
        buckets_.emplace_back(left);
    }

    bool contains(const K& key, int32_t hint = -1) {
        return buckets_[bucketIndex(key, hint)].contains(key);
    }

    void insert(const K& key, const V& val, int32_t hint = -1) {
        buckets_[bucketIndex(key, hint)].insert(key, val);
    }

    StatusOr<V> get(const K& key, int32_t hint = -1) {
        return buckets_[bucketIndex(key, hint)].get(key);
    }

    /**
     * Insert the {key, val} if key not existed, and return Status::Inserted.
     * Otherwise, just return the value for the existed key.
     * */
    StatusOr<V> putIfAbsent(const K& key, const V& val, int32_t hint = -1) {
        return buckets_[bucketIndex(key, hint)].putIfAbsent(key, val);
    }

    void evict(const K& key, int32_t hint = -1) {
        buckets_[bucketIndex(key, hint)].evict(key);
    }

    void clear() {
        for (auto i = 0; i < bucketsNum_; i++) {
            buckets_[i].clear();
        }
    }

private:
    class Bucket {
    public:
        explicit Bucket(size_t capacity)
            : lru_(std::make_unique<LRU<K, V>>(capacity)) {}

        Bucket(Bucket&& b)
            : lru_(std::move(b.lru_)) {}

        bool contains(const K& key) {
            std::lock_guard<std::mutex> guard(lock_);
            return lru_->contains(key);
        }

        void insert(const K& key, const V& val) {
            std::lock_guard<std::mutex> guard(lock_);
            lru_->insert(key, val);
        }

        StatusOr<V> get(const K& key) {
            std::lock_guard<std::mutex> guard(lock_);
            auto v = lru_->get(key);
            if (v == boost::none) {
                return Status::Error();
            }
            return std::move(v).value();
        }

        StatusOr<V> putIfAbsent(const K& key, const V& val) {
            std::lock_guard<std::mutex> guard(lock_);
            auto v = lru_->get(key);
            if (v == boost::none) {
                lru_->insert(key, val);
                return Status::Inserted();
            }
            return std::move(v).value();
        }

        void evict(const K& key) {
            std::lock_guard<std::mutex> guard(lock_);
            lru_->evict(key);
        }

        void clear() {
            std::lock_guard<std::mutex> guard(lock_);
            lru_->clear();
        }

    private:
        std::mutex lock_;
        std::unique_ptr<LRU<K, V>> lru_;
    };


private:
    /**
     * If hint is specified, we could use it to cal the bucket index directly without hash key.
     * */
    uint32_t bucketIndex(const K& key, int32_t hint = -1) {
        return hint >= 0 ? (hint & ((1 << bucketsExp_) - 1))
                         : (std::hash<K>()(key) & ((1 << bucketsExp_) - 1));
    }


private:
    std::vector<Bucket> buckets_;
    uint32_t bucketsNum_ = 1;
    uint32_t bucketsExp_ = 0;
};


/**
    It is copied from boost::compute::detail::LRU.
    The differences:
    1. Add methed evict(const K& key);
    2. Instead std::map with std::unordered_map
    3. Update the code style.
*/
template<class Key, class Value>
class LRU {
public:
    typedef Key key_type;
    typedef Value value_type;
    typedef std::list<key_type> list_type;
    typedef std::unordered_map<
                key_type,
                std::tuple<value_type, typename list_type::iterator>
            > map_type;

    explicit LRU(size_t capacity)
        : capacity_(capacity) {
    }

    ~LRU() = default;

    size_t size() const {
        return map_.size();
    }

    size_t capacity() const {
        return capacity_;
    }

    bool empty() const {
        return map_.empty();
    }

    bool contains(const key_type& key) {
        return map_.find(key) != map_.end();
    }

    void insert(const key_type& key, const value_type& value) {
        typename map_type::iterator i = map_.find(key);
        if (i == map_.end()) {
            // insert item into the cache, but first check if it is full
            if (size() >= capacity_) {
                VLOG(3) << "Size:" << size() << ", capacity " << capacity_;
                // cache is full, evict the least recently used item
                evict();
            }
            // insert the new item
            list_.push_front(key);
            map_[key] = std::make_tuple(value, list_.begin());
            VLOG(3) << "Insert key " << key << ", val " << value;
        }
    }

    boost::optional<value_type> get(const key_type& key) {
        // lookup value in the cache
        typename map_type::iterator i = map_.find(key);
        if (i == map_.end()) {
            // value not in cache
            VLOG(3) << key  << " not found!";
            return boost::none;
        }

        // return the value, but first update its place in the most
        // recently used list
        typename list_type::iterator j = std::get<1>(i->second);
        const value_type& value = std::get<0>(i->second);
        if (j != list_.begin()) {
            // move item to the front of the most recently used list
            list_.erase(j);
            list_.push_front(key);
            // update iterator in map
            j = list_.begin();
            i->second = std::make_tuple(value, j);
        }
        VLOG(3) << "Get key : " << key << ", val: " << value;
        return value;
    }

    /**
     * evict the key if exist.
     * */
    void evict(const key_type& key) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.erase(std::get<1>(it->second));
            map_.erase(it);
        }
    }

    void clear() {
        map_.clear();
        list_.clear();
    }

private:
    void evict() {
        // evict item from the end of most recently used list
        typename list_type::iterator i = --list_.end();
        VLOG(3) << "Evict the oldest key " << *i;
        map_.erase(*i);
        list_.erase(i);
    }

private:
    map_type map_;
    list_type list_;
    size_t capacity_;
};

}  // namespace nebula

#endif  // COMMON_BASE_CONCURRENTLRUCACHE_H_


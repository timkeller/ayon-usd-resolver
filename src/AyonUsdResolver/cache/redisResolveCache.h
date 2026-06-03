#ifndef AR_AYONUSDRESOLVER_REDIS_RESOLVE_CACHE_H
#define AR_AYONUSDRESOLVER_REDIS_RESOLVE_CACHE_H

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct redisContext;

/**
 * @class RedisResolveCache
 * @brief Region-local L2 cache for AYON URI -> resolved path, backed by Redis (hiredis).
 *
 * Sits between the in-process L1 cache (ResolverContextCache) and the central AYON server.
 * The first node in a region to resolve a URI pays the WAN call to AYON and writes the result
 * here; every other process/host in the same region then reads it over the LAN.
 *
 * Thread-safety: the resolver composes on many threads, and a redisContext is NOT thread-safe,
 * so this keeps a small pool of contexts guarded by a mutex (acquire one per operation, return
 * it on success, discard on error). Every operation fails OPEN: any connection/protocol error
 * returns a miss (or silently drops a write) so resolution always falls back to the server and
 * a dead regional cache never stalls or breaks a resolve.
 *
 * Disabled (every op is a no-op/miss) when constructed from an empty URL.
 */
class RedisResolveCache {
    public:
        /**
         * @param url       AYON_RESOLVER_CACHE_URL, e.g. "redis://host:6379" or "host:6379".
         *                  Empty -> cache disabled.
         * @param keyPrefix Namespacing prefix already containing project/platform/site, e.g.
         *                  "ayon:resolve:ASJ:darwin:work". Final key = keyPrefix + ":" + uri.
         */
        RedisResolveCache(const std::string &url, const std::string &keyPrefix);
        ~RedisResolveCache();

        RedisResolveCache(const RedisResolveCache &) = delete;
        RedisResolveCache &operator=(const RedisResolveCache &) = delete;

        bool enabled() const {
            return m_enabled;
        }

        /** @return the resolved path, or nullopt on miss/error. */
        std::optional<std::string> get(const std::string &uri);

        /** @return map of uri -> resolved path for the URIs that were cache hits (misses absent). */
        std::unordered_map<std::string, std::string> mget(const std::vector<std::string> &uris);

        /** Store one mapping with no expiry. Only immutable resolutions are ever stored (see
         *  the resolver's write path), so entries never need a TTL. Failures are ignored. */
        void set(const std::string &uri, const std::string &resolvedPath);

        /** Store many mappings in one round-trip (pipelined), no expiry. Failures are ignored. */
        void mset(const std::vector<std::pair<std::string, std::string>> &entries);

    private:
        redisContext* acquire();
        void release(redisContext* ctx, bool healthy);
        std::string makeKey(const std::string &uri) const;

        std::string m_host;
        int m_port = 6379;
        std::string m_keyPrefix;
        bool m_enabled = false;

        int m_connectTimeoutMs = 200;
        int m_opTimeoutMs = 200;
        size_t m_maxPoolSize = 16;

        // Circuit breaker: after a failed connect, stop attempting (return misses immediately)
        // for a cooldown so a downed regional cache doesn't add a connect-timeout to every
        // resolve across the whole region.
        int m_breakerCooldownMs = 5000;

        std::mutex m_poolMutex;
        std::vector<redisContext*> m_pool;
        std::chrono::steady_clock::time_point m_breakerUntil{};   // guarded by m_poolMutex
};

#endif   // AR_AYONUSDRESOLVER_REDIS_RESOLVE_CACHE_H

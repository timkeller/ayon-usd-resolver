#include "redisResolveCache.h"

#include <hiredis/hiredis.h>

#include <string>
#include <utility>

namespace {

timeval
makeTimeval(int ms) {
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return tv;
}

}   // namespace

RedisResolveCache::RedisResolveCache(const std::string &url, const std::string &keyPrefix): m_keyPrefix(keyPrefix) {
    if (url.empty()) {
        m_enabled = false;
        return;
    }

    std::string s = url;
    const std::string scheme = "redis://";
    if (s.rfind(scheme, 0) == 0) {
        s = s.substr(scheme.size());
    }
    // Drop any trailing /db or path component.
    const auto slash = s.find('/');
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }
    const auto colon = s.find(':');
    if (colon != std::string::npos) {
        m_host = s.substr(0, colon);
        try {
            m_port = std::stoi(s.substr(colon + 1));
        }
        catch (...) {
            m_port = 6379;
        }
    }
    else {
        m_host = s;
        m_port = 6379;
    }

    m_enabled = !m_host.empty();
}

RedisResolveCache::~RedisResolveCache() {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    for (redisContext* ctx: m_pool) {
        if (ctx != nullptr) {
            redisFree(ctx);
        }
    }
    m_pool.clear();
}

redisContext*
RedisResolveCache::acquire() {
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (!m_pool.empty()) {
            redisContext* ctx = m_pool.back();
            m_pool.pop_back();
            return ctx;
        }
        // Circuit open: skip the connect attempt (and its timeout) during the cooldown.
        if (std::chrono::steady_clock::now() < m_breakerUntil) {
            return nullptr;
        }
    }

    const timeval connectTv = makeTimeval(m_connectTimeoutMs);
    redisContext* ctx = redisConnectWithTimeout(m_host.c_str(), m_port, connectTv);
    if (ctx == nullptr || ctx->err) {
        if (ctx != nullptr) {
            redisFree(ctx);
        }
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_breakerUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_breakerCooldownMs);
        return nullptr;
    }
    const timeval opTv = makeTimeval(m_opTimeoutMs);
    redisSetTimeout(ctx, opTv);
    return ctx;
}

void
RedisResolveCache::release(redisContext* ctx, bool healthy) {
    if (ctx == nullptr) {
        return;
    }
    if (!healthy || ctx->err) {
        redisFree(ctx);
        return;
    }
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (m_pool.size() < m_maxPoolSize) {
        m_pool.push_back(ctx);
    }
    else {
        redisFree(ctx);
    }
}

std::string
RedisResolveCache::makeKey(const std::string &uri) const {
    return m_keyPrefix + ":" + uri;
}

std::optional<std::string>
RedisResolveCache::get(const std::string &uri) {
    if (!m_enabled || uri.empty()) {
        return std::nullopt;
    }
    redisContext* ctx = acquire();
    if (ctx == nullptr) {
        return std::nullopt;   // fail open
    }

    const std::string key = makeKey(uri);
    const char* argv[2] = {"GET", key.c_str()};
    const size_t argvlen[2] = {3, key.size()};
    redisReply* reply = static_cast<redisReply*>(redisCommandArgv(ctx, 2, argv, argvlen));

    std::optional<std::string> out;
    const bool healthy = (reply != nullptr);
    if (reply != nullptr) {
        if (reply->type == REDIS_REPLY_STRING) {
            out = std::string(reply->str, reply->len);
        }
        freeReplyObject(reply);
    }
    release(ctx, healthy);
    return out;
}

std::unordered_map<std::string, std::string>
RedisResolveCache::mget(const std::vector<std::string> &uris) {
    std::unordered_map<std::string, std::string> out;
    if (!m_enabled || uris.empty()) {
        return out;
    }

    std::vector<std::string> keys;
    keys.reserve(uris.size());
    for (const auto &uri: uris) {
        keys.push_back(makeKey(uri));
    }

    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.reserve(keys.size() + 1);
    argvlen.reserve(keys.size() + 1);
    argv.push_back("MGET");
    argvlen.push_back(4);
    for (const auto &key: keys) {
        argv.push_back(key.c_str());
        argvlen.push_back(key.size());
    }

    redisContext* ctx = acquire();
    if (ctx == nullptr) {
        return out;
    }
    redisReply* reply
        = static_cast<redisReply*>(redisCommandArgv(ctx, static_cast<int>(argv.size()), argv.data(), argvlen.data()));

    const bool healthy = (reply != nullptr);
    if (reply != nullptr) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == uris.size()) {
            for (size_t i = 0; i < reply->elements; ++i) {
                redisReply* element = reply->element[i];
                if (element != nullptr && element->type == REDIS_REPLY_STRING) {
                    out.emplace(uris[i], std::string(element->str, element->len));
                }
            }
        }
        freeReplyObject(reply);
    }
    release(ctx, healthy);
    return out;
}

void
RedisResolveCache::set(const std::string &uri, const std::string &resolvedPath) {
    if (!m_enabled || uri.empty() || resolvedPath.empty()) {
        return;
    }
    redisContext* ctx = acquire();
    if (ctx == nullptr) {
        return;
    }

    const std::string key = makeKey(uri);
    const char* argv[3] = {"SET", key.c_str(), resolvedPath.c_str()};
    const size_t argvlen[3] = {3, key.size(), resolvedPath.size()};
    redisReply* reply = static_cast<redisReply*>(redisCommandArgv(ctx, 3, argv, argvlen));

    const bool healthy = (reply != nullptr);
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    release(ctx, healthy);
}

void
RedisResolveCache::mset(const std::vector<std::pair<std::string, std::string>> &entries) {
    if (!m_enabled || entries.empty()) {
        return;
    }
    redisContext* ctx = acquire();
    if (ctx == nullptr) {
        return;
    }

    // Pipeline every SET into the output buffer (one round-trip), then drain the replies.
    // redisAppendCommandArgv copies the arg bytes immediately, so the per-iteration locals
    // do not need to outlive the loop.
    size_t queued = 0;
    for (const auto &entry: entries) {
        const std::string &uri = entry.first;
        const std::string &resolvedPath = entry.second;
        if (uri.empty() || resolvedPath.empty()) {
            continue;
        }
        const std::string key = makeKey(uri);
        const char* argv[3] = {"SET", key.c_str(), resolvedPath.c_str()};
        const size_t argvlen[3] = {3, key.size(), resolvedPath.size()};
        redisAppendCommandArgv(ctx, 3, argv, argvlen);
        ++queued;
    }

    bool healthy = true;
    for (size_t i = 0; i < queued; ++i) {
        void* rawReply = nullptr;
        if (redisGetReply(ctx, &rawReply) != REDIS_OK) {
            healthy = false;
            break;
        }
        if (rawReply != nullptr) {
            freeReplyObject(static_cast<redisReply*>(rawReply));
        }
    }
    release(ctx, healthy);
}

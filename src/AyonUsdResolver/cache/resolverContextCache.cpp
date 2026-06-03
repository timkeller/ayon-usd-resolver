#include "resolverContextCache.h"

#include "../codes/debugCodes.h"
#include "../config.h"
#include "../helpers/ayonApiGet.h"
#include "../helpers/resolutionFunctions.h"
#include <ynput/core/iostd/envVarHelpers.hpp>
#include <ynput/tool/ayon/rootHelpers.hpp>

#include "nlohmann/json_fwd.hpp"
#include "pxr/base/arch/systemInfo.h"
#include "pxr/base/tf/pathUtils.h"
#include "pxr/usd/ar/resolvedPath.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Build the region-local cache key namespace from the AYON env. The cache instance is already
// per-region, so the prefix only needs project/platform/site to keep distinct contexts apart
// (the resolved path is platform/site-specific because of rootReplace).
std::string
buildCacheKeyPrefix() {
    const char* project = std::getenv("AYON_PROJECT_NAME");
    const char* site = std::getenv("AYON_SITE_ID");
#if defined(_WIN32)
    const std::string platform = "windows";
#elif defined(__APPLE__)
    const std::string platform = "darwin";
#else
    const std::string platform = "linux";
#endif
    return std::string("ayon:resolve:") + (project ? project : "") + ":" + platform + ":" + (site ? site : "");
}

// Whether a resolved URI is safe to persist in the shared L2 cache. Only immutable
// resolutions go in: an explicit version (a frozen published artifact) or `hero` (a stable,
// in-place hardlinked path that the server resolves to a fixed `.../hero/...` location). A
// `latest`/bare ref moves on every publish, so it is resolved live and never stored. This is
// what lets the cache run with no per-key invalidation: the only thing that can move an
// immutable resolution is a global change (storage-root remap or a resolve-template change),
// handled out of band by flushing the regional Redis.
bool
shouldCacheUri(const std::string &uri) {
    const std::string marker = "version=";
    const auto pos = uri.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    std::string version = uri.substr(pos + marker.size());
    const auto amp = version.find('&');
    if (amp != std::string::npos) {
        version = version.substr(0, amp);
    }
    return !version.empty() && version != "latest";
}

}   // namespace

// TODO pinning file hanlder should construct its cache directly at construction getAssetData should not call
// rootReplace
PinningFileHandler::PinningFileHandler(const std::string &pinningFilePath,
                                       const std::unordered_map<std::string, std::string> &rootReplaceData):
    m_pinningFilePath(pinningFilePath),
    m_rootReplaceData(rootReplaceData) {
    std::ifstream pinningFile(m_pinningFilePath);

    if (!pinningFile.is_open()) {
        throw std::runtime_error("PinningFileHandler was not able to open PinningFile: "
                                 + m_pinningFilePath.string());
    }

    nlohmann::json raw_pinning_file;
    try {
        raw_pinning_file = nlohmann::json::parse(pinningFile);
    }
    catch (const nlohmann::json::parse_error &e) {
        throw std::runtime_error("The pining File is not in the Correct Format: ");
    }

    nlohmann::json pinningData = raw_pinning_file.at("ayon_resolver_pinning_data");
    pinningData.erase("ayon_pinning_data_entry_scene");

    for (auto &entry: pinningData.items()) {
        std::string pathed_key = ynput::tool::ayon::rootReplace(entry.key(), m_rootReplaceData);
        std::string pathed_val = ynput::tool::ayon::rootReplace(entry.value(), m_rootReplaceData);
        m_pinningFileData[pathed_key] = pathed_val;
    }
};

/**
 * @brief return AssetIdentifier populated with root rootReplaceData from the pinning file using the pinning file data loaded
 * at construction and the PROJECT_ROOTS env variable.
 * this is not a cached function it will reconstruct the AssetIdentifier. it will not reload the file or the env var however.
 *
 * @param resolveKey UsdAssetIdent
 * @return populated AssetIdentifier if key was found in pinning file. Empty AssetIdentifier if key was not found
 */
AssetIdentifier
PinningFileHandler::getAssetData(const std::string &resolveKey) {
    AssetIdentifier assetEntry;

    std::string pinnedAssetPath;
    try {
        pinnedAssetPath = m_pinningFileData.at(resolveKey);
    }
    catch (const nlohmann::json::out_of_range &e) {
        return assetEntry;
    }

    if (!pinnedAssetPath.empty()) {
        assetEntry.setAssetIdentifier(resolveKey);
        assetEntry.setResolvedAssetPath(pinnedAssetPath);
    }

    return assetEntry;
};

ResolverContextCache::ResolverContextCache(): m_AyonCache(), m_CommonCache(), m_PreCache(), m_staticCache(true) {
    m_PreCache.reserve(PRECACHE_SIZE);
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::ResolverContextCache() \n");

    const char* enable_static_env_var = std::getenv(ENABLE_STATIC_GLOBAL_CACHE_ENV_KEY);
    if (enable_static_env_var == nullptr || std::strcmp(enable_static_env_var, "false") == 0) {
        std::unique_ptr<AyonApi> api = getAyonApiFromEnv();
        m_ayon.emplace(std::move(api));

        m_staticCache = false;

        // Optional region-local L2 cache. Disabled (no-op) when AYON_RESOLVER_CACHE_URL is unset.
        const char* cacheUrl = std::getenv("AYON_RESOLVER_CACHE_URL");
        m_redisCache = std::make_unique<RedisResolveCache>(cacheUrl ? cacheUrl : "", buildCacheKeyPrefix());
        if (m_redisCache->enabled()) {
            TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
                .Msg("ResolverContextCache: region-local Redis cache enabled (%s)\n", cacheUrl);
        }
    }
    else {
        std::map<std::string, std::string> projectRootsEnvMap = ynput::core::iostd::getEnvMap(PROJECT_ROOTS_ENV_KEY);
        std::unordered_map<std::string, std::string> projectRootsEnvUMap(
            std::make_move_iterator(projectRootsEnvMap.begin()), std::make_move_iterator(projectRootsEnvMap.end()));
        m_pinningFileHandler.emplace(ynput::core::iostd::getEnvKey(PINNING_FILE_PATH_ENV_KEY),
                                           projectRootsEnvUMap);
    }
};

ResolverContextCache::~ResolverContextCache() {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::~ResolverContextCache() \n");
};

// TODO when ayonLogger.h has the header guards then we can import it and use logging from there
void
ResolverContextCache::printCache() const {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::printCache \n");

    std::shared_lock<std::shared_mutex> PreCacheReadLock(m_PreCacheSharedMutex);
    std::shared_lock<std::shared_mutex> AyonCacheReadLock(m_AyonCacheSharedMutex);
    std::shared_lock<std::shared_mutex> CommonCacheReadLock(m_CommonCacheSharedMutex);
    std::cout << "Printing out the Cache Entries \n";

    std::cout << "PreCache size: " << m_PreCache.size() << "\n";
    for (const auto &assetIdentifierInstance: m_PreCache) {
        assetIdentifierInstance.printInfo();
    }
    std::cout << "AyonCache size: " << m_AyonCache.size() << "\n";
    for (const auto &assetIdentifierInstance: m_AyonCache) {
        assetIdentifierInstance.printInfo();
    }
    std::cout << "CommonCache size: " << m_CommonCache.size() << "\n";
    for (const auto &assetIdentifierInstance: m_CommonCache) {
        assetIdentifierInstance.printInfo();
    }
    std::ostringstream oss;
    oss << static_cast<const void*>(this);
    std::cout << "ResolverContextCache infos;" << " Instance_M_Pose; " << oss.str().c_str() << " Instance_m_Size; "
              << std::to_string(sizeof(*this)).c_str() << "\n";

    std::cout << "-----------------------------------------------------\n" << std::endl;
};

void
ResolverContextCache::insert(AssetIdentifier &sourceAssetIdent) {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
        .Msg("ResolverContextCache::insert(%s) \n", sourceAssetIdent.getAssetIdentifier().c_str());
    if (m_PreCache.size() == PRECACHE_SIZE) {
        migratePreCacheIntoAyonCache();
    }

    std::unique_lock<std::shared_mutex> PreCacheWriteLock(m_PreCacheSharedMutex);
    std::unique_lock<std::shared_mutex> AyonCacheWriteLock(m_AyonCacheSharedMutex);

    m_PreCache.insert(std::move(sourceAssetIdent));
};

void
ResolverContextCache::migratePreCacheIntoAyonCache() {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::migratePreCacheIntoAyonCache \n");
    std::unique_lock<std::shared_mutex> PreCacheWriteLock(m_PreCacheSharedMutex);
    std::unique_lock<std::shared_mutex> AyonCacheWriteLock(m_AyonCacheSharedMutex);

    m_AyonCache.reserve(m_AyonCache.size() + m_PreCache.size());
    m_AyonCache.insert(std::make_move_iterator(m_PreCache.begin()), std::make_move_iterator(m_PreCache.end()));
    m_PreCache.clear();
};

std::unordered_map<std::string, std::string>
ResolverContextCache::batchWarm(std::vector<std::string> &uriPaths) {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
        .Msg("ResolverContextCache::batchWarm: %zu uris \n", uriPaths.size());

    std::unordered_map<std::string, std::string> resolved;
    if (m_staticCache || uriPaths.empty()) {
        return resolved;
    }

    // L2 first: one MGET against the region-local cache, then only WAN-resolve the misses.
    std::vector<std::string> serverMisses;
    if (m_redisCache && m_redisCache->enabled()) {
        resolved = m_redisCache->mget(uriPaths);
        for (const auto &uri: uriPaths) {
            if (resolved.find(uri) == resolved.end()) {
                serverMisses.push_back(uri);
            }
        }
    }
    else {
        serverMisses = uriPaths;
    }

    if (!serverMisses.empty()) {
        // One batched request over the persistent keep-alive client: few round-trips,
        // deterministic, connection reused.
        std::unordered_map<std::string, std::string> serverResolved
            = m_ayon->get()->batchResolvePathSerial(serverMisses);

        std::vector<std::pair<std::string, std::string>> writeBack;
        for (const auto &entry: serverResolved) {
            resolved.emplace(entry.first, entry.second);
            if (m_redisCache && m_redisCache->enabled() && !entry.second.empty()
                && shouldCacheUri(entry.first)) {
                writeBack.emplace_back(entry.first, entry.second);
            }
        }
        if (!writeBack.empty()) {
            m_redisCache->mset(writeBack);
        }
    }

    for (const auto &entry: resolved) {
        if (entry.first.empty() || entry.second.empty()) {
            continue;
        }
        AssetIdentifier asset;
        asset.setAssetIdentifier(entry.first);
        asset.setResolvedAssetPath(entry.second);
        this->insert(asset);
    }
    return resolved;
};

AssetIdentifier
ResolverContextCache::getAsset(const std::string &assetIdentifier,
                               const CacheName selectedCache,
                               const bool isAyonPath) {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::getAsset: (%s) \n", assetIdentifier.c_str());

    AssetIdentifier asset;

    if (assetIdentifier.empty()) {
        return asset;
    }
    if (m_staticCache) {
        return m_pinningFileHandler->getAssetData(assetIdentifier);
    }

    std::unordered_set<AssetIdentifier, AssetIdentifierHash>::iterator hit;

    std::shared_lock<std::shared_mutex> preCacheSharedLock(m_PreCacheSharedMutex);
    hit = m_PreCache.find(assetIdentifier);
    if (hit != m_PreCache.end()) {
        asset = *hit;
        preCacheSharedLock.unlock();

        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
            .Msg("ResolverContextCache::getAsset: PreCache Hit on (%s) with (%s) \n",
                 asset.getAssetIdentifier().c_str(), asset.getResolvedAssetPath().GetPathString().c_str());
        return asset;
    }
    preCacheSharedLock.unlock();

    switch (selectedCache) {
        case CacheName::AYONCACHE:
            {
                std::shared_lock<std::shared_mutex> ayonCacheSharedLock(m_AyonCacheSharedMutex);
                hit = m_AyonCache.find(assetIdentifier);
                if (hit != m_AyonCache.end()) {
                    asset = *hit;
                    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::getAsset: AyonCache Hit \n");
                }

                ayonCacheSharedLock.unlock();
                break;
            }

        case CacheName::COMMONCACHE:
            {
                std::shared_lock<std::shared_mutex> CommonCacheSharedLock(m_CommonCacheSharedMutex);
                hit = m_CommonCache.find(assetIdentifier);
                if (hit != m_CommonCache.end()) {
                    asset = *hit;
                    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
                        .Msg("ResolverContextCache::getAsset: CommonCache Hit \n");
                }

                CommonCacheSharedLock.unlock();
                break;
            }
    }
    if (!asset.isEmpty()) {
        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
            .Msg("ResolverContextCache::getAsset: Cache Hit with (%s) with (%s) \n", asset.getAssetIdentifier().c_str(),
                 asset.getResolvedAssetPath().GetPathString().c_str());
        return asset;
    }

    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::getAsset: No Cache Hit \n");
    if (isAyonPath) {
        // L2: region-local Redis before the WAN call to the AYON server.
        if (m_redisCache && m_redisCache->enabled()) {
            std::optional<std::string> l2Hit = m_redisCache->get(assetIdentifier);
            if (l2Hit && !l2Hit->empty()) {
                asset.setAssetIdentifier(assetIdentifier);
                asset.setResolvedAssetPath(*l2Hit);
                TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::getAsset: Redis L2 hit \n");
                this->insert(asset);
                return asset;
            }
        }

        std::pair<std::string, std::string> resolvedAsset = m_ayon->get()->resolvePath(assetIdentifier);

        asset.setAssetIdentifier(resolvedAsset.first);
        asset.setResolvedAssetPath(resolvedAsset.second);

        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::getAsset: called ayon.resolvePath() \n");

        // Write-through to L2, but only for immutable resolutions (explicit version or hero),
        // so the shared cache never holds a ref that can move on publish.
        if (m_redisCache && m_redisCache->enabled() && !resolvedAsset.first.empty()
            && !resolvedAsset.second.empty() && shouldCacheUri(resolvedAsset.first)) {
            m_redisCache->set(resolvedAsset.first, resolvedAsset.second);
        }

        this->insert(asset);
    }
    else {
        if (_IsRelativePath(assetIdentifier)) {
            asset.setResolvedAssetPath(_ResolveAnchored(ArchGetCwd(), assetIdentifier));
        }
        else {
            asset.setResolvedAssetPath(ArResolvedPath(TfNormPath(TfAbsPath(assetIdentifier))));
        }
        if (!asset.getResolvedAssetPath().empty()) {
            asset.setAssetIdentifier(assetIdentifier);

            std::shared_lock<std::shared_mutex> CommonCacheSharedLock(m_CommonCacheSharedMutex);

            TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
                .Msg("ResolverContextCache::getAsset: insert into CommonCache \n");
            m_CommonCache.insert(asset);
        }
    }

    return asset;
};

void
ResolverContextCache::removeCachedObject(const std::string &key) {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::removeCachedObject (%s) \n", key.c_str());

    std::unordered_set<AssetIdentifier>::iterator hit;

    std::unique_lock<std::shared_mutex> preCacheSharedWriteLock(m_PreCacheSharedMutex);

    hit = m_PreCache.find(key);
    if (hit != m_PreCache.end()) {
        m_PreCache.erase(hit);
        preCacheSharedWriteLock.unlock();
        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
            .Msg("ResolverContextCache::removeCachedObject removed object from PreCache");
        return;
    }

    std::unique_lock<std::shared_mutex> AyonCachesharedWriteLock(m_AyonCacheSharedMutex);

    hit = m_AyonCache.find(key);
    if (hit != m_AyonCache.end()) {
        m_AyonCache.erase(hit);
        AyonCachesharedWriteLock.unlock();
        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
            .Msg("ResolverContextCache::removeCachedObject removed object from AyonCache");
        return;
    }

    std::unique_lock<std::shared_mutex> CommonCachesharedWriteLock(m_CommonCacheSharedMutex);
    hit = m_CommonCache.find(key);
    if (hit != m_CommonCache.end()) {
        m_CommonCache.erase(hit);
        CommonCachesharedWriteLock.unlock();
        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
            .Msg("ResolverContextCache::removeCachedObject removed object from CommonCache");
        return;
    }

    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
        .Msg("ResolverContextCache::removeCachedObject the object could not be found");
};

void
ResolverContextCache::removeCachedObject(const std::string &key, const CacheName selectedCache) {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::removeCachedObject (%s) \n", key.c_str());

    std::unordered_set<AssetIdentifier>::iterator hit;

    std::unique_lock<std::shared_mutex> preCacheSharedDeleteLock(m_PreCacheSharedMutex);
    hit = m_PreCache.find(key);
    if (hit != m_PreCache.end()) {
        m_PreCache.erase(hit);
        preCacheSharedDeleteLock.unlock();
        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
            .Msg("ResolverContextCache::removeCachedObject removed object from PreCache");

        return;
    }
    else {
        switch (selectedCache) {
            case CacheName::AYONCACHE:
                {
                    std::unique_lock<std::shared_mutex> AyonCachesharedDellLock(m_AyonCacheSharedMutex);

                    hit = m_AyonCache.find(key);
                    if (hit != m_AyonCache.end()) {
                        m_AyonCache.erase(hit);
                        AyonCachesharedDellLock.unlock();
                        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
                            .Msg("ResolverContextCache::removeCachedObject removed object from AyonCache");
                        return;
                    }
                    break;
                }
            case CacheName::COMMONCACHE:
                {
                    std::unique_lock<std::shared_mutex> CommonCachesharedDellLock(m_CommonCacheSharedMutex);
                    hit = m_CommonCache.find(key);
                    if (hit != m_CommonCache.end()) {
                        m_CommonCache.erase(hit);
                        CommonCachesharedDellLock.unlock();
                        TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
                            .Msg("ResolverContextCache::removeCachedObject removed object from CommonCache");
                        return;
                    }
                    break;
                }
        }
    }

    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT)
        .Msg("ResolverContextCache::removeCachedObject the object could not be found");
};

void
ResolverContextCache::ClearCache() {
    TF_DEBUG(AYONUSDRESOLVER_RESOLVER_CONTEXT).Msg("ResolverContextCache::ClearCache \n");

    std::unique_lock<std::shared_mutex> PreCachesharedMutexLock(m_PreCacheSharedMutex);
    std::unique_lock<std::shared_mutex> AyonCachesharedMutexLock(m_AyonCacheSharedMutex);
    std::unique_lock<std::shared_mutex> CommonCachesharedMutexLock(m_CommonCacheSharedMutex);
    m_CommonCache.clear();
    m_AyonCache.clear();
    m_PreCache.clear();
};

bool
ResolverContextCache::isCacheStatic() const {
    return m_staticCache;
};

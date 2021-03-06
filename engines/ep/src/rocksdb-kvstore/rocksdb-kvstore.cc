/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include "rocksdb-kvstore.h"

#include "ep_time.h"

#include "kvstore_config.h"
#include "kvstore_priv.h"

#include <platform/sysinfo.h>
#include <rocksdb/convenience.h>

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <limits>
#include <thread>

#include "vbucket.h"

namespace rockskv {
// MetaData is used to serialize and de-serialize metadata respectively when
// writing a Document mutation request to RocksDB and when reading a Document
// from RocksDB.
class MetaData {
public:
    MetaData()
        : deleted(0),
          version(0),
          datatype(0),
          flags(0),
          valueSize(0),
          exptime(0),
          cas(0),
          revSeqno(0),
          bySeqno(0){};
    MetaData(bool deleted,
             uint8_t version,
             uint8_t datatype,
             uint32_t flags,
             uint32_t valueSize,
             time_t exptime,
             uint64_t cas,
             uint64_t revSeqno,
             int64_t bySeqno)
        : deleted(deleted),
          version(version),
          datatype(datatype),
          flags(flags),
          valueSize(valueSize),
          exptime(exptime),
          cas(cas),
          revSeqno(revSeqno),
          bySeqno(bySeqno){};

// The `#pragma pack(1)` directive and the order of members are to keep
// the size of MetaData as small as possible and uniform across different
// platforms.
#pragma pack(1)
    uint8_t deleted : 1;
    uint8_t version : 7;
    uint8_t datatype;
    uint32_t flags;
    uint32_t valueSize;
    time_t exptime;
    uint64_t cas;
    uint64_t revSeqno;
    int64_t bySeqno;
#pragma pack()
};
} // namespace rockskv

/**
 * Class representing a document to be persisted in RocksDB.
 */
class RocksRequest : public IORequest {
public:
    /**
     * Constructor
     *
     * @param item Item instance to be persisted
     * @param callback Persistence Callback
     * @param del Flag indicating if it is an item deletion or not
     */
    RocksRequest(const Item& item, MutationRequestCallback& callback)
        : IORequest(item.getVBucketId(),
                    callback,
                    item.isDeleted(),
                    item.getKey()),
          docBody(item.getValue()) {
        docMeta = rockskv::MetaData(
                item.isDeleted(),
                0,
                item.getDataType(),
                item.getFlags(),
                item.getNBytes(),
                item.isDeleted() ? ep_real_time() : item.getExptime(),
                item.getCas(),
                item.getRevSeqno(),
                item.getBySeqno());
    }

    const rockskv::MetaData& getDocMeta() {
        return docMeta;
    }

    // Get a rocksdb::Slice wrapping the Document MetaData
    rocksdb::Slice getDocMetaSlice() {
        return rocksdb::Slice(reinterpret_cast<char*>(&docMeta),
                              sizeof(docMeta));
    }

    // Get a rocksdb::Slice wrapping the Document Body
    rocksdb::Slice getDocBodySlice() {
        const char* data = docBody ? docBody->getData() : nullptr;
        size_t size = docBody ? docBody->valueSize() : 0;
        return rocksdb::Slice(data, size);
    }

private:
    rockskv::MetaData docMeta;
    value_t docBody;
};

using RDBPtr = std::unique_ptr<rocksdb::DB>;
// RocksDB docs suggest to "Use `rocksdb::DB::DestroyColumnFamilyHandle()` to
// close a column family instead of deleting the column family handle directly"
struct ColumnFamilyDeleter {
    ColumnFamilyDeleter(rocksdb::DB& db) : db(db) {
    }
    void operator()(rocksdb::ColumnFamilyHandle* cfh) {
        db.DestroyColumnFamilyHandle(cfh);
    }

private:
    rocksdb::DB& db;
};
using ColumnFamilyPtr =
        std::unique_ptr<rocksdb::ColumnFamilyHandle, ColumnFamilyDeleter>;

// The `KVRocksDB` class is a wrapper around an instance of `rocksdb::DB` and
// the linked Column Family pointers (which are usually used together).
// Also, this class guarantees that all resources are released when the
// `rocksdb::DB` instance is destroyed. From RocksDB docs:
//     "Before delete DB, you have to close All column families by calling
//      DestroyColumnFamilyHandle() with all the handles."
class KVRocksDB {
public:
    KVRocksDB(rocksdb::DB* rdb,
              rocksdb::ColumnFamilyHandle* defaultCFH,
              rocksdb::ColumnFamilyHandle* seqnoCFH,
              rocksdb::ColumnFamilyHandle* localCFH,
              uint16_t vbid)
        : rdb(RDBPtr(rdb)),
          defaultCFH(ColumnFamilyPtr(defaultCFH, *rdb)),
          seqnoCFH(ColumnFamilyPtr(seqnoCFH, *rdb)),
          localCFH(ColumnFamilyPtr(localCFH, *rdb)),
          vbid(vbid) {
    }

    const RDBPtr rdb;
    const ColumnFamilyPtr defaultCFH;
    const ColumnFamilyPtr seqnoCFH;
    const ColumnFamilyPtr localCFH;
    const uint16_t vbid;
};

RocksDBKVStore::RocksDBKVStore(KVStoreConfig& config)
    : KVStore(config),
      vbDB(configuration.getMaxVBuckets()),
      in_transaction(false),
      scanCounter(0),
      logger(config.getLogger()) {
    cachedVBStates.resize(configuration.getMaxVBuckets());
    writeOptions.sync = true;

    createDataDir(configuration.getDBName());

    // The RocksDB Options is a set of DBOptions and ColumnFamilyOptions.
    // Together they cover all RocksDB available parameters.
    auto status = rocksdb::GetDBOptionsFromString(
            dbOptions, configuration.getRocksDBOptions(), &dbOptions);
    if (!status.ok()) {
        throw std::invalid_argument(
                std::string("RocksDBKVStore::open: GetDBOptionsFromString "
                            "error: ") +
                status.getState());
    }

    // Set number of background threads - note these are per-environment, so
    // are shared across all DB instances (vBuckets) and all Buckets.
    auto lowPri = configuration.getRocksDbLowPriBackgroundThreads();
    if (lowPri == 0) {
        lowPri = cb::get_available_cpu_count();
    }
    rocksdb::Env::Default()->SetBackgroundThreads(lowPri, rocksdb::Env::LOW);

    auto highPri = configuration.getRocksDbHighPriBackgroundThreads();
    if (highPri == 0) {
        highPri = cb::get_available_cpu_count();
    }
    rocksdb::Env::Default()->SetBackgroundThreads(highPri, rocksdb::Env::HIGH);

    dbOptions.create_if_missing = true;
    dbOptions.create_missing_column_families = true;

    /* Use a listener to set the appropriate engine in the
     * flusher threads RocksDB creates. We need the flusher threads to
     * account for news/deletes against the appropriate bucket. */
    auto fsl = std::make_shared<FlushStartListener>(
            ObjectRegistry::getCurrentEngine());
    dbOptions.listeners.emplace_back(fsl);

    // Enable Statistics if 'Statistics::stat_level_' is provided by the
    // configuration. We create a statistics object and pass to the multiple
    // DBs managed by the same KVStore. Then the statistics object will contain
    // aggregated values for all those DBs. Note that some stats are undefined
    // and have no meaningful information across multiple DBs (e.g.,
    // "rocksdb.sequence.number").
    if (!configuration.getRocksdbStatsLevel().empty()) {
        dbOptions.statistics = rocksdb::CreateDBStatistics();
        dbOptions.statistics->stats_level_ =
                getStatsLevel(configuration.getRocksdbStatsLevel());
    }

    // Allocate the per-shard Block Cache
    if (configuration.getRocksdbBlockCacheSize() > 0) {
        blockCache =
                rocksdb::NewLRUCache(configuration.getRocksdbBlockCacheSize() /
                                     configuration.getMaxShards());
    }
    // Configure all the Column Families
    const auto& cfOptions = configuration.getRocksDBCFOptions();
    const auto& bbtOptions = configuration.getRocksDbBBTOptions();
    defaultCFOptions = getBaselineDefaultCFOptions();
    seqnoCFOptions = getBaselineSeqnoCFOptions();
    localCFOptions = getBaselineLocalCFOptions();
    applyUserCFOptions(defaultCFOptions, cfOptions, bbtOptions);
    applyUserCFOptions(seqnoCFOptions, cfOptions, bbtOptions);
    applyUserCFOptions(localCFOptions, cfOptions, bbtOptions);

    // Read persisted VBs state
    auto vbids = discoverVBuckets();
    for (auto vbid : vbids) {
        const auto db = openDB(vbid);
        readVBState(*db);
        // Update stats
        ++st.numLoadedVb;
    }
}

RocksDBKVStore::~RocksDBKVStore() {
    in_transaction = false;
}

std::shared_ptr<KVRocksDB> RocksDBKVStore::openDB(uint16_t vbid) {
    std::lock_guard<std::mutex> lg(vbDBMutex);

    if (vbDB[vbid]) {
        return vbDB[vbid];
    }

    auto dbname = getVBDBSubdir(vbid);

    std::vector<rocksdb::ColumnFamilyDescriptor> families{
            rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName,
                                            defaultCFOptions),
            rocksdb::ColumnFamilyDescriptor("vbid_seqno_to_key",
                                            seqnoCFOptions),
            rocksdb::ColumnFamilyDescriptor("_local", localCFOptions)};

    std::vector<rocksdb::ColumnFamilyHandle*> handles;

    rocksdb::DB* db;
    auto status = rocksdb::DB::Open(dbOptions, dbname, families, &handles, &db);
    if (!status.ok()) {
        throw std::runtime_error(
                "RocksDBKVStore::open: failed to open database '" + dbname +
                "': " + status.getState());
    }

    vbDB[vbid] = std::make_shared<KVRocksDB>(
            db, handles[0], handles[1], handles[2], vbid);

    return vbDB[vbid];
}

std::string RocksDBKVStore::getVBDBSubdir(uint16_t vbid) {
    return configuration.getDBName() + "/rocksdb." + std::to_string(vbid);
}

std::vector<uint16_t> RocksDBKVStore::discoverVBuckets() {
    std::vector<uint16_t> vbids;
    auto vbDirs =
            cb::io::findFilesContaining(configuration.getDBName(), "rocksdb.");
    for (auto& dir : vbDirs) {
        size_t lastDotIndex = dir.rfind(".");
        size_t vbidLength = dir.size() - lastDotIndex - 1;
        std::string vbidStr = dir.substr(lastDotIndex + 1, vbidLength);
        uint16_t vbid = atoi(vbidStr.c_str());
        // Take in account only VBuckets managed by this Shard
        if ((vbid % configuration.getMaxShards()) ==
            configuration.getShardId()) {
            vbids.push_back(vbid);
        }
    }
    return vbids;
}

bool RocksDBKVStore::begin(std::unique_ptr<TransactionContext> txCtx) {
    in_transaction = true;
    transactionCtx = std::move(txCtx);
    return in_transaction;
}

bool RocksDBKVStore::commit(const Item* collectionsManifest) {
    // This behaviour is to replicate the one in Couchstore.
    // If `commit` is called when not in transaction, just return true.
    if (!in_transaction) {
        return true;
    }

    if (pendingReqs.size() == 0) {
        in_transaction = false;
        return true;
    }

    // Swap `pendingReqs` with the temporary `commitBatch` so that we can
    // shorten the scope of the lock.
    std::vector<std::unique_ptr<RocksRequest>> commitBatch;
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        std::swap(pendingReqs, commitBatch);
    }

    bool success = true;
    auto vbid = commitBatch[0]->getVBucketId();

    // Flush all documents to disk
    auto status = saveDocs(vbid, collectionsManifest, commitBatch);
    if (!status.ok()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::commit: saveDocs error:%d, "
                   "vb:%" PRIu16,
                   status.code(),
                   vbid);
        success = false;
    }

    commitCallback(status, commitBatch);

    // This behaviour is to replicate the one in Couchstore.
    // Set `in_transanction = false` only if `commit` is successful.
    if (success) {
        in_transaction = false;
        transactionCtx.reset();
    }

    return success;
}

static int getMutationStatus(rocksdb::Status status) {
    switch (status.code()) {
    case rocksdb::Status::Code::kOk:
        return MUTATION_SUCCESS;
    case rocksdb::Status::Code::kNotFound:
        // This return value causes ep-engine to drop the failed flush
        return DOC_NOT_FOUND;
    case rocksdb::Status::Code::kBusy:
        // This return value causes ep-engine to keep re-queueing the failed
        // flush
        return MUTATION_FAILED;
    default:
        throw std::runtime_error(
                std::string("getMutationStatus: RocksDB error:") +
                std::string(status.getState()));
    }
}

void RocksDBKVStore::commitCallback(
        rocksdb::Status status,
        const std::vector<std::unique_ptr<RocksRequest>>& commitBatch) {
    for (const auto& request : commitBatch) {
        auto dataSize = request->getDocMetaSlice().size() +
                        request->getDocBodySlice().size();
        const auto& key = request->getKey();
        /* update ep stats */
        ++st.io_num_write;
        st.io_write_bytes += (key.size() + dataSize);

        auto rv = getMutationStatus(status);
        if (request->isDelete()) {
            if (status.code()) {
                ++st.numDelFailure;
            } else {
                st.delTimeHisto.add(request->getDelta() / 1000);
            }
            if (rv != -1) {
                // TODO: Should set `rv` to 1 or 0 depending on if this is a
                // delete to an existing (1) or non-existing (0) item. However,
                // to achieve this we would need to perform a Get to RocksDB
                // which is costly. For now just assume that the item did exist.
                rv = 1;
            }
            request->getDelCallback()->callback(*transactionCtx, rv);
        } else {
            if (status.code()) {
                ++st.numSetFailure;
            } else {
                st.writeTimeHisto.add(request->getDelta() / 1000);
                st.writeSizeHisto.add(dataSize + key.size());
            }
            // TODO: Should set `mr.second` to true or false depending on if
            // this is an insertion (true) or an update of an existing item
            // (false). However, to achieve this we would need to perform a Get
            // to RocksDB which is costly. For now just assume that the item
            // did not exist.
            mutation_result mr = std::make_pair(1, true);
            request->getSetCallback()->callback(*transactionCtx, mr);
        }
    }
}

void RocksDBKVStore::rollback() {
    if (in_transaction) {
        in_transaction = false;
        transactionCtx.reset();
    }
}

std::vector<vbucket_state*> RocksDBKVStore::listPersistedVbuckets() {
    std::vector<vbucket_state*> result;
    for (const auto& vb : cachedVBStates) {
        result.emplace_back(vb.get());
    }
    return result;
}

void RocksDBKVStore::set(const Item& item,
                         Callback<TransactionContext, mutation_result>& cb) {
    if (!in_transaction) {
        throw std::logic_error(
                "RocksDBKVStore::set: in_transaction must be true to perform a "
                "set operation.");
    }
    MutationRequestCallback callback;
    callback.setCb = &cb;
    pendingReqs.push_back(std::make_unique<RocksRequest>(item, callback));
}

GetValue RocksDBKVStore::get(const DocKey& key, uint16_t vb, bool fetchDelete) {
    return getWithHeader(nullptr, key, vb, GetMetaOnly::No, fetchDelete);
}

GetValue RocksDBKVStore::getWithHeader(void* dbHandle,
                                       const DocKey& key,
                                       uint16_t vb,
                                       GetMetaOnly getMetaOnly,
                                       bool fetchDelete) {
    std::string value;
    const auto db = openDB(vb);
    // TODO RDB: use a PinnableSlice to avoid some memcpy
    rocksdb::Slice keySlice = getKeySlice(key);
    rocksdb::Status s = db->rdb->Get(rocksdb::ReadOptions(), keySlice, &value);
    if (!s.ok()) {
        return GetValue{NULL, ENGINE_KEY_ENOENT};
    }
    return makeGetValue(vb, key, value, getMetaOnly);
}

void RocksDBKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t& itms) {
    // TODO RDB: RocksDB supports a multi get which we should use here.
    for (auto& it : itms) {
        auto& key = it.first;
        rocksdb::Slice keySlice = getKeySlice(key);
        std::string value;
        const auto db = openDB(vb);
        rocksdb::Status s =
                db->rdb->Get(rocksdb::ReadOptions(), keySlice, &value);
        if (s.ok()) {
            it.second.value =
                    makeGetValue(vb, key, value, it.second.isMetaOnly);
            GetValue* rv = &it.second.value;
            for (auto& fetch : it.second.bgfetched_list) {
                fetch->value = rv;
            }
        } else {
            for (auto& fetch : it.second.bgfetched_list) {
                fetch->value->setStatus(ENGINE_KEY_ENOENT);
            }
        }
    }
}

void RocksDBKVStore::reset(uint16_t vbucketId) {
    // TODO RDB:  Implement.
}

void RocksDBKVStore::del(const Item& item,
                         Callback<TransactionContext, int>& cb) {
    if (!in_transaction) {
        throw std::logic_error(
                "RocksDBKVStore::del: in_transaction must be true to perform a "
                "delete operation.");
    }
    // TODO: Deleted items remain as tombstones, but are not yet expired,
    // they will accumuate forever.
    MutationRequestCallback callback;
    callback.delCb = &cb;
    pendingReqs.push_back(std::make_unique<RocksRequest>(item, callback));
}

void RocksDBKVStore::delVBucket(uint16_t vbid, uint64_t vb_version) {
    std::lock_guard<std::mutex> lg1(writeMutex);
    std::lock_guard<std::mutex> lg2(vbDBMutex);

    if (!vbDB[vbid]) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::delVBucket: DB not found, vb:%" PRIu16,
                   vbid);
        return;
    }

    // 'vbDB' stores shared_ptr for each VBucket DB. The ownership of each
    // pointer is shared among multiple threads performing different operations
    // (e.g., 'get' and 'commit').
    // We want to call 'DestroyDB' here rather than in '~KVRocksDB' because
    // it is an expensive, IO-intensive operation and we do not want it to
    // cause another thread (possibly a front-end one) from being blocked
    // performing the destroy.
    // So, the thread executing 'delVBucket' spins until it is the exclusive
    // owner of the shared_ptr (i.e., other concurrent threads like 'commit'
    // have completed and do not own any copy of the shared_ptr).
    {
        std::shared_ptr<KVRocksDB> sharedPtr;
        std::swap(vbDB[vbid], sharedPtr);
        while (!sharedPtr.unique()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    // Just destroy the DB in the sub-folder for vbid
    auto dbname = getVBDBSubdir(vbid);
    auto status = rocksdb::DestroyDB(
            dbname, static_cast<const rocksdb::Options&>(dbOptions));
    if (!status.ok()) {
        throw std::runtime_error("RocksDBKVStore::open: DestroyDB '" + dbname +
                                 "' failed: " + status.getState());
    }
}

bool RocksDBKVStore::snapshotVBucket(uint16_t vbucketId,
                                     const vbucket_state& vbstate,
                                     VBStatePersist options) {
    // TODO RDB: Refactor out behaviour common to this and CouchKVStore
    auto start = ProcessClock::now();

    if (updateCachedVBState(vbucketId, vbstate) &&
        (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
         options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT)) {
        const auto db = openDB(vbucketId);
        rocksdb::WriteBatch batch;
        auto status = saveVBStateToBatch(*db, vbstate, batch);
        if (!status.ok()) {
            logger.log(EXTENSION_LOG_WARNING,
                       "RocksDBKVStore::snapshotVBucket: saveVBStateToBatch() "
                       "failed state:%s vb:%" PRIu16 " :%s",
                       VBucket::toString(vbstate.state),
                       vbucketId,
                       status.getState());
            return false;
        }
        status = db->rdb->Write(writeOptions, &batch);
        if (!status.ok()) {
            logger.log(EXTENSION_LOG_WARNING,
                       "RocksDBKVStore::snapshotVBucket: Write() "
                       "failed state:%s vb:%" PRIu16 " :%s",
                       VBucket::toString(vbstate.state),
                       vbucketId,
                       status.getState());
            return false;
        }
    }

    LOG(EXTENSION_LOG_DEBUG,
        "RocksDBKVStore::snapshotVBucket: Snapshotted vbucket:%" PRIu16
        " state:%s",
        vbucketId,
        vbstate.toJSON().c_str());

    st.snapshotHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - start));

    return true;
}

bool RocksDBKVStore::snapshotStats(const std::map<std::string, std::string>&) {
    // TODO RDB:  Implement
    return true;
}

void RocksDBKVStore::destroyInvalidVBuckets(bool) {
    // TODO RDB:  implement
}

size_t RocksDBKVStore::getNumShards() {
    return configuration.getMaxShards();
}

bool RocksDBKVStore::getStat(const char* name_, size_t& value) {
    std::string name(name_);

    // Memory Usage
    if (name == "kMemTableTotal") {
        return getStatFromMemUsage(rocksdb::MemoryUtil::kMemTableTotal, value);
    } else if (name == "kMemTableUnFlushed") {
        return getStatFromMemUsage(rocksdb::MemoryUtil::kMemTableUnFlushed,
                                   value);
    } else if (name == "kTableReadersTotal") {
        return getStatFromMemUsage(rocksdb::MemoryUtil::kTableReadersTotal,
                                   value);
    } else if (name == "kCacheTotal") {
        return getStatFromMemUsage(rocksdb::MemoryUtil::kCacheTotal, value);
    }

    // MemTable Size per Column Famiy
    else if (name == "default_kSizeAllMemTables") {
        return getStatFromProperties(ColumnFamily::Default,
                                     rocksdb::DB::Properties::kSizeAllMemTables,
                                     value);
    } else if (name == "seqno_kSizeAllMemTables") {
        return getStatFromProperties(ColumnFamily::Seqno,
                                     rocksdb::DB::Properties::kSizeAllMemTables,
                                     value);
    } else if (name == "local_kSizeAllMemTables") {
        return getStatFromProperties(ColumnFamily::Local,
                                     rocksdb::DB::Properties::kSizeAllMemTables,
                                     value);
    }

    // Block Cache hit/miss
    else if (name == "rocksdb.block.cache.hit") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_HIT, value);
    } else if (name == "rocksdb.block.cache.miss") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_MISS, value);
    } else if (name == "rocksdb.block.cache.data.hit") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_DATA_HIT,
                                     value);
    } else if (name == "rocksdb.block.cache.data.miss") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_DATA_MISS,
                                     value);
    } else if (name == "rocksdb.block.cache.index.hit") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_INDEX_HIT,
                                     value);
    } else if (name == "rocksdb.block.cache.index.miss") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_INDEX_MISS,
                                     value);
    } else if (name == "rocksdb.block.cache.filter.hit") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_FILTER_HIT,
                                     value);
    } else if (name == "rocksdb.block.cache.filter.miss") {
        return getStatFromStatistics(rocksdb::Tickers::BLOCK_CACHE_FILTER_MISS,
                                     value);
    }

    // Disk Usage per Column Family
    else if (name == "default_kTotalSstFilesSize") {
        return getStatFromProperties(
                ColumnFamily::Default,
                rocksdb::DB::Properties::kTotalSstFilesSize,
                value);
    } else if (name == "seqno_kTotalSstFilesSize") {
        return getStatFromProperties(
                ColumnFamily::Seqno,
                rocksdb::DB::Properties::kTotalSstFilesSize,
                value);
    } else if (name == "local_kTotalSstFilesSize") {
        return getStatFromProperties(
                ColumnFamily::Local,
                rocksdb::DB::Properties::kTotalSstFilesSize,
                value);
    }

    return false;
}

StorageProperties RocksDBKVStore::getStorageProperties(void) {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::No,
                         // TODO RDB: Not strictly true, multiGet
                         // does not yet use the underlying multi get
                         // of RocksDB
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::Yes);
    return rv;
}

std::unordered_set<const rocksdb::Cache*> RocksDBKVStore::getCachePointers(
        const std::vector<rocksdb::DB*>& dbs) {
    std::unordered_set<const rocksdb::Cache*> cache_set;

    for (const auto* db : dbs) {
        if (db) {
            // TODO: Cache from DBImpl. The 'std::shared_ptr<Cache>
            // table_cache_' pointer is not exposed through the 'DB' interface

            // Cache from DBOptions
            // Note: we do not use the 'row_cache' currently. As the Block
            // Cache, it can be shared among multiple DBs.
            cache_set.insert(db->GetDBOptions().row_cache.get());
        }
    }

    // Cache from table factories.
    addCFBlockCachePointers(defaultCFOptions, cache_set);
    addCFBlockCachePointers(seqnoCFOptions, cache_set);
    addCFBlockCachePointers(localCFOptions, cache_set);

    return cache_set;
}

void RocksDBKVStore::addCFBlockCachePointers(
        const rocksdb::ColumnFamilyOptions& cfOptions,
        std::unordered_set<const rocksdb::Cache*>& cache_set) {
    if (cfOptions.table_factory) {
        auto* table_options = cfOptions.table_factory->GetOptions();
        auto* bbt_options =
                static_cast<rocksdb::BlockBasedTableOptions*>(table_options);
        cache_set.insert(bbt_options->block_cache.get());
        cache_set.insert(bbt_options->block_cache_compressed.get());
    }
}

rocksdb::StatsLevel RocksDBKVStore::getStatsLevel(
        const std::string& stats_level) {
    if (stats_level == "kExceptDetailedTimers") {
        return rocksdb::StatsLevel::kExceptDetailedTimers;
    } else if (stats_level == "kExceptTimeForMutex") {
        return rocksdb::StatsLevel::kExceptTimeForMutex;
    } else if (stats_level == "kAll") {
        return rocksdb::StatsLevel::kAll;
    } else {
        throw std::invalid_argument(
                std::string("RocksDBKVStore::getStatsLevel: stats_level: '") +
                stats_level + std::string("'"));
    }
}

rocksdb::Slice RocksDBKVStore::getKeySlice(const DocKey& key) {
    return rocksdb::Slice(reinterpret_cast<const char*>(key.data()),
                          key.size());
}

rocksdb::Slice RocksDBKVStore::getSeqnoSlice(const int64_t* seqno) {
    return rocksdb::Slice(reinterpret_cast<const char*>(seqno), sizeof(*seqno));
}

int64_t RocksDBKVStore::getNumericSeqno(const rocksdb::Slice& seqnoSlice) {
    assert(seqnoSlice.size() == sizeof(int64_t));
    int64_t seqno;
    std::memcpy(&seqno, seqnoSlice.data(), seqnoSlice.size());
    return seqno;
}

std::unique_ptr<Item> RocksDBKVStore::makeItem(uint16_t vb,
                                               const DocKey& key,
                                               const rocksdb::Slice& s,
                                               GetMetaOnly getMetaOnly) {
    assert(s.size() >= sizeof(rockskv::MetaData));

    const char* data = s.data();

    rockskv::MetaData meta;
    std::memcpy(&meta, data, sizeof(meta));
    data += sizeof(meta);

    bool includeValue = getMetaOnly == GetMetaOnly::No && meta.valueSize;

    auto item = std::make_unique<Item>(key,
                                       meta.flags,
                                       meta.exptime,
                                       includeValue ? data : nullptr,
                                       includeValue ? meta.valueSize : 0,
                                       meta.datatype,
                                       meta.cas,
                                       meta.bySeqno,
                                       vb,
                                       meta.revSeqno);

    if (meta.deleted) {
        item->setDeleted();
    }

    return item;
}

GetValue RocksDBKVStore::makeGetValue(uint16_t vb,
                                      const DocKey& key,
                                      const std::string& value,
                                      GetMetaOnly getMetaOnly) {
    rocksdb::Slice sval(value);
    return GetValue(
            makeItem(vb, key, sval, getMetaOnly), ENGINE_SUCCESS, -1, 0);
}

void RocksDBKVStore::readVBState(const KVRocksDB& db) {
    // Largely copied from CouchKVStore
    // TODO RDB: refactor out sections common to CouchKVStore
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    int64_t highSeqno = readHighSeqnoFromDisk(db);
    std::string failovers;
    uint64_t purgeSeqno = 0;
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t hlcCasEpochSeqno = HlcCasSeqnoUninitialised;
    bool mightContainXattrs = false;

    auto key = getVbstateKey();
    std::string vbstate;
    auto vbid = db.vbid;
    auto status = db.rdb->Get(
            rocksdb::ReadOptions(), db.localCFH.get(), key, &vbstate);
    if (!status.ok()) {
        if (status.IsNotFound()) {
            logger.log(EXTENSION_LOG_NOTICE,
                       "RocksDBKVStore::readVBState: '_local/vbstate.%" PRIu16
                       "' not found",
                       vbid);
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "RocksDBKVStore::readVBState: error getting vbstate "
                       "error:%s, vb:%" PRIu16,
                       status.getState(),
                       vbid);
        }
    } else {
        cJSON* jsonObj = cJSON_Parse(vbstate.c_str());
        if (!jsonObj) {
            logger.log(EXTENSION_LOG_WARNING,
                       "RocksKVStore::readVBState: Failed to parse the vbstat "
                       "json doc for vb:%" PRIu16 ", json:%s",
                       vbid,
                       vbstate.c_str());
        }

        const std::string vb_state =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "state"));
        const std::string checkpoint_id =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "checkpoint_id"));
        const std::string max_deleted_seqno = getJSONObjString(
                cJSON_GetObjectItem(jsonObj, "max_deleted_seqno"));
        const std::string snapStart =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "snap_start"));
        const std::string snapEnd =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "snap_end"));
        const std::string maxCasValue =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "max_cas"));
        const std::string hlcCasEpoch =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "hlc_epoch"));
        mightContainXattrs = getJSONObjBool(
                cJSON_GetObjectItem(jsonObj, "might_contain_xattrs"));

        cJSON* failover_json = cJSON_GetObjectItem(jsonObj, "failover_table");
        if (vb_state.compare("") == 0 || checkpoint_id.compare("") == 0 ||
            max_deleted_seqno.compare("") == 0) {
            logger.log(EXTENSION_LOG_WARNING,
                       "RocksDBKVStore::readVBState: State"
                       " JSON doc for vb:%" PRIu16
                       " is in the wrong format:%s, "
                       "vb state:%s, checkpoint id:%s and max deleted seqno:%s",
                       vbid,
                       vbstate.c_str(),
                       vb_state.c_str(),
                       checkpoint_id.c_str(),
                       max_deleted_seqno.c_str());
        } else {
            state = VBucket::fromString(vb_state.c_str());
            maxDeletedSeqno = std::stoull(max_deleted_seqno);
            checkpointId = std::stoull(checkpoint_id);

            if (snapStart.compare("") == 0) {
                lastSnapStart = highSeqno;
            } else {
                lastSnapStart = std::stoull(snapStart.c_str());
            }

            if (snapEnd.compare("") == 0) {
                lastSnapEnd = highSeqno;
            } else {
                lastSnapEnd = std::stoull(snapEnd.c_str());
            }

            if (maxCasValue.compare("") != 0) {
                maxCas = std::stoull(maxCasValue.c_str());
            }

            if (!hlcCasEpoch.empty()) {
                hlcCasEpochSeqno = std::stoull(hlcCasEpoch);
            }

            if (failover_json) {
                char* json = cJSON_PrintUnformatted(failover_json);
                failovers.assign(json);
                cJSON_Free(json);
            }
        }
        cJSON_Delete(jsonObj);
    }

    cachedVBStates[vbid] = std::make_unique<vbucket_state>(state,
                                                           checkpointId,
                                                           maxDeletedSeqno,
                                                           highSeqno,
                                                           purgeSeqno,
                                                           lastSnapStart,
                                                           lastSnapEnd,
                                                           maxCas,
                                                           hlcCasEpochSeqno,
                                                           mightContainXattrs,
                                                           failovers);
}

rocksdb::Status RocksDBKVStore::saveVBStateToBatch(const KVRocksDB& db,
                                                   const vbucket_state& vbState,
                                                   rocksdb::WriteBatch& batch) {
    std::stringstream jsonState;

    jsonState << "{\"state\": \"" << VBucket::toString(vbState.state) << "\""
              << ",\"checkpoint_id\": \"" << vbState.checkpointId << "\""
              << ",\"max_deleted_seqno\": \"" << vbState.maxDeletedSeqno
              << "\"";
    if (!vbState.failovers.empty()) {
        jsonState << ",\"failover_table\": " << vbState.failovers;
    }
    jsonState << ",\"snap_start\": \"" << vbState.lastSnapStart << "\""
              << ",\"snap_end\": \"" << vbState.lastSnapEnd << "\""
              << ",\"max_cas\": \"" << vbState.maxCas << "\""
              << ",\"hlc_epoch\": \"" << vbState.hlcCasEpochSeqno << "\"";

    if (vbState.mightContainXattrs) {
        jsonState << ",\"might_contain_xattrs\": true";
    } else {
        jsonState << ",\"might_contain_xattrs\": false";
    }

    jsonState << "}";

    auto key = getVbstateKey();
    return batch.Put(db.localCFH.get(), key, jsonState.str());
}

rocksdb::ColumnFamilyOptions RocksDBKVStore::getBaselineDefaultCFOptions() {
    rocksdb::ColumnFamilyOptions cfOptions;

    // Enable Point Lookup Optimization for the 'default' Column Family
    // Note: whatever we give in input as 'block_cache_size_mb', the Block
    // Cache will be reset with the shared 'blockCache' of size
    // 'rocksdb_block_cache_size'
    cfOptions.OptimizeForPointLookup(1);

    // Set the given Memory Budget as the write_buffer_size
    if (configuration.getRocksdbDefaultCfMemBudget() > 0) {
        cfOptions.write_buffer_size =
                configuration.getRocksdbDefaultCfMemBudget();
    }

    // Overwrite Compaction options if Compaction Optimization is enabled
    // for the 'default' CF
    if (configuration.getRocksdbDefaultCfOptimizeCompaction() == "level") {
        cfOptions.OptimizeLevelStyleCompaction(cfOptions.write_buffer_size);
    } else if (configuration.getRocksdbDefaultCfOptimizeCompaction() ==
               "universal") {
        cfOptions.OptimizeUniversalStyleCompaction(cfOptions.write_buffer_size);
    }

    return cfOptions;
}

rocksdb::ColumnFamilyOptions RocksDBKVStore::getBaselineSeqnoCFOptions() {
    rocksdb::ColumnFamilyOptions cfOptions;

    cfOptions.comparator = &vbidSeqnoComparator;

    // Set the given Memory Budget as the write_buffer_size
    if (configuration.getRocksdbSeqnoCfMemBudget() > 0) {
        cfOptions.write_buffer_size =
                configuration.getRocksdbSeqnoCfMemBudget();
    }

    // Overwrite Compaction options if Compaction Optimization is enabled
    // for the 'seqno' CF
    if (configuration.getRocksdbSeqnoCfOptimizeCompaction() == "level") {
        cfOptions.OptimizeLevelStyleCompaction(cfOptions.write_buffer_size);
    } else if (configuration.getRocksdbSeqnoCfOptimizeCompaction() ==
               "universal") {
        cfOptions.OptimizeUniversalStyleCompaction(cfOptions.write_buffer_size);
    }

    return cfOptions;
}

rocksdb::ColumnFamilyOptions RocksDBKVStore::getBaselineLocalCFOptions() {
    rocksdb::ColumnFamilyOptions cfOptions;
    // Set the Memtable size to the current RocksDB minimum (64KB)
    cfOptions.write_buffer_size = 65536;
    return cfOptions;
}

void RocksDBKVStore::applyUserCFOptions(rocksdb::ColumnFamilyOptions& cfOptions,
                                        const std::string& newCfOptions,
                                        const std::string& newBbtOptions) {
    // Apply 'newCfOptions' on top of 'cfOptions'
    auto status = rocksdb::GetColumnFamilyOptionsFromString(
            cfOptions, newCfOptions, &cfOptions);
    if (!status.ok()) {
        throw std::invalid_argument(
                std::string("RocksDBKVStore::applyUserCFOptions:  "
                            "GetColumnFamilyOptionsFromString error: ") +
                status.getState());
    }

    // RocksDB ColumnFamilyOptions provide advanced options for the
    // Block Based Table file format, which is the default format for SST files.
    // Apply 'newBbtOptions' on top of the current BlockBasedTableOptions of
    // 'cfOptions'
    rocksdb::BlockBasedTableOptions baseOptions;
    if (cfOptions.table_factory) {
        auto* bbtOptions = cfOptions.table_factory->GetOptions();
        if (bbtOptions) {
            baseOptions = *(
                    static_cast<rocksdb::BlockBasedTableOptions*>(bbtOptions));
        }
    }

    rocksdb::BlockBasedTableOptions tableOptions;
    status = rocksdb::GetBlockBasedTableOptionsFromString(
            baseOptions, newBbtOptions, &tableOptions);
    if (!status.ok()) {
        throw std::invalid_argument(
                std::string("RocksDBKVStore::applyUserCFOptions: "
                            "GetBlockBasedTableOptionsFromString error: ") +
                status.getState());
    }

    // Always use the per-shard shared Block Cache. If it is nullptr, RocksDB
    // will allocate a default size Block Cache.
    tableOptions.block_cache = blockCache;

    // Set the new BlockBasedTableOptions
    cfOptions.table_factory.reset(
            rocksdb::NewBlockBasedTableFactory(tableOptions));
}

rocksdb::Status RocksDBKVStore::writeAndTimeBatch(const KVRocksDB& db,
                                                  rocksdb::WriteBatch batch) {
    auto begin = ProcessClock::now();
    auto status = db.rdb->Write(writeOptions, &batch);
    st.commitHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - begin));
    return status;
}

rocksdb::Status RocksDBKVStore::saveDocs(
        uint16_t vbid,
        const Item* collectionsManifest,
        const std::vector<std::unique_ptr<RocksRequest>>& commitBatch) {
    auto reqsSize = commitBatch.size();
    if (reqsSize == 0) {
        st.docsCommitted = 0;
        return rocksdb::Status::OK();
    }

    auto& vbstate = cachedVBStates[vbid];
    if (vbstate == nullptr) {
        throw std::logic_error("RocksDBKVStore::saveDocs: cachedVBStates[" +
                               std::to_string(vbid) + "] is NULL");
    }

    rocksdb::Status status;
    int64_t maxDBSeqno = 0;
    rocksdb::WriteBatch batch;

    const auto db = openDB(vbid);

    for (const auto& request : commitBatch) {
        int64_t bySeqno = request->getDocMeta().bySeqno;
        maxDBSeqno = std::max(maxDBSeqno, bySeqno);

        status = addRequestToWriteBatch(*db, batch, request.get());
        if (!status.ok()) {
            logger.log(EXTENSION_LOG_WARNING,
                       "RocksDBKVStore::saveDocs: addRequestToWriteBatch "
                       "error:%d, vb:%" PRIu16,
                       status.code(),
                       vbid);
            return status;
        }

        // Check if we should split into a new writeBatch if the batch size
        // exceeds the write_buffer_size - this is necessary because we
        // don't want our WriteBatch to exceed the configured memtable size, as
        // that can cause significant memory bloating (see MB-26521).
        // Note the limit check is only approximate, as the batch contains
        // updates for at least 2 CFs (key & seqno) which will be written into
        // separate memtables, so we don't exactly know the size contribution
        // to each memtable in the batch.
        const auto batchLimit = defaultCFOptions.write_buffer_size +
                                seqnoCFOptions.write_buffer_size;
        if (batch.GetDataSize() > batchLimit) {
            status = writeAndTimeBatch(*db, batch);
            if (!status.ok()) {
                logger.log(EXTENSION_LOG_WARNING,
                           "RocksDBKVStore::saveDocs: rocksdb::DB::Write "
                           "error:%d, "
                           "vb:%" PRIu16,
                           status.code(),
                           vbid);
                return status;
            }
            batch.Clear();
        }
    }

    status = saveVBStateToBatch(*db, *vbstate, batch);
    if (!status.ok()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::saveDocs: saveVBStateToBatch error:%d",
                   status.code());
        return status;
    }

    status = writeAndTimeBatch(*db, batch);
    if (!status.ok()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::saveDocs: rocksdb::DB::Write error:%d, "
                   "vb:%" PRIu16,
                   status.code(),
                   vbid);
        return status;
    }

    st.batchSize.add(reqsSize);
    st.docsCommitted = reqsSize;

    // Check and update last seqno
    auto lastSeqno = readHighSeqnoFromDisk(*db);
    if (maxDBSeqno != lastSeqno) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::saveDocs: Seqno in db header (%" PRIu64
                   ") is not matched with what was persisted (%" PRIu64
                   ") for vb:%" PRIu16,
                   lastSeqno,
                   maxDBSeqno,
                   vbid);
    }
    vbstate->highSeqno = lastSeqno;

    return rocksdb::Status::OK();
}

rocksdb::Status RocksDBKVStore::addRequestToWriteBatch(
        const KVRocksDB& db,
        rocksdb::WriteBatch& batch,
        RocksRequest* request) {
    uint16_t vbid = request->getVBucketId();

    rocksdb::Slice keySlice = getKeySlice(request->getKey());
    rocksdb::SliceParts keySliceParts(&keySlice, 1);

    rocksdb::Slice docSlices[] = {request->getDocMetaSlice(),
                                  request->getDocBodySlice()};
    rocksdb::SliceParts valueSliceParts(docSlices, 2);

    rocksdb::Slice bySeqnoSlice = getSeqnoSlice(&request->getDocMeta().bySeqno);
    // We use the `saveDocsHisto` to track the time spent on
    // `rocksdb::WriteBatch::Put()`.
    auto begin = ProcessClock::now();
    auto status = batch.Put(keySliceParts, valueSliceParts);
    if (!status.ok()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::saveDocs: rocksdb::WriteBatch::Put "
                   "[ColumnFamily: \'default\']  error:%d, "
                   "vb:%" PRIu16,
                   status.code(),
                   vbid);
        return status;
    }
    status = batch.Put(db.seqnoCFH.get(), bySeqnoSlice, keySlice);
    if (!status.ok()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "RocksDBKVStore::saveDocs: rocksdb::WriteBatch::Put "
                   "[ColumnFamily: \'seqno\']  error:%d, "
                   "vb:%" PRIu16,
                   status.code(),
                   vbid);
        return status;
    }
    st.saveDocsHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - begin));

    return rocksdb::Status::OK();
}

int64_t RocksDBKVStore::readHighSeqnoFromDisk(const KVRocksDB& db) {
    std::unique_ptr<rocksdb::Iterator> it(
            db.rdb->NewIterator(rocksdb::ReadOptions(), db.seqnoCFH.get()));

    // Seek to the highest seqno=>key mapping stored for the vbid
    auto maxSeqno = std::numeric_limits<int64_t>::max();
    rocksdb::Slice maxSeqnoSlice = getSeqnoSlice(&maxSeqno);
    it->SeekForPrev(maxSeqnoSlice);

    if (!it->Valid()) {
        return 0;
    }

    return getNumericSeqno(it->key());
}

std::string RocksDBKVStore::getVbstateKey() {
    return "vbstate";
}

ScanContext* RocksDBKVStore::initScanContext(
        std::shared_ptr<StatusCallback<GetValue>> cb,
        std::shared_ptr<StatusCallback<CacheLookup>> cl,
        uint16_t vbid,
        uint64_t startSeqno,
        DocumentFilter options,
        ValueFilter valOptions) {
    size_t scanId = scanCounter++;
    const auto db = openDB(vbid);
    scanSnapshots.emplace(scanId,
                          SnapshotPtr(db->rdb->GetSnapshot(), *db->rdb));

    // As we cannot efficiently determine how many documents this scan will
    // find, we approximate this value with the seqno difference + 1
    // as scan is supposed to be inclusive at both ends,
    // seqnos 2 to 4 covers 3 docs not 4 - 2 = 2

    uint64_t endSeqno = cachedVBStates[vbid]->highSeqno;
    return new ScanContext(cb,
                           cl,
                           vbid,
                           scanId,
                           startSeqno,
                           endSeqno,
                           options,
                           valOptions,
                           /* documentCount */ endSeqno - startSeqno + 1,
                           configuration);
}

scan_error_t RocksDBKVStore::scan(ScanContext* ctx) {
    if (!ctx) {
        return scan_failed;
    }

    if (ctx->lastReadSeqno == ctx->maxSeqno) {
        return scan_success;
    }

    auto startSeqno = ctx->startSeqno;
    if (ctx->lastReadSeqno != 0) {
        startSeqno = ctx->lastReadSeqno + 1;
    }

    GetMetaOnly isMetaOnly = ctx->valFilter == ValueFilter::KEYS_ONLY
                                     ? GetMetaOnly::Yes
                                     : GetMetaOnly::No;

    rocksdb::ReadOptions snapshotOpts{rocksdb::ReadOptions()};
    snapshotOpts.snapshot = scanSnapshots.at(ctx->scanId).get();

    rocksdb::Slice startSeqnoSlice = getSeqnoSlice(&startSeqno);
    const auto db = openDB(ctx->vbid);
    std::unique_ptr<rocksdb::Iterator> it(
            db->rdb->NewIterator(snapshotOpts, db->seqnoCFH.get()));
    if (!it) {
        throw std::logic_error(
                "RocksDBKVStore::scan: rocksdb::Iterator to Seqno Column "
                "Family is nullptr");
    }
    it->Seek(startSeqnoSlice);

    rocksdb::Slice endSeqnoSlice = getSeqnoSlice(&ctx->maxSeqno);
    auto isPastEnd = [&endSeqnoSlice, this](rocksdb::Slice seqSlice) {
        return vbidSeqnoComparator.Compare(seqSlice, endSeqnoSlice) == 1;
    };

    for (; it->Valid() && !isPastEnd(it->key()); it->Next()) {
        auto seqno = getNumericSeqno(it->key());
        rocksdb::Slice keySlice = it->value();
        std::string valueStr;
        auto s = db->rdb->Get(snapshotOpts, keySlice, &valueStr);

        if (!s.ok()) {
            // TODO RDB: Old seqnos are never removed from the db!
            // If the item does not exist (s.isNotFound())
            // the seqno => key mapping could be removed; not even
            // a tombstone remains of that item.
            continue;
        }

        rocksdb::Slice valSlice(valueStr);

        // TODO RDB: Deal with collections
        DocKey key(reinterpret_cast<const uint8_t*>(keySlice.data()),
                   keySlice.size(),
                   DocNamespace::DefaultCollection);

        std::unique_ptr<Item> itm =
                makeItem(ctx->vbid, key, valSlice, isMetaOnly);

        if (itm->getBySeqno() > seqno) {
            // TODO RDB: Old seqnos are never removed from the db!
            // If the item has a newer seqno now, the stale
            // seqno => key mapping could be removed
            continue;
        } else if (itm->getBySeqno() < seqno) {
            throw std::logic_error(
                    "RocksDBKVStore::scan: index has a higher seqno"
                    "than the document in a snapshot!");
        }

        bool includeDeletes =
                (ctx->docFilter == DocumentFilter::NO_DELETES) ? false : true;
        bool onlyKeys =
                (ctx->valFilter == ValueFilter::KEYS_ONLY) ? true : false;

        if (!includeDeletes && itm->isDeleted()) {
            continue;
        }
        int64_t byseqno = itm->getBySeqno();
        CacheLookup lookup(key, byseqno, ctx->vbid);
        ctx->lookup->callback(lookup);

        int status = ctx->lookup->getStatus();

        if (status == ENGINE_KEY_EEXISTS) {
            ctx->lastReadSeqno = byseqno;
            continue;
        } else if (status == ENGINE_ENOMEM) {
            return scan_again;
        }

        GetValue rv(std::move(itm), ENGINE_SUCCESS, -1, onlyKeys);
        ctx->callback->callback(rv);
        status = ctx->callback->getStatus();

        if (status == ENGINE_ENOMEM) {
            return scan_again;
        }

        ctx->lastReadSeqno = byseqno;
    }

    cb_assert(it->status().ok()); // Check for any errors found during the scan

    return scan_success;
}

void RocksDBKVStore::destroyScanContext(ScanContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    // TODO RDB: Might be nice to have the snapshot in the ctx and
    // release it on destruction
    auto it = scanSnapshots.find(ctx->scanId);
    if (it != scanSnapshots.end()) {
        scanSnapshots.erase(it);
    }
    delete ctx;
}

bool RocksDBKVStore::getStatFromMemUsage(
        const rocksdb::MemoryUtil::UsageType type, size_t& value) {
    std::vector<rocksdb::DB*> dbs;
    std::map<rocksdb::MemoryUtil::UsageType, uint64_t> usageByType;
    {
        // Note: we need to call 'GetApproximateMemoryUsageByType' under lock
        // to avoid that 'delVBucket' deletes pointers in 'dbs'
        std::lock_guard<std::mutex> lg(vbDBMutex);
        for (const auto db : vbDB) {
            if (db) {
                dbs.push_back(db->rdb.get());
            }
        }

        if (dbs.empty()) {
            return false;
        }
        auto cache_set = getCachePointers(dbs);

        auto status = rocksdb::MemoryUtil::GetApproximateMemoryUsageByType(
                dbs, cache_set, &usageByType);
        if (!status.ok()) {
            logger.log(EXTENSION_LOG_NOTICE,
                       "RocksDBKVStore::getStatFromMemUsage: "
                       "GetApproximateMemoryUsageByType error: %s",
                       status.getState());
            return false;
        }
    }

    value = usageByType.at(type);

    return true;
}

bool RocksDBKVStore::getStatFromStatistics(const rocksdb::Tickers ticker,
                                           size_t& value) {
    std::lock_guard<std::mutex> lg(vbDBMutex);
    for (const auto db : vbDB) {
        if (db) {
            const auto statistics = db->rdb->GetDBOptions().statistics;
            if (!statistics) {
                return false;
            }
            value += statistics->getTickerCount(ticker);
        }
    }

    return true;
}

bool RocksDBKVStore::getStatFromProperties(ColumnFamily cf,
                                           const std::string& property,
                                           size_t& value) {
    std::lock_guard<std::mutex> lg(vbDBMutex);
    for (const auto db : vbDB) {
        if (db) {
            rocksdb::ColumnFamilyHandle* cfh = nullptr;
            switch (cf) {
            case ColumnFamily::Default:
                cfh = db->defaultCFH.get();
                break;
            case ColumnFamily::Seqno:
                cfh = db->seqnoCFH.get();
                break;
            case ColumnFamily::Local:
                cfh = db->localCFH.get();
                break;
            }
            if (!cfh) {
                return false;
            }
            std::string out;
            if (!db->rdb->GetProperty(cfh, property, &out)) {
                return false;
            }
            value += std::stoi(out);
        }
    }

    return true;
}

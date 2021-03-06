/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
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

/**
 * Tests for Collection functionality in EPStore.
 */
#include "bgfetcher.h"
#include "dcp/dcpconnmap.h"
#include "kvstore.h"
#include "programs/engine_testapp/mock_server.h"
#include "tests/mock/mock_dcp.h"
#include "tests/mock/mock_dcp_consumer.h"
#include "tests/mock/mock_dcp_producer.h"
#include "tests/mock/mock_global_task.h"
#include "tests/module_tests/evp_store_single_threaded_test.h"
#include "tests/module_tests/evp_store_test.h"
#include "tests/module_tests/test_helpers.h"

#include <functional>
#include <thread>

extern uint8_t dcp_last_op;
extern std::string dcp_last_key;

class CollectionsDcpTest : public SingleThreadedKVBucketTest {
public:
    CollectionsDcpTest()
        : cookieC(create_mock_cookie()), cookieP(create_mock_cookie()) {
    }

    // Setup a producer/consumer ready for the test
    void SetUp() override {
        config_string += "collections_prototype_enabled=true";
        SingleThreadedKVBucketTest::SetUp();
        // Start vbucket as active to allow us to store items directly to it.
        store->setVBucketState(vbid, vbucket_state_active, false);
        producers = get_dcp_producers(
                reinterpret_cast<ENGINE_HANDLE*>(engine.get()),
                reinterpret_cast<ENGINE_HANDLE_V1*>(engine.get()));
        createDcpObjects({/*no filter*/}, true /*collections on*/);
    }
    std::string getManifest(uint16_t vb) const {
        return store->getVBucket(vb)
                ->getShard()
                ->getRWUnderlying()
                ->getCollectionsManifest(vbid);
    }

    void createDcpObjects(const std::string& filter, bool dcpCollectionAware) {
        CollectionsDcpTest::consumer = std::make_shared<MockDcpConsumer>(
                *engine, cookieC, "test_consumer");

        int flags = DCP_OPEN_INCLUDE_XATTRS;
        if (dcpCollectionAware) {
            flags |= DCP_OPEN_COLLECTIONS;
        }
        producer = std::make_shared<MockDcpProducer>(
                *engine,
                cookieP,
                "test_producer",
                flags,
                cb::const_byte_buffer(
                        reinterpret_cast<const uint8_t*>(filter.data()),
                        filter.size()),
                false /*startTask*/);

        // Create the task object, but don't schedule
        producer->createCheckpointProcessorTask();

        // Need to enable NOOP for XATTRS (and collections).
        producer->setNoopEnabled(true);

        store->setVBucketState(replicaVB, vbucket_state_replica, false);
        ASSERT_EQ(ENGINE_SUCCESS,
                  consumer->addStream(/*opaque*/ 0,
                                      replicaVB,
                                      /*flags*/ 0));
        uint64_t rollbackSeqno;
        ASSERT_EQ(ENGINE_SUCCESS,
                  producer->streamRequest(
                          0, // flags
                          1, // opaque
                          vbid,
                          0, // start_seqno
                          ~0ull, // end_seqno
                          0, // vbucket_uuid,
                          0, // snap_start_seqno,
                          0, // snap_end_seqno,
                          &rollbackSeqno,
                          &CollectionsDcpTest::dcpAddFailoverLog));

        // Patch our local callback into the handlers
        producers->system_event = &CollectionsDcpTest::sendSystemEvent;

        // Setup a snapshot on the consumer
        ASSERT_EQ(ENGINE_SUCCESS,
                  consumer->snapshotMarker(/*opaque*/ 1,
                                           /*vbucket*/ replicaVB,
                                           /*start_seqno*/ 0,
                                           /*end_seqno*/ 100,
                                           /*flags*/ 0));
    }

    void TearDown() override {
        teardown();
        SingleThreadedKVBucketTest::TearDown();
    }

    void teardown() {
        destroy_mock_cookie(cookieC);
        destroy_mock_cookie(cookieP);
        consumer->closeAllStreams();
        consumer->cancelTask();
        producer->closeAllStreams();
        producer.reset();
        consumer.reset();
    }

    void runCheckpointProcessor() {
        // Step which will notify the snapshot task
        EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));

        EXPECT_EQ(1, producer->getCheckpointSnapshotTask().queueSize());

        // Now call run on the snapshot task to move checkpoint into DCP
        // stream
        producer->getCheckpointSnapshotTask().run();
    }

    void notifyAndStepToCheckpoint(bool expectSnapshot = true,
                                   bool fromMemory = true) {
        auto vb = store->getVBucket(vbid);
        ASSERT_NE(nullptr, vb.get());

        if (fromMemory) {
            producer->notifySeqnoAvailable(vbid, vb->getHighSeqno());
            runCheckpointProcessor();
        } else {
            // Run a backfill
            auto& lpAuxioQ = *task_executor->getLpTaskQ()[AUXIO_TASK_IDX];
            // backfill:create()
            runNextTask(lpAuxioQ);
            // backfill:scan()
            runNextTask(lpAuxioQ);
            // backfill:complete()
            runNextTask(lpAuxioQ);
            // backfill:finished()
            runNextTask(lpAuxioQ);
        }

        // Next step which will process a snapshot marker and then the caller
        // should now be able to step through the checkpoint
        if (expectSnapshot) {
            EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));
            EXPECT_EQ(PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER, dcp_last_op);
        } else {
            EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));
        }
    }

    void testDcpCreateDelete(int expectedCreates,
                             int expectedDeletes,
                             int expectedMutations,
                             bool fromMemory = true);

    void resetEngineAndWarmup() {
        teardown();
        SingleThreadedKVBucketTest::resetEngineAndWarmup();
        producers = get_dcp_producers(
                reinterpret_cast<ENGINE_HANDLE*>(engine.get()),
                reinterpret_cast<ENGINE_HANDLE_V1*>(engine.get()));
        cookieC = create_mock_cookie();
        cookieP = create_mock_cookie();
    }

    static const uint16_t replicaVB{1};
    static std::shared_ptr<MockDcpConsumer> consumer;
    static mcbp::systemevent::id dcp_last_system_event;

    /*
     * DCP callback method to push SystemEvents on to the consumer
     */
    static ENGINE_ERROR_CODE sendSystemEvent(gsl::not_null<const void*> cookie,
                                             uint32_t opaque,
                                             uint16_t vbucket,
                                             mcbp::systemevent::id event,
                                             uint64_t bySeqno,
                                             cb::const_byte_buffer key,
                                             cb::const_byte_buffer eventData) {
        (void)cookie;
        (void)vbucket; // ignored as we are connecting VBn to VBn+1
        dcp_last_op = PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT;
        dcp_last_key.assign(reinterpret_cast<const char*>(key.data()),
                            key.size());
        dcp_last_system_event = event;
        return consumer->systemEvent(
                opaque, replicaVB, event, bySeqno, key, eventData);
    }

    static ENGINE_ERROR_CODE dcpAddFailoverLog(
            vbucket_failover_t* entry,
            size_t nentries,
            gsl::not_null<const void*> cookie) {
        return ENGINE_SUCCESS;
    }

    const void* cookieC;
    const void* cookieP;
    std::unique_ptr<dcp_message_producers> producers;
    std::shared_ptr<MockDcpProducer> producer;
};

std::shared_ptr<MockDcpConsumer> CollectionsDcpTest::consumer;
mcbp::systemevent::id CollectionsDcpTest::dcp_last_system_event;

TEST_F(CollectionsDcpTest, test_dcp_consumer) {
    const void* cookie = create_mock_cookie();

    auto consumer =
            std::make_shared<MockDcpConsumer>(*engine, cookie, "test_consumer");

    store->setVBucketState(vbid, vbucket_state_replica, false);
    ASSERT_EQ(ENGINE_SUCCESS,
              consumer->addStream(/*opaque*/ 0, vbid, /*flags*/ 0));

    std::string collection = "meat";

    Collections::uid_t uid = 4;
    ASSERT_EQ(ENGINE_SUCCESS,
              consumer->snapshotMarker(/*opaque*/ 1,
                                       vbid,
                                       /*start_seqno*/ 0,
                                       /*end_seqno*/ 100,
                                       /*flags*/ 0));

    VBucketPtr vb = store->getVBucket(vbid);

    EXPECT_FALSE(vb->lockCollections().doesKeyContainValidCollection(
            {"meat:bacon", DocNamespace::Collections}));

    // Call the consumer function for handling DCP events
    // create the meat collection
    EXPECT_EQ(ENGINE_SUCCESS,
              consumer->systemEvent(
                      /*opaque*/ 1,
                      vbid,
                      mcbp::systemevent::id::CreateCollection,
                      /*seqno*/ 1,
                      {reinterpret_cast<const uint8_t*>(collection.data()),
                       collection.size()},
                      {reinterpret_cast<const uint8_t*>(&uid), sizeof(uid)}));

    // We can now access the collection
    EXPECT_TRUE(vb->lockCollections().doesKeyContainValidCollection(
            {"meat:bacon", DocNamespace::Collections}));

    // Call the consumer function for handling DCP events
    // delete the meat collection
    EXPECT_EQ(ENGINE_SUCCESS,
              consumer->systemEvent(
                      /*opaque*/ 1,
                      vbid,
                      mcbp::systemevent::id::DeleteCollection,
                      /*seqno*/ 2,
                      {reinterpret_cast<const uint8_t*>(collection.data()),
                       collection.size()},
                      {reinterpret_cast<const uint8_t*>(&uid), sizeof(uid)}));

    // It's gone!
    EXPECT_FALSE(vb->lockCollections().doesKeyContainValidCollection(
            {"meat:bacon", DocNamespace::Collections}));

    consumer->closeAllStreams();
    destroy_mock_cookie(cookie);
    consumer->cancelTask();
}

/*
 * test_dcp connects a producer and consumer to test that collections created
 * on the producer are transferred to the consumer
 *
 * The test replicates VBn to VBn+1
 */
TEST_F(CollectionsDcpTest, test_dcp) {
    VBucketPtr vb = store->getVBucket(vbid);

    // Add a collection, then remove it. This generated events into the CP which
    // we'll manually replicate with calls to step
    vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat","uid":"1"}]})"});

    notifyAndStepToCheckpoint();

    VBucketPtr replica = store->getVBucket(replicaVB);

    // 1. Replica does not know about meat
    EXPECT_FALSE(replica->lockCollections().doesKeyContainValidCollection(
            {"meat:bacon", DocNamespace::Collections}));

    // Now step the producer to transfer the collection creation
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // 1. Replica now knows the collection
    EXPECT_TRUE(replica->lockCollections().doesKeyContainValidCollection(
            {"meat:bacon", DocNamespace::Collections}));

    // remove meat
    vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"}]})"});

    notifyAndStepToCheckpoint();

    // Now step the producer to transfer the collection deletion
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // 3. Replica now blocking access to meat
    EXPECT_FALSE(replica->lockCollections().doesKeyContainValidCollection(
            {"meat:bacon", DocNamespace::Collections}));

    // Now step the producer, no more collection events
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));
}

void CollectionsDcpTest::testDcpCreateDelete(int expectedCreates,
                                             int expectedDeletes,
                                             int expectedMutations,
                                             bool fromMemory) {
    notifyAndStepToCheckpoint(true /* expect a snapshot*/, fromMemory);

    int creates = 0, deletes = 0, mutations = 0;
    // step until done
    while (ENGINE_WANT_MORE == producer->step(producers.get())) {
        if (dcp_last_op == PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT) {
            switch (dcp_last_system_event) {
            case mcbp::systemevent::id::CreateCollection:
                creates++;
                break;
            case mcbp::systemevent::id::DeleteCollection:
                deletes++;
                break;
            case mcbp::systemevent::id::CollectionsSeparatorChanged: {
                EXPECT_FALSE(true);
                break;
            }
            }
        } else if (dcp_last_op == PROTOCOL_BINARY_CMD_DCP_MUTATION) {
            mutations++;
        }
    }

    EXPECT_EQ(expectedCreates, creates);
    EXPECT_EQ(expectedDeletes, deletes);
    EXPECT_EQ(expectedMutations, mutations);

    // Finally check that the active and replica have the same manifest, our
    // BeginDeleteCollection should of contained enough information to form
    // an equivalent manifest
    EXPECT_EQ(getManifest(vbid), getManifest(vbid + 1));
}

// Test that a create/delete don't dedup (collections creates new checkpoints)
TEST_F(CollectionsDcpTest, test_dcp_create_delete) {
    const int items = 3;
    {
        VBucketPtr vb = store->getVBucket(vbid);
        // Create dairy
        vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"fruit","uid":"1"},
                             {"name":"dairy","uid":"1"}]})"});

        // Mutate dairy
        for (int ii = 0; ii < items; ii++) {
            std::string key = "dairy:" + std::to_string(ii);
            store_item(vbid, {key, DocNamespace::Collections}, "value");
        }

        // Mutate fruit
        for (int ii = 0; ii < items; ii++) {
            std::string key = "fruit:" + std::to_string(ii);
            store_item(vbid, {key, DocNamespace::Collections}, "value");
        }

        // Delete dairy
        vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"fruit","uid":"1"}]})"});

        // Persist everything ready for warmup and check.
        // Flusher will merge create/delete and we only flush the delete
        flush_vbucket_to_disk(0, (2 * items) + 2);

        // We will see create fruit/dairy and delete dairy (from another CP)
        // In-memory stream will also see all 2*items mutations (ordered with
        // create
        // and delete)
        testDcpCreateDelete(2, 1, (2 * items));
    }

    resetEngineAndWarmup();

    createDcpObjects({}, true); // from disk

    // Streamed from disk, one create (create of fruit) and items of fruit
    testDcpCreateDelete(1, 0, items, false);

    EXPECT_TRUE(store->getVBucket(vbid)->lockCollections().isCollectionOpen(
            "fruit"));
}

// Test that a create/delete don't dedup (collections creates new checkpoints)
TEST_F(CollectionsDcpTest, test_dcp_create_delete_create) {
    {
        VBucketPtr vb = store->getVBucket(vbid);
        // Create dairy
        vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"dairy","uid":"1"}]})"});

        // Mutate dairy
        const int items = 3;
        for (int ii = 0; ii < items; ii++) {
            std::string key = "dairy:" + std::to_string(ii);
            store_item(vbid, {key, DocNamespace::Collections}, "value");
        }

        // Delete dairy
        vb->updateFromManifest(
                {R"({"separator":":","collections":[{"name":"$default", "uid":"0"}]})"});

        // Create dairy (new uid)
        vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"dairy","uid":"2"}]})"});

        // Persist everything ready for warmup and check.
        // Flusher will merge create/delete and we only flush the delete
        flush_vbucket_to_disk(0, items + 1);

        // Should see 2x create dairy and 1x delete dairy
        testDcpCreateDelete(2, 1, items);
    }

    resetEngineAndWarmup();

    createDcpObjects({}, true /* from disk*/);

    // Streamed from disk, we won't see the 2x create events or the intermediate
    // delete. So check DCP sends only 1 collection create.
    testDcpCreateDelete(1, 0, 0, false);

    EXPECT_TRUE(store->getVBucket(vbid)->lockCollections().isCollectionOpen(
            "dairy"));
}

// Test that a create/delete/create don't dedup
TEST_F(CollectionsDcpTest, test_dcp_create_delete_create2) {
    {
        VBucketPtr vb = store->getVBucket(vbid);
        // Create dairy
        vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"dairy","uid":"1"}]})"});

        // Mutate dairy
        const int items = 3;
        for (int ii = 0; ii < items; ii++) {
            std::string key = "dairy:" + std::to_string(ii);
            store_item(vbid, {key, DocNamespace::Collections}, "value");
        }

        // Delete dairy/create dairy in one update
        vb->updateFromManifest({R"({"separator":":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"dairy","uid":"2"}]})"});

        // Persist everything ready for warmup and check.
        // Flusher will merge create/delete and we only flush the delete
        flush_vbucket_to_disk(0, items + 1);

        testDcpCreateDelete(2, 1, 3);
    }

    resetEngineAndWarmup();

    createDcpObjects({}, true /* from disk*/);

    // Streamed from disk, we won't see the first create or delete
    testDcpCreateDelete(1, 0, 0, false);

    EXPECT_TRUE(store->getVBucket(vbid)->lockCollections().isCollectionOpen(
            "dairy"));
}

TEST_F(CollectionsDcpTest, test_dcp_separator) {
    VBucketPtr vb = store->getVBucket(vbid);

    // Change the separator
    vb->updateFromManifest({R"({"separator":"@@",
              "collections":[{"name":"$default", "uid":"0"}]})"});

    // Add a collection
    vb->updateFromManifest({R"({"separator":"@@",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat","uid":"1"}]})"});

    // The producer should start with the old separator
    EXPECT_EQ(":", producer->getCurrentSeparatorForStream(vbid));

    notifyAndStepToCheckpoint();

    VBucketPtr replica = store->getVBucket(replicaVB);

    // The replica should have the old : separator
    EXPECT_EQ(":", replica->lockCollections().getSeparator());

    // Now step the producer to transfer the separator
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // The producer should now have the new separator
    EXPECT_EQ("@@", producer->getCurrentSeparatorForStream(vbid));

    // The replica should now have the new separator
    EXPECT_EQ("@@", replica->lockCollections().getSeparator());

    // Now step the producer to transfer the collection
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // Collection should now be live on the replica
    EXPECT_TRUE(replica->lockCollections().doesKeyContainValidCollection(
            {"meat@@bacon", DocNamespace::Collections}));

    // And done
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));
}

TEST_F(CollectionsDcpTest, test_dcp_separator_many) {
    auto vb = store->getVBucket(vbid);

    // Change the separator
    vb->updateFromManifest({R"({"separator": "@@",
              "collections":[{"name":"$default", "uid":"0"}]})"});
    // Change the separator
    vb->updateFromManifest({R"({"separator": "-",
              "collections":[{"name":"$default", "uid":"0"}]})"});
    // Change the separator
    vb->updateFromManifest({R"({"separator": ",",
              "collections":[{"name":"$default", "uid":"0"}]})"});
    // Add a collection
    vb->updateFromManifest({R"({"separator": ",",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"}]})"});

    // All the changes will be collapsed into one update and we will expect
    // to see , as the separator once DCP steps through the checkpoint

    // The producer should start with the initial separator
    EXPECT_EQ(":", producer->getCurrentSeparatorForStream(vbid));

    notifyAndStepToCheckpoint();

    auto replica = store->getVBucket(replicaVB);

    // The replica should have the old separator
    EXPECT_EQ(":", replica->lockCollections().getSeparator());

    std::array<std::string, 3> expectedData = {{"@@", "-", ","}};
    for (auto expected : expectedData) {
        // Now step the producer to transfer the separator
        EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

        // The producer should now have the new separator
        EXPECT_EQ(expected, producer->getCurrentSeparatorForStream(vbid));

        // The replica should now have the new separator
        EXPECT_EQ(expected, replica->lockCollections().getSeparator());
    }

    // Now step the producer to transfer the create "meat"
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // Collection should now be live on the replica with the final separator
    EXPECT_TRUE(replica->lockCollections().doesKeyContainValidCollection(
            {"meat,bacon", DocNamespace::Collections}));

    // And done
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));
}

class CollectionsFilteredDcpErrorTest : public SingleThreadedKVBucketTest {
public:
    CollectionsFilteredDcpErrorTest() : cookieP(create_mock_cookie()) {
    }
    void SetUp() override {
        config_string += "collections_prototype_enabled=true";
        SingleThreadedKVBucketTest::SetUp();
        // Start vbucket as active to allow us to store items directly to it.
        store->setVBucketState(vbid, vbucket_state_active, false);
    }

    void TearDown() override {
        destroy_mock_cookie(cookieP);
        producer.reset();
        SingleThreadedKVBucketTest::TearDown();
    }

protected:
    std::shared_ptr<MockDcpProducer> producer;
    const void* cookieP;
};

TEST_F(CollectionsFilteredDcpErrorTest, error1) {
    // Set some collections
    store->setCollections({R"({"separator": "@@",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"},
                             {"name":"dairy", "uid":"2"}]})"});

    std::string filter = R"({"collections":["fruit"]})";
    cb::const_byte_buffer buffer{
            reinterpret_cast<const uint8_t*>(filter.data()), filter.size()};
    // Can't create a filter for unknown collections
    EXPECT_THROW(std::make_unique<MockDcpProducer>(*engine,
                                                   cookieP,
                                                   "test_producer",
                                                   DCP_OPEN_COLLECTIONS,
                                                   buffer,
                                                   false /*startTask*/),
                 std::invalid_argument);
}

TEST_F(CollectionsFilteredDcpErrorTest, error2) {
    // Set some collections
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"},
                             {"name":"dairy", "uid":"2"}]})"});

    std::string filter = R"({"collections":["meat"]})";
    cb::const_byte_buffer buffer{
            reinterpret_cast<const uint8_t*>(filter.data()), filter.size()};
    // Can't create a filter for unknown collections
    producer = std::make_shared<MockDcpProducer>(*engine,
                                                 cookieP,
                                                 "test_producer",
                                                 DCP_OPEN_COLLECTIONS,
                                                 buffer,
                                                 false /*startTask*/);
    producer->setNoopEnabled(true);

    // Remove meat
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"dairy", "uid":"2"}]})"});

    // Now should be prevented from creating a new stream
    uint64_t rollbackSeqno = 0;
    EXPECT_EQ(ENGINE_UNKNOWN_COLLECTION,
              producer->streamRequest(0, // flags
                                      1, // opaque
                                      vbid,
                                      0, // start_seqno
                                      ~0ull, // end_seqno
                                      0, // vbucket_uuid,
                                      0, // snap_start_seqno,
                                      0, // snap_end_seqno,
                                      &rollbackSeqno,
                                      &CollectionsDcpTest::dcpAddFailoverLog));
}

class CollectionsFilteredDcpTest : public CollectionsDcpTest {
public:
    CollectionsFilteredDcpTest() : CollectionsDcpTest() {
    }

    void SetUp() override {
        config_string += "collections_prototype_enabled=true";
        SingleThreadedKVBucketTest::SetUp();
        producers = get_dcp_producers(
                reinterpret_cast<ENGINE_HANDLE*>(engine.get()),
                reinterpret_cast<ENGINE_HANDLE_V1*>(engine.get()));
        // Start vbucket as active to allow us to store items directly to it.
        store->setVBucketState(vbid, vbucket_state_active, false);
    }
};

TEST_F(CollectionsFilteredDcpTest, filtering) {
    VBucketPtr vb = store->getVBucket(vbid);

    // Perform a create of meat/dairy via the bucket level (filters are
    // worked out from the bucket manifest)
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"},
                             {"name":"dairy", "uid":"2"}]})"});
    // Setup filtered DCP
    createDcpObjects(R"({"collections":["dairy"]})", true);

    notifyAndStepToCheckpoint();

    // SystemEvent createCollection
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));
    EXPECT_EQ(PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT, dcp_last_op);
    EXPECT_EQ("dairy", dcp_last_key);

    // Store collection documents
    std::array<std::string, 2> expectedKeys = {{"dairy:one", "dairy:two"}};
    store_item(vbid, {"meat:one", DocNamespace::Collections}, "value");
    store_item(vbid, {expectedKeys[0], DocNamespace::Collections}, "value");
    store_item(vbid, {"meat:two", DocNamespace::Collections}, "value");
    store_item(vbid, {expectedKeys[1], DocNamespace::Collections}, "value");
    store_item(vbid, {"meat:three", DocNamespace::Collections}, "value");

    auto vb0Stream = producer->findStream(0);
    ASSERT_NE(nullptr, vb0Stream.get());

    notifyAndStepToCheckpoint();

    // Now step DCP to transfer keys, only two keys are expected as all "meat"
    // keys are filtered
    for (auto& key : expectedKeys) {
        EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));
        EXPECT_EQ(PROTOCOL_BINARY_CMD_DCP_MUTATION, dcp_last_op);
        EXPECT_EQ(key, dcp_last_key);
    }
    // And no more
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));

    flush_vbucket_to_disk(vbid, 7);

    vb.reset();

    // Now stream back from disk and check filtering
    resetEngineAndWarmup();

    // In order to create a filter, a manifest needs to be set
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"},
                             {"name":"dairy", "uid":"2"}]})"});

    createDcpObjects(R"({"collections":["dairy"]})", true);

    // Streamed from disk
    // 1 create - create of dairy
    // 2 mutations in the dairy collection
    testDcpCreateDelete(1, 0, 2, false);
}

// Check that when filtering is on, we don't send snapshots for fully filtered
// snapshots
TEST_F(CollectionsFilteredDcpTest, MB_24572) {
    VBucketPtr vb = store->getVBucket(vbid);

    // Perform a create of meat/dairy via the bucket level (filters are
    // worked out from the bucket manifest)
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"},
                             {"name":"dairy", "uid":"2"}]})"});
    // Setup filtered DCP
    createDcpObjects(R"({"collections":["dairy"]})", true);

    // Store collection documents
    store_item(vbid, {"meat::one", DocNamespace::Collections}, "value");
    store_item(vbid, {"meat::two", DocNamespace::Collections}, "value");
    store_item(vbid, {"meat::three", DocNamespace::Collections}, "value");

    notifyAndStepToCheckpoint();

    // SystemEvent createCollection for dairy is expected
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));
    EXPECT_EQ(PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT, dcp_last_op);
    EXPECT_EQ("dairy", dcp_last_key);

    // And no more for this stream - no meat
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));

    // and new mutations?
    store_item(vbid, {"meat::one1", DocNamespace::Collections}, "value");
    store_item(vbid, {"meat::two2", DocNamespace::Collections}, "value");
    store_item(vbid, {"meat::three3", DocNamespace::Collections}, "value");
    notifyAndStepToCheckpoint(false /* no checkpoint should be generated*/);
}

TEST_F(CollectionsFilteredDcpTest, default_only) {
    VBucketPtr vb = store->getVBucket(vbid);

    // Perform a create of meat/dairy via the bucket level (filters are
    // worked out from the bucket manifest)
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"},
                             {"name":"dairy", "uid":"2"}]})"});
    // Setup DCP
    createDcpObjects({/*no filter*/}, false /*don't know about collections*/);

    // Store collection documents and one default collection document
    store_item(vbid, {"meat:one", DocNamespace::Collections}, "value");
    store_item(vbid, {"dairy:one", DocNamespace::Collections}, "value");
    store_item(vbid, {"anykey", DocNamespace::DefaultCollection}, "value");
    store_item(vbid, {"dairy:two", DocNamespace::Collections}, "value");
    store_item(vbid, {"meat:three", DocNamespace::Collections}, "value");

    auto vb0Stream = producer->findStream(0);
    ASSERT_NE(nullptr, vb0Stream.get());

    // Now step into the items of which we expect to see only anykey
    notifyAndStepToCheckpoint();

    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));
    EXPECT_EQ(PROTOCOL_BINARY_CMD_DCP_MUTATION, dcp_last_op);
    EXPECT_EQ("anykey", dcp_last_key);

    // And no more
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));
}

TEST_F(CollectionsFilteredDcpTest, stream_closes) {
    VBucketPtr vb = store->getVBucket(vbid);

    // Perform a create of meat via the bucket level (filters are worked out
    // from the bucket manifest)
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"},
                             {"name":"meat", "uid":"1"}]})"});
    // Setup filtered DCP
    createDcpObjects(R"({"collections":["meat"]})", true);

    auto vb0Stream = producer->findStream(0);
    ASSERT_NE(nullptr, vb0Stream.get());

    notifyAndStepToCheckpoint();

    // Now step DCP to transfer system events. We expect that the stream will
    // close once we transfer DeleteCollection

    // Now step the producer to transfer the collection creation
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // Not dead yet...
    EXPECT_TRUE(vb0Stream->isActive());

    // Perform a delete of meat via the bucket level (filters are worked out
    // from the bucket manifest)
    store->setCollections({R"({"separator": ":",
              "collections":[{"name":"$default", "uid":"0"}]})"});

    notifyAndStepToCheckpoint();

    // Now step the producer to transfer the collection deletion
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // Done... collection deletion of meat has closed the stream
    EXPECT_FALSE(vb0Stream->isActive());

    // Now step the producer to transfer the close stream
    EXPECT_EQ(ENGINE_WANT_MORE, producer->step(producers.get()));

    // And no more
    EXPECT_EQ(ENGINE_SUCCESS, producer->step(producers.get()));
}

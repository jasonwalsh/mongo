/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/query/cursor_response.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class ClusterAggregateTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kAggregateCmdTargeted{
        fromjson("{aggregate: 'coll', pipeline: [{$match: {_id: 0}}], explain: false, "
                 "allowDiskUse: false, fromMongos: true, "
                 "cursor: {batchSize: 10}, maxTimeMS: 100, readConcern: {level: 'snapshot'}}")};

    const BSONObj kAggregateCmdScatterGather{fromjson(
        "{aggregate: 'coll', pipeline: [], explain: false, allowDiskUse: false, fromMongos: true, "
        "cursor: {batchSize: 10}, readConcern: {level: 'snapshot'}}")};

    // The index of the shard expected to receive the response is used to prevent different shards
    // from returning documents with the same shard key. This is expected to be 0 for queries
    // targeting one shard.
    void expectReturnsSuccess(int shardIndex) {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    void expectInspectRequest(int shardIndex, InspectionCallback cb) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            cb(request);

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }
};

TEST_F(ClusterAggregateTest, NoErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runCommandSuccessful(kAggregateCmdTargeted, true);

    // Target all shards.
    runCommandSuccessful(kAggregateCmdScatterGather, false);
}

// Verify aggregate through mongos will retry on a snapshot error.
TEST_F(ClusterAggregateTest, RetryOnSnapshotError) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runCommandOneError(kAggregateCmdTargeted, ErrorCodes::SnapshotUnavailable, true);
    runCommandOneError(kAggregateCmdTargeted, ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runCommandOneError(kAggregateCmdScatterGather, ErrorCodes::SnapshotUnavailable, false);
    runCommandOneError(kAggregateCmdScatterGather, ErrorCodes::SnapshotTooOld, false);
}

TEST_F(ClusterAggregateTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    auto containsAtClusterTime = [](const executor::RemoteCommandRequest& request) {
        ASSERT(!request.cmdObj["readConcern"]["atClusterTime"].eoo());
    };

    // Target one shard.
    runCommandInspectRequests(kAggregateCmdTargeted, containsAtClusterTime, true);

    // Target all shards.
    runCommandInspectRequests(kAggregateCmdScatterGather, containsAtClusterTime, false);
}

// Verify aggregate commands will retry up to its max retry attempts on snapshot errors
// then return the final error it receives.
TEST_F(ClusterAggregateTest, MaxRetriesSnapshotErrors) {
    loadRoutingTableWithTwoChunksAndTwoShards(kNss);

    // Target one shard.
    runCommandMaxErrors(kAggregateCmdTargeted, ErrorCodes::SnapshotUnavailable, true);
    runCommandMaxErrors(kAggregateCmdTargeted, ErrorCodes::SnapshotTooOld, true);

    // Target all shards
    runCommandMaxErrors(kAggregateCmdScatterGather, ErrorCodes::SnapshotUnavailable, false);
    runCommandMaxErrors(kAggregateCmdScatterGather, ErrorCodes::SnapshotTooOld, false);
}

}  // namespace
}  // namespace mongo

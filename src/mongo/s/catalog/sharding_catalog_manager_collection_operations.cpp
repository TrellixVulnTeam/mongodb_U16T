/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_manager.h"

#include <iomanip>
#include <set>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using CollectionUUID = UUID;
using std::string;
using std::vector;
using std::set;

namespace {

const Seconds kDefaultFindHostMaxWaitTime(20);

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

void checkForExistingChunks(OperationContext* opCtx, const string& ns) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", NamespaceString(ChunkType::ConfigNS).coll());
    countBuilder.append("query", BSON(ChunkType::ns(ns)));

    // OK to use limit=1, since if any chunks exist, we will fail.
    countBuilder.append("limit", 1);

    // Use readConcern local to guarantee we see any chunks that have been written and may
    // become committed; readConcern majority will not see the chunks if they have not made it
    // to the majority snapshot.
    repl::ReadConcernArgs readConcern(repl::ReadConcernLevel::kLocalReadConcern);
    readConcern.appendInfo(&countBuilder);

    auto cmdResponse = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kConfigReadSelector,
            NamespaceString(ChunkType::ConfigNS).db().toString(),
            countBuilder.done(),
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);

    long long numChunks;
    uassertStatusOK(bsonExtractIntegerField(cmdResponse.response, "n", &numChunks));
    uassert(ErrorCodes::ManualInterventionRequired,
            str::stream() << "A previous attempt to shard collection " << ns
                          << " failed after writing some initial chunks to config.chunks. Please "
                             "manually delete the partially written chunks for collection "
                          << ns
                          << " from config.chunks",
            numChunks == 0);
}

}  // namespace

/**
 * Creates and writes to the config server the first chunks for a newly sharded collection. Returns
 * the version generated for the collection.
 */
ChunkVersion ShardingCatalogManager::_createFirstChunks(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const ShardKeyPattern& shardKeyPattern,
                                                        const ShardId& primaryShardId,
                                                        const std::vector<BSONObj>& initPoints,
                                                        const bool distributeInitialChunks) {

    const KeyPattern keyPattern = shardKeyPattern.getKeyPattern();

    vector<BSONObj> splitPoints;
    vector<ShardId> shardIds;

    std::string primaryShardName = primaryShardId.toString();
    auto drainingCount = uassertStatusOK(_runCountCommandOnConfig(
        opCtx,
        NamespaceString(ShardType::ConfigNS),
        BSON(ShardType::name() << primaryShardName << ShardType::draining(true))));

    const bool primaryDraining = (drainingCount > 0);
    auto getPrimaryOrFirstNonDrainingShard =
        [&opCtx, primaryShardId, &shardIds, primaryDraining]() {
            if (primaryDraining) {
                vector<ShardId> allShardIds;
                Grid::get(opCtx)->shardRegistry()->getAllShardIds(&allShardIds);

                auto dbShardId = allShardIds[0];
                if (allShardIds[0] == primaryShardId && allShardIds.size() > 1) {
                    dbShardId = allShardIds[1];
                }

                return dbShardId;
            } else {
                return primaryShardId;
            }
        };

    if (initPoints.empty()) {
        // If no split points were specified use the shard's data distribution to determine them
        auto primaryShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShardId));

        auto result = uassertStatusOK(primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
            nss.db().toString(),
            BSON("count" << nss.coll()),
            Shard::RetryPolicy::kIdempotent));

        long long numObjects = 0;
        uassertStatusOK(result.commandStatus);
        uassertStatusOK(bsonExtractIntegerField(result.response, "n", &numObjects));

        // Refresh the balancer settings to ensure the chunk size setting, which is sent as part of
        // the splitVector command and affects the number of chunks returned, has been loaded.
        uassertStatusOK(Grid::get(opCtx)->getBalancerConfiguration()->refreshAndCheck(opCtx));

        if (numObjects > 0) {
            splitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
                opCtx,
                primaryShardId,
                nss,
                shardKeyPattern,
                ChunkRange(keyPattern.globalMin(), keyPattern.globalMax()),
                Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
                0));
        }

        // If docs already exist for the collection, must use primary shard,
        // otherwise defer to passed-in distribution option.
        if (numObjects == 0 && distributeInitialChunks) {
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
            if (primaryDraining && shardIds.size() > 1) {
                shardIds.erase(std::remove(shardIds.begin(), shardIds.end(), primaryShardId),
                               shardIds.end());
            }
        } else {
            shardIds.push_back(getPrimaryOrFirstNonDrainingShard());
        }
    } else {
        // Make sure points are unique and ordered
        auto orderedPts = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

        for (const auto& initPoint : initPoints) {
            orderedPts.insert(initPoint);
        }

        for (const auto& initPoint : orderedPts) {
            splitPoints.push_back(initPoint);
        }

        if (distributeInitialChunks) {
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
            if (primaryDraining) {
                shardIds.erase(std::remove(shardIds.begin(), shardIds.end(), primaryShardId),
                               shardIds.end());
            }
        } else {
            shardIds.push_back(getPrimaryOrFirstNonDrainingShard());
        }
    }

    // This is the first chunk; start the versioning from scratch
    const OID epoch = OID::gen();
    ChunkVersion version(1, 0, epoch);

    log() << "going to create " << splitPoints.size() + 1 << " chunk(s) for: " << nss
          << " using new epoch " << version.epoch();

    for (unsigned i = 0; i <= splitPoints.size(); i++) {
        const BSONObj min = (i == 0) ? keyPattern.globalMin() : splitPoints[i - 1];
        const BSONObj max = (i < splitPoints.size()) ? splitPoints[i] : keyPattern.globalMax();

        // The correct version must be returned as part of this call so only increment for versions,
        // which get written
        if (i > 0) {
            version.incMinor();
        }

        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(min);
        chunk.setMax(max);
        chunk.setShard(shardIds[i % shardIds.size()]);
        chunk.setVersion(version);

        uassertStatusOK(Grid::get(opCtx)->catalogClient()->insertConfigDocument(
            opCtx,
            ChunkType::ConfigNS,
            chunk.toConfigBSON(),
            ShardingCatalogClient::kMajorityWriteConcern));
    }

    return version;
}

void ShardingCatalogManager::shardCollection(OperationContext* opCtx,
                                             const string& ns,
                                             const boost::optional<UUID> uuid,
                                             const ShardKeyPattern& fieldsAndOrder,
                                             const BSONObj& defaultCollation,
                                             bool unique,
                                             const vector<BSONObj>& initPoints,
                                             const bool distributeInitialChunks,
                                             const ShardId& dbPrimaryShardId) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    const auto primaryShard = uassertStatusOK(shardRegistry->getShard(opCtx, dbPrimaryShardId));

    // Fail if there are partially written chunks from a previous failed shardCollection.
    checkForExistingChunks(opCtx, ns);

    // Record start in changelog
    {
        BSONObjBuilder collectionDetail;
        collectionDetail.append("shardKey", fieldsAndOrder.toBSON());
        collectionDetail.append("collection", ns);
        if (uuid) {
            uuid->appendToBuilder(&collectionDetail, "uuid");
        }
        collectionDetail.append("primary", primaryShard->toString());
        collectionDetail.append("numChunks", static_cast<int>(initPoints.size() + 1));
        catalogClient
            ->logChange(opCtx,
                        "shardCollection.start",
                        ns,
                        collectionDetail.obj(),
                        ShardingCatalogClient::kMajorityWriteConcern)
            .transitional_ignore();
    }

    const NamespaceString nss(ns);

    // Construct the collection default collator.
    std::unique_ptr<CollatorInterface> defaultCollator;
    if (!defaultCollation.isEmpty()) {
        defaultCollator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(defaultCollation));
    }

    const auto& collVersion = _createFirstChunks(
        opCtx, nss, fieldsAndOrder, dbPrimaryShardId, initPoints, distributeInitialChunks);

    {
        CollectionType coll;
        coll.setNs(nss);
        if (uuid) {
            coll.setUUID(*uuid);
        }
        coll.setEpoch(collVersion.epoch());

        // TODO(schwerin): The following isn't really a date, but is stored as one in-memory and in
        // config.collections, as a historical oddity.
        coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(collVersion.toLong()));
        coll.setKeyPattern(fieldsAndOrder.toBSON());
        coll.setDefaultCollation(defaultCollator ? defaultCollator->getSpec().toBSON() : BSONObj());
        coll.setUnique(unique);

        uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
            opCtx, ns, coll, true /*upsert*/));
    }

    auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, dbPrimaryShardId));
    invariant(!shard->isConfig());

    // Tell the primary mongod to refresh its data
    // TODO:  Think the real fix here is for mongos to just
    //        assume that all collections are sharded, when we get there
    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        shardRegistry->getConfigServerConnectionString(),
        dbPrimaryShardId,
        primaryShard->getConnString(),
        NamespaceString(ns),
        collVersion,
        true);

    auto ssvResponse =
        shard->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                "admin",
                                                ssv.toBSON(),
                                                Shard::RetryPolicy::kIdempotent);
    auto status = ssvResponse.isOK() ? std::move(ssvResponse.getValue().commandStatus)
                                     : std::move(ssvResponse.getStatus());
    if (!status.isOK()) {
        warning() << "could not update initial version of " << ns << " on shard primary "
                  << dbPrimaryShardId << causedBy(redact(status));
    }

    catalogClient
        ->logChange(opCtx,
                    "shardCollection.end",
                    ns,
                    BSON("version" << collVersion.toString()),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .transitional_ignore();
}

void ShardingCatalogManager::generateUUIDsForExistingShardedCollections(OperationContext* opCtx) {
    // Retrieve all collections in config.collections that do not have a UUID. Some collections
    // may already have a UUID if an earlier upgrade attempt failed after making some progress.
    auto shardedColls =
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                NamespaceString(CollectionType::ConfigNS),
                BSON(CollectionType::uuid.name() << BSON("$exists" << false) << "dropped" << false),
                BSONObj(),   // sort
                boost::none  // limit
                ))
            .docs;

    if (shardedColls.empty()) {
        LOG(0) << "all sharded collections already have UUIDs";

        // We did a local read of the collections collection above and found that all sharded
        // collections already have UUIDs. However, the data may not be majority committed (a
        // previous setFCV attempt may have failed with a write concern error). Since the current
        // Client doesn't know the opTime of the last write to the collections collection, make it
        // wait for the last opTime in the system when we wait for writeConcern.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return;
    }

    // Generate and persist a new UUID for each collection that did not have a UUID.
    LOG(0) << "generating UUIDs for " << shardedColls.size()
           << " sharded collections that do not yet have a UUID";
    for (auto& coll : shardedColls) {
        auto collType = uassertStatusOK(CollectionType::fromBSON(coll));
        invariant(!collType.getUUID());

        auto uuid = CollectionUUID::gen();
        collType.setUUID(uuid);

        uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
            opCtx, collType.getNs().ns(), collType, false /* upsert */));
        LOG(2) << "updated entry in config.collections for sharded collection " << collType.getNs()
               << " with generated UUID " << uuid;
    }
}

}  // namespace mongo

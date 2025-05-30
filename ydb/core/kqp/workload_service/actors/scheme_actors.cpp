#include "actors.h"

#include <ydb/core/base/path.h>
#include <ydb/core/base/tablet_pipe.h>

#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/protos/workload_manager_config.pb.h>

#include <ydb/core/kqp/common/simple/services.h>
#include <ydb/core/kqp/workload_service/common/events.h>
#include <ydb/core/kqp/workload_service/common/helpers.h>

#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>

#include <ydb/library/table_creator/table_creator.h>


namespace NKikimr::NKqp::NWorkload {

namespace {

using namespace NActors;


class TPoolResolverActor : public TActorBootstrapped<TPoolResolverActor> {
public:
    TPoolResolverActor(TEvPlaceRequestIntoPool::TPtr event, bool defaultPoolExists, const NKikimrConfig::TWorkloadManagerConfig& workloadManagerConfig)
        : Event(std::move(event))
        , WorkloadManagerConfig(workloadManagerConfig)
    {
        if (!Event->Get()->PoolId) {
            Event->Get()->PoolId = NResourcePool::DEFAULT_POOL_ID;
        }
        CanCreatePool = Event->Get()->PoolId == NResourcePool::DEFAULT_POOL_ID && !defaultPoolExists;
    }

    void Bootstrap() {
        Become(&TPoolResolverActor::StateFunc);
        StartPoolFetchRequest();
    }

    void StartPoolFetchRequest() const {
        LOG_D("Start pool fetching");
        Register(CreatePoolFetcherActor(SelfId(), Event->Get()->DatabaseId, Event->Get()->PoolId, Event->Get()->UserToken, WorkloadManagerConfig));
    }

    void Handle(TEvPrivate::TEvFetchPoolResponse::TPtr& ev) {
        if (ev->Get()->Status == Ydb::StatusIds::NOT_FOUND && CanCreatePool) {
            CanCreatePool = false;
            StartCreateDefaultPoolRequest();
            return;
        }

        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS) {
            LOG_E("Failed to fetch pool info " << ev->Get()->Status << ", issues: " << ev->Get()->Issues.ToOneLineString());
            NYql::TIssues issues = GroupIssues(ev->Get()->Issues, TStringBuilder() << "Failed to resolve pool id " << Event->Get()->PoolId);
            Reply(ev->Get()->Status, std::move(issues));
            return;
        }

        Reply(ev->Get()->PoolConfig, ev->Get()->PathId);
    }

    void StartCreateDefaultPoolRequest() const {
        LOG_I("Start default pool creation");

        NACLib::TDiffACL diffAcl;
        for (const TString& usedSid : AppData()->AdministrationAllowedSIDs) {
            diffAcl.AddAccess(NACLib::EAccessType::Allow, NACLib::EAccessRights::GenericFull, usedSid);
        }

        auto useAccess = NACLib::EAccessRights::SelectRow | NACLib::EAccessRights::DescribeSchema;
        for (const auto& userSID : AppData()->DefaultUserSIDs) {
            diffAcl.AddAccess(NACLib::EAccessType::Allow, useAccess, userSID);
        }
        diffAcl.AddAccess(NACLib::EAccessType::Allow, useAccess, AppData()->AllAuthenticatedUsers);
        diffAcl.AddAccess(NACLib::EAccessType::Allow, useAccess, BUILTIN_ACL_ROOT);

        auto token = MakeIntrusive<NACLib::TUserToken>(BUILTIN_ACL_METADATA, TVector<NACLib::TSID>{});
        Register(CreatePoolCreatorActor(SelfId(), Event->Get()->DatabaseId, Event->Get()->PoolId, NResourcePool::TPoolSettings(), token, diffAcl));
    }

    void Handle(TEvPrivate::TEvCreatePoolResponse::TPtr& ev) {
        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS) {
            LOG_E("Failed to create default pool " << ev->Get()->Status << ", issues: " << ev->Get()->Issues.ToOneLineString());
            Reply(ev->Get()->Status, GroupIssues(ev->Get()->Issues, TStringBuilder() << "Failed to create default pool in database " << Event->Get()->DatabaseId));
            return;
        }

        LOG_D("Successfully created default pool");
        DefaultPoolCreated = true;
        StartPoolFetchRequest();
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvFetchPoolResponse, Handle);
        hFunc(TEvPrivate::TEvCreatePoolResponse, Handle);
    )

private:
    TString LogPrefix() const {
        return TStringBuilder() << "[TPoolResolverActor] ActorId: " << SelfId() << ", DatabaseId: " << Event->Get()->DatabaseId << ", PoolId: " << Event->Get()->PoolId << ", SessionId: " << Event->Get()->SessionId << ", ";
    }

    void Reply(NResourcePool::TPoolSettings poolConfig, TPathId pathId) {
        LOG_D("Pool info successfully resolved");

        Send(MakeKqpWorkloadServiceId(SelfId().NodeId()), new TEvPrivate::TEvResolvePoolResponse(Ydb::StatusIds::SUCCESS, poolConfig, pathId, DefaultPoolCreated, std::move(Event)));
        PassAway();
    }

    void Reply(Ydb::StatusIds::StatusCode status, NYql::TIssues issues) {
        LOG_W("Failed to resolve pool, " << status << ", issues: " << issues.ToOneLineString());

        Send(MakeKqpWorkloadServiceId(SelfId().NodeId()), new TEvPrivate::TEvResolvePoolResponse(status, {}, {}, DefaultPoolCreated, std::move(Event), std::move(issues)));
        PassAway();
    }

private:
    TEvPlaceRequestIntoPool::TPtr Event;
    bool CanCreatePool = false;
    bool DefaultPoolCreated = false;
    NKikimrConfig::TWorkloadManagerConfig WorkloadManagerConfig;
};


class TPoolFetcherActor : public TSchemeActorBase<TPoolFetcherActor> {
public:
    TPoolFetcherActor(const TActorId& replyActorId, const TString& databaseId, const TString& poolId, TIntrusiveConstPtr<NACLib::TUserToken> userToken)
        : ReplyActorId(replyActorId)
        , DatabaseId(databaseId)
        , PoolId(poolId)
        , UserToken(userToken)
    {}

    void DoBootstrap() {
        Become(&TPoolFetcherActor::StateFunc);
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        const auto& results = ev->Get()->Request->ResultSet;
        if (results.size() != 1) {
            Reply(Ydb::StatusIds::INTERNAL_ERROR, "Unexpected scheme cache response");
            return;
        }

        const auto& result = results[0];
        switch (result.Status) {
            case EStatus::Unknown:
            case EStatus::PathNotTable:
            case EStatus::PathNotPath:
            case EStatus::RedirectLookupError:
                Reply(Ydb::StatusIds::BAD_REQUEST, TStringBuilder() << "Invalid resource pool id " << PoolId);
                return;
            case EStatus::AccessDenied:
                Reply(Ydb::StatusIds::UNAUTHORIZED, TStringBuilder() << "You don't have access permissions for resource pool " << PoolId);
                return;
            case EStatus::RootUnknown:
            case EStatus::PathErrorUnknown:
                Reply(Ydb::StatusIds::NOT_FOUND, TStringBuilder() << "Resource pool " << PoolId << " not found or you don't have access permissions");
                return;
            case EStatus::LookupError:
            case EStatus::TableCreationNotComplete:
                if (!ScheduleRetry(TStringBuilder() << "Retry error " << result.Status)) {
                    Reply(Ydb::StatusIds::UNAVAILABLE, TStringBuilder() << "Retry limit exceeded on scheme error: " << result.Status);
                }
                return;
            case EStatus::Ok:
                Reply(result.ResourcePoolInfo);
                return;
        }
    }

    STFUNC(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            default:
                StateFuncBase(ev);
        }
    }

protected:
    void StartRequest() override {
        LOG_D("Start pool fetching");
        auto event = NTableCreator::BuildSchemeCacheNavigateRequest(
            {{".metadata/workload_manager/pools", PoolId}},
            DatabaseIdToDatabase(DatabaseId),
            UserToken
        );
        event->ResultSet[0].Access |= NACLib::SelectRow;
        event->ResultSet[0].Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(event.Release()), IEventHandle::FlagTrackDelivery);
    }

    void OnFatalError(Ydb::StatusIds::StatusCode status, NYql::TIssue issue) override {
        Reply(status, {std::move(issue)});
    }

    TString LogPrefix() const override {
        return TStringBuilder() << "[TPoolFetcherActor] ActorId: " << SelfId() << ", DatabaseId: " << DatabaseId << ", PoolId: " << PoolId << ", ";
    }

private:
    void Reply(const TIntrusiveConstPtr<NSchemeCache::TSchemeCacheNavigate::TResourcePoolInfo>& poolInfo) {
        if (!poolInfo) {
            Reply(Ydb::StatusIds::INTERNAL_ERROR, "Unexpected scheme cache response");
            return;
        }

        PathId = poolInfo->Description.GetPathId();
        ParsePoolSettings(poolInfo->Description, PoolConfig);

        Reply(Ydb::StatusIds::SUCCESS);
    }

    void Reply(Ydb::StatusIds::StatusCode status, const TString& message) {
        Reply(status, {NYql::TIssue(message)});
    }

    void Reply(Ydb::StatusIds::StatusCode status, NYql::TIssues issues = {}) {
        if (status == Ydb::StatusIds::SUCCESS) {
            LOG_D("Pool info successfully fetched");
        } else {
            LOG_W("Failed to fetch pool info, " << status << ", issues: " << issues.ToOneLineString());
        }

        Issues.AddIssues(std::move(issues));
        Send(ReplyActorId, new TEvPrivate::TEvFetchPoolResponse(status, DatabaseId, PoolId, PoolConfig, TPathId::FromProto(PathId), std::move(Issues)));
        PassAway();
    }

private:
    const TActorId ReplyActorId;
    const TString DatabaseId;
    const TString PoolId;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;

    NResourcePool::TPoolSettings PoolConfig;
    NKikimrProto::TPathID PathId;
};

class TStaticPoolFetcherActor : public NActors::TActorBootstrapped<TStaticPoolFetcherActor> {
public:
    TStaticPoolFetcherActor(const TActorId& replyActorId, const TString& databaseId, const TString& poolId, const NKikimrConfig::TWorkloadManagerConfig& workloadManagerConfig)
        : ReplyActorId(replyActorId)
        , DatabaseId(databaseId)
        , PoolId(poolId)
        , WorkloadManagerConfig(workloadManagerConfig)
    {}

    void Bootstrap() {
        if (PoolId == NResourcePool::DEFAULT_POOL_ID) {
            NResourcePool::TPoolSettings poolSettings;
            poolSettings.ResourceWeight = WorkloadManagerConfig.GetResourceWeight();
            poolSettings.ConcurrentQueryLimit = WorkloadManagerConfig.GetConcurrentQueryLimit();
            poolSettings.QueueSize = WorkloadManagerConfig.GetQueueSize();
            poolSettings.QueryCpuLimitPercentPerNode = WorkloadManagerConfig.GetQueryCpuLimitPercentPerNode();
            poolSettings.QueryMemoryLimitPercentPerNode = WorkloadManagerConfig.GetQueryMemoryLimitPercentPerNode();
            poolSettings.TotalCpuLimitPercentPerNode = WorkloadManagerConfig.GetTotalCpuLimitPercentPerNode();
            poolSettings.DatabaseLoadCpuThreshold = WorkloadManagerConfig.GetDatabaseLoadCpuThreshold();
            Reply(poolSettings);
            return;
        }
        ReplyError(Ydb::StatusIds::BAD_REQUEST, "Unknown static pool " + PoolId + ", please check the database configuration");
    }

    STRICT_STFUNC(StateFunc,
        cFunc(TEvents::TEvBootstrap::EventType, Bootstrap);
    )

    TString LogPrefix() const {
        return TStringBuilder() << "[TStaticPoolFetcherActor] ActorId: " << SelfId() << ", DatabaseId: " << DatabaseId << ", PoolId: " << PoolId << ", ";
    }

private:
    void ReplyError(Ydb::StatusIds::StatusCode status, const TString& message) {
        ReplyError(status, {NYql::TIssue(message)});
    }

    void ReplyError(Ydb::StatusIds::StatusCode status, NYql::TIssues issues) {
        LOG_W("Failed to fetch static pool info, " << status << ", issues: " << issues.ToOneLineString());
        Send(ReplyActorId, new TEvPrivate::TEvFetchPoolResponse(status, DatabaseId, PoolId, {}, {}, std::move(issues)));
        PassAway();
    }

    void Reply(const NResourcePool::TPoolSettings& poolConfig) {
        LOG_D("Static Pool info successfully fetched");
        Send(ReplyActorId, new TEvPrivate::TEvFetchPoolResponse(Ydb::StatusIds::SUCCESS, DatabaseId, PoolId, poolConfig, {}, {}));
        PassAway();
    }

private:
    const TActorId ReplyActorId;
    const TString DatabaseId;
    const TString PoolId;
    const NKikimrConfig::TWorkloadManagerConfig WorkloadManagerConfig;
};


class TPoolCreatorActor : public TSchemeActorBase<TPoolCreatorActor> {
    using TBase = TSchemeActorBase<TPoolCreatorActor>;

public:
    TPoolCreatorActor(const TActorId& replyActorId, const TString& databaseId, const TString& poolId, const NResourcePool::TPoolSettings& poolConfig, TIntrusiveConstPtr<NACLib::TUserToken> userToken, NACLibProto::TDiffACL diffAcl)
        : ReplyActorId(replyActorId)
        , DatabaseId(databaseId)
        , PoolId(poolId)
        , UserToken(userToken)
        , DiffAcl(diffAcl)
        , PoolConfig(poolConfig)
    {}

    void DoBootstrap() {
        Become(&TPoolCreatorActor::StateFunc);
    }

    void Handle(TEvTxUserProxy::TEvProposeTransactionStatus::TPtr& ev) {
        const auto& response = ev->Get()->Record;
        const auto ssStatus = response.GetSchemeShardStatus();
        const auto status = ev->Get()->Status();
        switch (status) {
            case NTxProxy::TResultStatus::ExecComplete:
            case NTxProxy::TResultStatus::ExecAlready:
                if (ssStatus == NKikimrScheme::EStatus::StatusSuccess || ssStatus == NKikimrScheme::EStatus::StatusAlreadyExists) {
                    Reply(Ydb::StatusIds::SUCCESS);
                } else {
                    Reply(Ydb::StatusIds::SCHEME_ERROR, ExtractIssues(response, TStringBuilder() << "Invalid creation status: " << static_cast<NKikimrScheme::EStatus>(ssStatus)));
                }
                return;
            case NTxProxy::TResultStatus::ExecError:
                if (ssStatus == NKikimrScheme::EStatus::StatusMultipleModifications) {
                    SubscribeOnTransactionOrRetry(status, response);
                } else {
                    Reply(Ydb::StatusIds::SCHEME_ERROR, ExtractIssues(response, TStringBuilder() << "Execution error: " << static_cast<NKikimrScheme::EStatus>(ssStatus)));
                }
                return;
            case NTxProxy::TResultStatus::ExecInProgress:
                SubscribeOnTransactionOrRetry(status, response);
                return;
            case NTxProxy::TResultStatus::ProxyShardNotAvailable:
                ScheduleRetry(response, "Retry shard unavailable error");
                return;
            default:
                Reply(Ydb::StatusIds::SCHEME_ERROR, ExtractIssues(response, TStringBuilder() << "Failed to create resource pool: " << static_cast<NKikimrScheme::EStatus>(ssStatus)));
                return;
        }
    }

    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev) {
        if (ev->Get()->Status == NKikimrProto::OK) {
            LOG_T("Tablet to pipe successfully connected");
            return;
        }

        ClosePipeClient();
        ScheduleRetry(TStringBuilder() << "Tablet to pipe not connected: " << NKikimrProto::EReplyStatus_Name(ev->Get()->Status));
    }

    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev) {
        const TActorId clientId = ev->Get()->ClientId;
        if (!ClosedSchemePipeActors.contains(clientId)) {
            ClosePipeClient();
            ScheduleRetry("Tablet to pipe destroyed");
        }
    }

    void Handle(NSchemeShard::TEvSchemeShard::TEvNotifyTxCompletionResult::TPtr& ev) {
        ScheduleRetry(TStringBuilder() << "Transaction " << ev->Get()->Record.GetTxId() << " completed, doublechecking");
    }

    STFUNC(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvTxUserProxy::TEvProposeTransactionStatus, Handle)
            hFunc(TEvTabletPipe::TEvClientConnected, Handle)
            hFunc(TEvTabletPipe::TEvClientDestroyed, Handle)
            hFunc(NSchemeShard::TEvSchemeShard::TEvNotifyTxCompletionResult, Handle)
            IgnoreFunc(NSchemeShard::TEvSchemeShard::TEvNotifyTxCompletionRegistered)

            default:
                StateFuncBase(ev);
        }
    }

protected:
    void StartRequest() override {
        LOG_D("Start pool creating");
        const auto& database = DatabaseIdToDatabase(DatabaseId);

        auto event = std::make_unique<TEvTxUserProxy::TEvProposeTransaction>();

        auto& schemeTx = *event->Record.MutableTransaction()->MutableModifyScheme();
        schemeTx.SetWorkingDir(JoinPath({database, ".metadata/workload_manager/pools"}));
        schemeTx.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateResourcePool);
        schemeTx.SetInternal(true);

        BuildCreatePoolRequest(*schemeTx.MutableCreateResourcePool());
        BuildModifyAclRequest(*schemeTx.MutableModifyACL());

        event->Record.SetDatabaseName(database);
        if (UserToken) {
            event->Record.SetUserToken(UserToken->SerializeAsString());
        }

        Send(MakeTxProxyID(), std::move(event));
    }

    void OnFatalError(Ydb::StatusIds::StatusCode status, NYql::TIssue issue) override {
        Reply(status, {std::move(issue)});
    }

    TString LogPrefix() const override {
        return TStringBuilder() << "[TPoolCreatorActor] ActorId: " << SelfId() << ", DatabaseId: " << DatabaseId << ", PoolId: " << PoolId << ", ";
    }

private:
    void SubscribeOnTransactionOrRetry(NTxProxy::TResultStatus::EStatus status, const NKikimrTxUserProxy::TEvProposeTransactionStatus& response) {
        const ui64 txId = status == NTxProxy::TResultStatus::ExecInProgress ? response.GetTxId() : response.GetPathCreateTxId();
        if (txId == 0) {
            ScheduleRetry(response, "Unable to subscribe to concurrent transaction", true);
            return;
        }

        SchemePipeActorId = Register(NTabletPipe::CreateClient(SelfId(), response.GetSchemeShardTabletId()));

        auto request = MakeHolder<NSchemeShard::TEvSchemeShard::TEvNotifyTxCompletion>();
        request->Record.SetTxId(txId);
        NTabletPipe::SendData(SelfId(), SchemePipeActorId, std::move(request));
        LOG_D("Subscribe on create pool tx: " << txId);
    }

    void ClosePipeClient() {
        if (SchemePipeActorId) {
            ClosedSchemePipeActors.insert(SchemePipeActorId);
            NTabletPipe::CloseClient(SelfId(), SchemePipeActorId);
            SchemePipeActorId = {};
        }
    }

    void ScheduleRetry(const NKikimrTxUserProxy::TEvProposeTransactionStatus& response, const TString& message, bool longDelay = false) {
        ClosePipeClient();

        auto ssStatus = static_cast<NKikimrScheme::EStatus>(response.GetSchemeShardStatus());
        if (!TBase::ScheduleRetry(ExtractIssues(response, TStringBuilder() << message << ", status: " << ssStatus), longDelay)) {
            Reply(Ydb::StatusIds::UNAVAILABLE, ExtractIssues(response, TStringBuilder() << "Retry limit exceeded on status: " << ssStatus));
        }
    }

    void ScheduleRetry(const TString& message, bool longDelay = false) {
        ClosePipeClient();
        if (!TBase::ScheduleRetry(message, longDelay)) {
            Reply(Ydb::StatusIds::UNAVAILABLE, TStringBuilder() << "Retry limit exceeded on error: " << message);
        }
    }

    void BuildCreatePoolRequest(NKikimrSchemeOp::TResourcePoolDescription& poolDescription) {
        poolDescription.SetName(PoolId);
        for (auto& [property, value] : PoolConfig.GetPropertiesMap()) {
            poolDescription.MutableProperties()->MutableProperties()->insert({
                property,
                std::visit(NResourcePool::TPoolSettings::TExtractor{}, value)
            });
        }
    }

    void BuildModifyAclRequest(NKikimrSchemeOp::TModifyACL& modifyACL) const {
        modifyACL.SetName(PoolId);
        modifyACL.SetDiffACL(DiffAcl.SerializeAsString());
        if (UserToken) {
            modifyACL.SetNewOwner(UserToken->GetUserSID());
        }
    }

    void Reply(Ydb::StatusIds::StatusCode status, const TString& message) {
        Reply(status, {NYql::TIssue(message)});
    }

    void Reply(Ydb::StatusIds::StatusCode status, NYql::TIssues issues = {}) {
        if (status == Ydb::StatusIds::SUCCESS) {
            LOG_D("Pool successfully created");
        } else {
            LOG_W("Failed to create pool, " << status << ", issues: " << issues.ToOneLineString());
        }

        ClosePipeClient();

        Issues.AddIssues(std::move(issues));
        Send(ReplyActorId, new TEvPrivate::TEvCreatePoolResponse(status, std::move(Issues)));
        PassAway();
    }

    static NYql::TIssues ExtractIssues(const NKikimrTxUserProxy::TEvProposeTransactionStatus& response, const TString& message) {
        NYql::TIssues issues;
        NYql::IssuesFromMessage(response.GetIssues(), issues);
        return GroupIssues(issues, message);
    }

private:
    const TActorId ReplyActorId;
    const TString DatabaseId;
    const TString PoolId;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const NACLibProto::TDiffACL DiffAcl;
    NResourcePool::TPoolSettings PoolConfig;

    std::unordered_set<TActorId> ClosedSchemePipeActors;
    TActorId SchemePipeActorId;
};


class TDatabaseFetcherActor : public TSchemeActorBase<TDatabaseFetcherActor> {
public:
    TDatabaseFetcherActor(const TActorId& replyActorId, const TString& database, TIntrusiveConstPtr<NACLib::TUserToken> userToken, NACLib::EAccessRights checkAccess)
        : ReplyActorId(replyActorId)
        , Database(database)
        , UserToken(userToken)
        , CheckAccess(checkAccess)
    {}

    void DoBootstrap() {
        Become(&TDatabaseFetcherActor::StateFunc);
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        const auto& results = ev->Get()->Request->ResultSet;
        if (results.size() != 1) {
            Reply(Ydb::StatusIds::INTERNAL_ERROR, "Unexpected scheme cache response");
            return;
        }

        const auto& result = results[0];
        switch (result.Status) {
            case EStatus::Unknown:
            case EStatus::PathNotTable:
            case EStatus::PathNotPath:
            case EStatus::RedirectLookupError:
            case EStatus::RootUnknown:
            case EStatus::PathErrorUnknown:
                Reply(Ydb::StatusIds::NOT_FOUND, TStringBuilder() << "Database " << Database << " not found or you don't have access permissions");
                return;
            case EStatus::AccessDenied:
                Reply(Ydb::StatusIds::UNAUTHORIZED, TStringBuilder() << "You don't have access permissions for database " << Database);
                return;
            case EStatus::LookupError:
            case EStatus::TableCreationNotComplete:
                if (!ScheduleRetry(TStringBuilder() << "Retry error " << result.Status)) {
                    Reply(Ydb::StatusIds::UNAVAILABLE, TStringBuilder() << "Retry limit exceeded on scheme error: " << result.Status);
                }
                return;
            case EStatus::Ok:
                if (!IsSubDomainPath(result)) {
                    Reply(Ydb::StatusIds::UNSUPPORTED, TStringBuilder() << "Invalid database path " << Database << ", please check the correctness of the path");
                    return;
                }
                if (result.DomainInfo) {
                    Serverless = result.DomainInfo->IsServerless();
                    PathId = result.DomainInfo->DomainKey;
                }
                Reply(Ydb::StatusIds::SUCCESS);
                return;
        }
    }

    STFUNC(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            default:
                StateFuncBase(ev);
        }
    }

protected:
    void StartRequest() override {
        LOG_D("Start database fetching");
        auto event = NTableCreator::BuildSchemeCacheNavigateRequest(
            {{}},
            Database ? Database : AppData()->TenantName,
            UserToken
        );
        event->ResultSet[0].Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        event->ResultSet[0].Access |= CheckAccess;
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(event.Release()), IEventHandle::FlagTrackDelivery);
    }

    void OnFatalError(Ydb::StatusIds::StatusCode status, NYql::TIssue issue) override {
        Reply(status, {std::move(issue)});
    }

    TString LogPrefix() const override {
        return TStringBuilder() << "[TDatabaseFetcherActor] ActorId: " << SelfId() << ", Database: " << Database << ", ";
    }

private:
    void Reply(Ydb::StatusIds::StatusCode status, const TString& message) {
        Reply(status, {NYql::TIssue(message)});
    }

    void Reply(Ydb::StatusIds::StatusCode status, NYql::TIssues issues = {}) {
        if (status == Ydb::StatusIds::SUCCESS) {
            LOG_D("Database info successfully fetched, serverless: " << Serverless);
        } else {
            LOG_W("Failed to fetch database info, " << status << ", issues: " << issues.ToOneLineString());
        }

        Issues.AddIssues(std::move(issues));
        Send(ReplyActorId, new TEvFetchDatabaseResponse(status, Database, CreateDatabaseId(Database, Serverless, PathId), Serverless, PathId, std::move(Issues)));
        PassAway();
    }

    static bool IsSubDomainPath(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry) {
        switch (entry.Kind) {
            case NSchemeCache::TSchemeCacheNavigate::EKind::KindSubdomain:
            case NSchemeCache::TSchemeCacheNavigate::EKind::KindExtSubdomain:
                return true;
            case NSchemeCache::TSchemeCacheNavigate::EKind::KindPath:
                return entry.Self->Info.GetPathId() == NSchemeShard::RootPathId;
            default:
                return false;
        }
    }

private:
    const TActorId ReplyActorId;
    const TString Database;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const NACLib::EAccessRights CheckAccess;

    bool Serverless = false;
    TPathId PathId;
};

}  // anonymous namespace

IActor* CreatePoolResolverActor(TEvPlaceRequestIntoPool::TPtr event, bool defaultPoolExists, const NKikimrConfig::TWorkloadManagerConfig& workloadManagerConfig) {
    return new TPoolResolverActor(std::move(event), defaultPoolExists, workloadManagerConfig);
}

IActor* CreatePoolFetcherActor(const TActorId& replyActorId, const TString& databaseId, const TString& poolId, TIntrusiveConstPtr<NACLib::TUserToken> userToken, const NKikimrConfig::TWorkloadManagerConfig& workloadManagerConfig) {
    if (workloadManagerConfig.GetEnabled()) {
        return new TStaticPoolFetcherActor(replyActorId, databaseId, poolId, workloadManagerConfig);
    }
    return new TPoolFetcherActor(replyActorId, databaseId, poolId, userToken);
}

IActor* CreatePoolCreatorActor(const TActorId& replyActorId, const TString& databaseId, const TString& poolId, const NResourcePool::TPoolSettings& poolConfig, TIntrusiveConstPtr<NACLib::TUserToken> userToken, NACLibProto::TDiffACL diffAcl) {
    return new TPoolCreatorActor(replyActorId, databaseId, poolId, poolConfig, userToken, diffAcl);
}

IActor* CreateDatabaseFetcherActor(const TActorId& replyActorId, const TString& database, TIntrusiveConstPtr<NACLib::TUserToken> userToken, NACLib::EAccessRights checkAccess) {
    return new TDatabaseFetcherActor(replyActorId, database, userToken, checkAccess);
}

}  // NKikimr::NKqp::NWorkload

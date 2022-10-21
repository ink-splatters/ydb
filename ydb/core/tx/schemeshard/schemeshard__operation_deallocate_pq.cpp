#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TDeallocatePQ: public ISubOperationBase {
    const TOperationId OperationId;
    const TTxTransaction Transaction;

public:
    TDeallocatePQ(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TDeallocatePQ(TOperationId id)
        : OperationId(id)
    {
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = Transaction.GetDeallocatePersQueueGroup().GetName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDeallocatePQ Propose"
                         << ", path: " << parentPathStr << "/" << name
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId));

        TPath path = TPath::Resolve(parentPathStr, context.SS).Dive(name);

        {
            TPath::TChecker checks = path.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .NotUnderOperation()
                .IsPQGroup()
                .NotUnderOperation();

            if (!checks) {
                TString explain = TStringBuilder() << "path table fail checks"
                                                   << ", path: " << path.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (path.IsResolved() && path.Base()->IsPQGroup() && path.Base()->PlannedToDrop()) {
                    result->SetPathDropTxId(ui64(path.Base()->DropTxId));
                    result->SetPathId(path.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        TPath parent = path.Parent();
        {
            TPath::TChecker checks = parent.Check();
            checks
                .NotEmpty()
                .IsResolved()
                .NotDeleted();

            if (checks) {
                if (parent.Base()->IsCdcStream()) {
                    checks
                        .IsCdcStream()
                        .IsInsideCdcStreamPath()
                        .IsUnderDeleting(TEvSchemeShard::EStatus::StatusNameConflict)
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else {
                    checks
                        .IsLikeDirectory()
                        .IsCommonSensePath()
                        .NotUnderDeleting();
                }
            }

            if (!checks) {
                TString explain = TStringBuilder() << "parent path fail checks"
                                                   << ", path: " << parent.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        auto pathId = path.Base()->PathId;
        TPersQueueGroupInfo::TPtr pqGroup = context.SS->PersQueueGroups.at(pathId);
        Y_VERIFY(pqGroup);

        if (pqGroup->AlterData) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, "Deallocate over Create/Alter");
            return result;
        }

        NIceDb::TNiceDb db(context.GetDB());

        path.Base()->LastTxId = OperationId.GetTxId();
        TStepId fakeStep = TStepId(TAppData::TimeProvider->Now().MilliSeconds());
        path->SetDropped(fakeStep, OperationId.GetTxId());
        context.SS->PersistDropStep(db, pathId, fakeStep, OperationId);

        context.SS->TabletCounters->Simple()[COUNTER_PQ_GROUP_COUNT].Sub(1);

        auto tabletConfig = pqGroup->TabletConfig;
        NKikimrPQ::TPQTabletConfig config;
        Y_VERIFY(!tabletConfig.empty());
        bool parseOk = ParseFromStringNoSizeLimit(config, tabletConfig);
        Y_VERIFY(parseOk);

        ui64 throughput = ((ui64)pqGroup->TotalPartitionCount) * config.GetPartitionConfig().GetWriteSpeedInBytesPerSecond();

        const ui64 storage = [&config, &throughput]() {
            if (config.GetPartitionConfig().HasStorageLimitBytes()) {
                return config.GetPartitionConfig().GetStorageLimitBytes();
            } else {
                return throughput * config.GetPartitionConfig().GetLifetimeSeconds();
            }
        }();
        
        auto domainInfo = context.SS->ResolveDomainInfo(pathId);
        domainInfo->DecPathsInside();
        domainInfo->DecPQPartitionsInside(pqGroup->TotalPartitionCount);
        domainInfo->DecPQReservedStorage(storage);

        context.SS->TabletCounters->Simple()[COUNTER_STREAM_RESERVED_THROUGHPUT].Sub(throughput);
        context.SS->TabletCounters->Simple()[COUNTER_STREAM_RESERVED_STORAGE].Sub(storage);

        context.SS->TabletCounters->Simple()[COUNTER_STREAM_SHARDS_COUNT].Sub(pqGroup->TotalPartitionCount);

        parent->DecAliveChildren();

        if (!AppData()->DisableSchemeShardCleanupOnDropForTest) {
            context.SS->PersistRemovePersQueueGroup(db, pathId);
        }

        context.SS->TabletCounters->Simple()[COUNTER_USER_ATTRIBUTES_COUNT].Sub(path->UserAttrs->Size());
        context.SS->PersistUserAttributes(db, path->PathId, path->UserAttrs, nullptr);

        auto parentDir = path.Parent();
        ++parentDir.Base()->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentDir.Base());
        context.SS->ClearDescribePathCaches(parentDir.Base());
        context.SS->ClearDescribePathCaches(path.Base());

        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parentDir.Base()->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, path.Base()->PathId);
        }

        context.OnComplete.DoneOperation(OperationId);
        return result;
    }

    void ProgressState(TOperationContext&) override {
        Y_FAIL("no progress state for modify acl");
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TDeallocatePQ");
    }

    void AbortUnsafe(TTxId, TOperationContext&) override {
        Y_FAIL("no AbortUnsafe for TDeallocatePQ");
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateDeallocatePQ(TOperationId id, const TTxTransaction& tx) {
    return new TDeallocatePQ(id, tx);
}

ISubOperationBase::TPtr CreateDeallocatePQ(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state == TTxState::Invalid);
    return new TDeallocatePQ(id);
}

}
}
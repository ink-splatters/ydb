#pragma once
#include "defs.h"

#include <ydb/core/base/row_version.h>
#include <ydb/library/mkql_proto/protos/minikql.pb.h>
#include <ydb/core/protos/tx_proxy.pb.h>

namespace NKikimr {
namespace NTxProxy {

    enum class EReadTableFormat {
        OldResultSet,
        YdbResultSet,
    };

    struct TReadTableSettings {
        TActorId Owner;
        ui64 Cookie = 0;
        ui64 ProxyFlags = 0;
        TString DatabaseName;
        TString TablePath;
        TVector<TString> Columns;
        NKikimrTxUserProxy::TKeyRange KeyRange;
        ui64 MaxRows = Max<ui64>();
        TRowVersion ReadVersion = TRowVersion::Max();
        TString UserToken;
        EReadTableFormat DataFormat = EReadTableFormat::YdbResultSet;
        bool Ordered = false;
        bool RequireResultSet = false;
    };

    IActor* CreateReadTableSnapshotWorker(const TReadTableSettings& settings);

} // namespace NTxProxy
} // namespace NKikimr

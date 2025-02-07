#include "coordinator_hooks.h"
#include <util/system/yassert.h>
#include <atomic>

namespace NKikimr::NFlatTxCoordinator {

    namespace {
        static std::atomic<ICoordinatorHooks*> CoordinatorHooks{ nullptr };
    }

    bool ICoordinatorHooks::PersistConfig(ui64 tabletId, const NKikimrSubDomains::TProcessingParams& config) {
        Y_UNUSED(tabletId);
        Y_UNUSED(config);
        return true;
    }

    ICoordinatorHooks* ICoordinatorHooks::Get() {
        return CoordinatorHooks.load(std::memory_order_acquire);
    }

    void ICoordinatorHooks::Set(ICoordinatorHooks* hooks) {
        CoordinatorHooks.store(hooks, std::memory_order_release);
    }

    TCoordinatorHooksGuard::TCoordinatorHooksGuard(ICoordinatorHooks& hooks) {
        auto* current = ICoordinatorHooks::Get();
        Y_VERIFY(!current, "Unexpected attempt to install nested hooks");
        ICoordinatorHooks::Set(&hooks);
    }

    TCoordinatorHooksGuard::~TCoordinatorHooksGuard() {
        ICoordinatorHooks::Set(nullptr);
    }

} // namespace NKikimr::NFlatTxCoordinator

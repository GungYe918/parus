#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

extern "C" {
void* __parus_actor_new(uint64_t type_tag, uint64_t draft_size, uint64_t draft_align);
void* __parus_actor_clone(void* handle);
void  __parus_actor_release(void* handle);

void* __parus_actor_enter(void* handle, uint32_t mode);
void* __parus_actor_draft_ptr(void* ctx);
void  __parus_actor_commit(void* ctx);
void  __parus_actor_recast(void* ctx);
void  __parus_actor_leave(void* ctx);
uint64_t __parus_prt_debug_live_actors(void);
}

namespace {

    static bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    constexpr uint32_t kModeInit = 0;
    constexpr uint32_t kModeSub = 1;
    constexpr uint32_t kModePub = 2;

    static bool test_basic_actor_roundtrip_() {
        bool ok = true;
        const uint64_t live_before = __parus_prt_debug_live_actors();
        void* handle = __parus_actor_new(/*type_tag=*/7, sizeof(int32_t), alignof(int32_t));
        ok &= require_(handle != nullptr, "actor_new must return a handle");
        if (!ok) return false;

        void* init_ctx = __parus_actor_enter(handle, kModeInit);
        ok &= require_(init_ctx != nullptr, "actor_enter(init) must return a context");
        if (!ok) return false;

        auto* init_ptr = static_cast<int32_t*>(__parus_actor_draft_ptr(init_ctx));
        ok &= require_(init_ptr != nullptr, "actor_draft_ptr(init) must return draft storage");
        if (!ok) return false;
        *init_ptr = 41;
        __parus_actor_commit(init_ctx);
        __parus_actor_leave(init_ctx);

        void* clone = __parus_actor_clone(handle);
        ok &= require_(clone == handle, "actor_clone must return the same opaque handle");
        if (!ok) return false;

        void* sub_ctx = __parus_actor_enter(clone, kModeSub);
        ok &= require_(sub_ctx != nullptr, "actor_enter(sub) must return a context");
        if (!ok) return false;

        auto* read_ptr = static_cast<int32_t*>(__parus_actor_draft_ptr(sub_ctx));
        ok &= require_(read_ptr != nullptr, "actor_draft_ptr(sub) must return draft storage");
        ok &= require_(*read_ptr == 41, "cloned actor handle must observe committed draft state");
        __parus_actor_recast(sub_ctx);
        __parus_actor_leave(sub_ctx);

        __parus_actor_release(clone);
        __parus_actor_release(handle);
        ok &= require_(__parus_prt_debug_live_actors() == live_before, "basic roundtrip must reclaim actor storage after final release");
        return ok;
    }

    static bool test_concurrent_actor_mutation_() {
        bool ok = true;
        const uint64_t live_before = __parus_prt_debug_live_actors();
        void* handle = __parus_actor_new(/*type_tag=*/9, sizeof(int32_t), alignof(int32_t));
        ok &= require_(handle != nullptr, "actor_new for stress case must return a handle");
        if (!ok) return false;

        constexpr int kThreads = 4;
        constexpr int kItersPerThread = 500;
        std::vector<void*> handles{};
        handles.reserve(kThreads);
        handles.push_back(handle);
        for (int i = 1; i < kThreads; ++i) {
            handles.push_back(__parus_actor_clone(handle));
        }

        std::vector<std::thread> threads{};
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([h = handles[static_cast<size_t>(i)]]() {
                for (int iter = 0; iter < kItersPerThread; ++iter) {
                    void* ctx = __parus_actor_enter(h, kModePub);
                    auto* draft = static_cast<int32_t*>(__parus_actor_draft_ptr(ctx));
                    *draft = *draft + 1;
                    __parus_actor_commit(ctx);
                    __parus_actor_leave(ctx);
                }
            });
        }
        for (auto& th : threads) th.join();

        void* sub_ctx = __parus_actor_enter(handle, kModeSub);
        ok &= require_(sub_ctx != nullptr, "actor_enter(sub) after stress must return a context");
        if (!ok) return false;

        auto* draft = static_cast<int32_t*>(__parus_actor_draft_ptr(sub_ctx));
        ok &= require_(draft != nullptr, "actor_draft_ptr(sub) after stress must return draft storage");
        ok &= require_(*draft == kThreads * kItersPerThread, "concurrent actor mutation must serialize correctly");
        __parus_actor_recast(sub_ctx);
        __parus_actor_leave(sub_ctx);

        for (void* h : handles) {
            __parus_actor_release(h);
        }
        ok &= require_(__parus_prt_debug_live_actors() == live_before, "stress case must reclaim actor storage after all handles are released");
        return ok;
    }

} // namespace

int main() {
    std::cout << "[TEST] prt_basic_roundtrip (" << PARUS_PRT_VARIANT << ")\n";
    const bool ok1 = test_basic_actor_roundtrip_();
    std::cout << (ok1 ? "  -> PASS\n" : "  -> FAIL\n");

    std::cout << "[TEST] prt_concurrent_actor_mutation (" << PARUS_PRT_VARIANT << ")\n";
    const bool ok2 = test_concurrent_actor_mutation_();
    std::cout << (ok2 ? "  -> PASS\n" : "  -> FAIL\n");

    if (!ok1 || !ok2) {
        std::cout << "\nFAILED prt test suite\n";
        return 1;
    }
    std::cout << "\nALL PRT TESTS PASSED\n";
    return 0;
}

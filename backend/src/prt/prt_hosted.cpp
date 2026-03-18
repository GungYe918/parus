#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {

std::atomic<uint64_t> g_live_actors{0};

#if defined(_WIN32)
struct ActorLock {
    SRWLOCK lock = SRWLOCK_INIT;
};

bool actor_lock_init_(ActorLock*) {
    return true;
}

void actor_lock_destroy_(ActorLock*) {}

void actor_lock_shared_lock_(ActorLock* lock) {
    AcquireSRWLockShared(&lock->lock);
}

void actor_lock_shared_unlock_(ActorLock* lock) {
    ReleaseSRWLockShared(&lock->lock);
}

void actor_lock_exclusive_lock_(ActorLock* lock) {
    AcquireSRWLockExclusive(&lock->lock);
}

void actor_lock_exclusive_unlock_(ActorLock* lock) {
    ReleaseSRWLockExclusive(&lock->lock);
}
#else
struct ActorLock {
    pthread_rwlock_t lock{};
    bool initialized = false;
};

bool actor_lock_init_(ActorLock* lock) {
    if (lock == nullptr) return false;
    if (pthread_rwlock_init(&lock->lock, nullptr) != 0) return false;
    lock->initialized = true;
    return true;
}

void actor_lock_destroy_(ActorLock* lock) {
    if (lock == nullptr || !lock->initialized) return;
    (void)pthread_rwlock_destroy(&lock->lock);
    lock->initialized = false;
}

void actor_lock_shared_lock_(ActorLock* lock) {
    (void)pthread_rwlock_rdlock(&lock->lock);
}

void actor_lock_shared_unlock_(ActorLock* lock) {
    (void)pthread_rwlock_unlock(&lock->lock);
}

void actor_lock_exclusive_lock_(ActorLock* lock) {
    (void)pthread_rwlock_wrlock(&lock->lock);
}

void actor_lock_exclusive_unlock_(ActorLock* lock) {
    (void)pthread_rwlock_unlock(&lock->lock);
}
#endif

struct ActorObject {
    std::atomic<uint64_t> refcount{1};
    std::atomic<uint64_t> active_contexts{0};
    uint64_t type_tag = 0;
    uint64_t draft_size = 0;
    uint64_t draft_align = 0;
    size_t alloc_align = alignof(void*);
    void* draft_ptr = nullptr;
    ActorLock lock{};
};

struct ActorContext {
    ActorObject* actor = nullptr;
    void* draft_ptr = nullptr;
    uint32_t mode = 0;
    bool holds_read_lock = false;
    bool holds_write_lock = false;
};

constexpr size_t align_up_(size_t value, size_t align) {
    return (align <= 1) ? value : ((value + align - 1) / align) * align;
}

void* aligned_alloc_bytes_(size_t align, size_t size) {
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void* out = nullptr;
    if (posix_memalign(&out, align, size) != 0) return nullptr;
    return out;
#endif
}

void aligned_free_bytes_(void* ptr) {
    if (ptr == nullptr) return;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

ActorObject* actor_from_handle_(void* handle) {
    return static_cast<ActorObject*>(handle);
}

void destroy_actor_(ActorObject* actor) {
    if (actor == nullptr) return;
    actor_lock_destroy_(&actor->lock);
    actor->~ActorObject();
    aligned_free_bytes_(actor);
    g_live_actors.fetch_sub(1, std::memory_order_acq_rel);
}

void try_destroy_actor_(ActorObject* actor) {
    if (actor == nullptr) return;
    if (actor->refcount.load(std::memory_order_acquire) != 0) return;
    if (actor->active_contexts.load(std::memory_order_acquire) != 0) return;
    destroy_actor_(actor);
}

} // namespace

extern "C" {

void* __parus_actor_new(uint64_t type_tag, uint64_t draft_size, uint64_t draft_align) {
    const size_t want_align = static_cast<size_t>(draft_align == 0 ? 1 : draft_align);
    const size_t alloc_align = align_up_(std::max(alignof(ActorObject), want_align), alignof(void*));
    const size_t draft_off = align_up_(sizeof(ActorObject), want_align);
    const size_t total = draft_off + static_cast<size_t>(draft_size);

    auto* raw = static_cast<std::byte*>(aligned_alloc_bytes_(alloc_align, total));
    if (raw == nullptr) return nullptr;
    auto* actor = new (raw) ActorObject{};
    if (!actor_lock_init_(&actor->lock)) {
        actor->~ActorObject();
        aligned_free_bytes_(raw);
        return nullptr;
    }
    actor->type_tag = type_tag;
    actor->draft_size = draft_size;
    actor->draft_align = draft_align;
    actor->alloc_align = alloc_align;
    actor->draft_ptr = raw + draft_off;
    if (draft_size != 0) {
        std::memset(actor->draft_ptr, 0, static_cast<size_t>(draft_size));
    }
    g_live_actors.fetch_add(1, std::memory_order_acq_rel);
    return actor;
}

void* __parus_actor_clone(void* handle) {
    auto* actor = actor_from_handle_(handle);
    if (actor == nullptr) return nullptr;
    actor->refcount.fetch_add(1, std::memory_order_relaxed);
    return actor;
}

void __parus_actor_release(void* handle) {
    auto* actor = actor_from_handle_(handle);
    if (actor == nullptr) return;
    const uint64_t prev = actor->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 0) {
        actor->refcount.store(0, std::memory_order_release);
        return;
    }
    if (prev == 1) {
        try_destroy_actor_(actor);
    }
}

void* __parus_actor_enter(void* handle, uint32_t mode) {
    auto* actor = actor_from_handle_(handle);
    if (actor == nullptr) return nullptr;
    actor->active_contexts.fetch_add(1, std::memory_order_acq_rel);

    void* raw = std::malloc(sizeof(ActorContext));
    if (raw == nullptr) {
        actor->active_contexts.fetch_sub(1, std::memory_order_acq_rel);
        return nullptr;
    }
    auto* ctx = new (raw) ActorContext{};
    ctx->actor = actor;
    ctx->draft_ptr = actor->draft_ptr;
    ctx->mode = mode;
    if (mode == 1u) {
        actor_lock_shared_lock_(&actor->lock);
        ctx->holds_read_lock = true;
    } else {
        actor_lock_exclusive_lock_(&actor->lock);
        ctx->holds_write_lock = true;
    }
    return ctx;
}

void* __parus_actor_draft_ptr(void* ctx) {
    auto* actor_ctx = static_cast<ActorContext*>(ctx);
    return actor_ctx != nullptr ? actor_ctx->draft_ptr : nullptr;
}

void __parus_actor_commit(void* ctx) {
    (void)ctx;
}

void __parus_actor_recast(void* ctx) {
    (void)ctx;
}

void __parus_actor_leave(void* ctx) {
    auto* actor_ctx = static_cast<ActorContext*>(ctx);
    if (actor_ctx == nullptr) return;
    auto* actor = actor_ctx->actor;
    if (actor != nullptr) {
        if (actor_ctx->holds_read_lock) actor_lock_shared_unlock_(&actor->lock);
        if (actor_ctx->holds_write_lock) actor_lock_exclusive_unlock_(&actor->lock);
    }
    actor_ctx->~ActorContext();
    std::free(actor_ctx);
    if (actor != nullptr) {
        const uint64_t prev = actor->active_contexts.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            try_destroy_actor_(actor);
        }
    }
}

uint64_t __parus_prt_debug_live_actors(void) {
    return g_live_actors.load(std::memory_order_acquire);
}

}

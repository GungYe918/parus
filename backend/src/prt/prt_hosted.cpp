#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>

namespace {

std::atomic<uint64_t> g_live_actors{0};

struct ActorObject {
    std::atomic<uint64_t> refcount{1};
    std::atomic<uint64_t> active_contexts{0};
    uint64_t type_tag = 0;
    uint64_t draft_size = 0;
    uint64_t draft_align = 0;
    size_t alloc_align = alignof(void*);
    void* draft_ptr = nullptr;
    std::shared_mutex mutex{};
};

struct ActorContext {
    ActorObject* actor = nullptr;
    void* draft_ptr = nullptr;
    uint32_t mode = 0;
    std::unique_ptr<std::unique_lock<std::shared_mutex>> write_lock{};
    std::unique_ptr<std::shared_lock<std::shared_mutex>> read_lock{};
};

constexpr size_t align_up_(size_t value, size_t align) {
    return (align <= 1) ? value : ((value + align - 1) / align) * align;
}

ActorObject* actor_from_handle_(void* handle) {
    return static_cast<ActorObject*>(handle);
}

void destroy_actor_(ActorObject* actor) {
    if (actor == nullptr) return;
    const size_t alloc_align = actor->alloc_align;
    actor->~ActorObject();
    ::operator delete(static_cast<void*>(actor), std::align_val_t(alloc_align));
    g_live_actors.fetch_sub(1, std::memory_order_acq_rel);
}

void try_destroy_actor_(ActorObject* actor) {
    if (actor == nullptr) return;
    if (actor->refcount.load(std::memory_order_acquire) != 0) return;
    if (actor->active_contexts.load(std::memory_order_acquire) != 0) return;
    destroy_actor_(actor);
}

}

extern "C" {

void* __parus_actor_new(uint64_t type_tag, uint64_t draft_size, uint64_t draft_align) {
    const size_t want_align = static_cast<size_t>(draft_align == 0 ? 1 : draft_align);
    const size_t alloc_align = align_up_(std::max(alignof(ActorObject), want_align), alignof(void*));
    const size_t draft_off = align_up_(sizeof(ActorObject), want_align);
    const size_t total = draft_off + static_cast<size_t>(draft_size);

    auto* raw = static_cast<std::byte*>(::operator new(total, std::align_val_t(alloc_align)));
    auto* actor = new (raw) ActorObject{};
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

    auto* ctx = new ActorContext{};
    ctx->actor = actor;
    ctx->draft_ptr = actor->draft_ptr;
    ctx->mode = mode;
    if (mode == 1u) {
        ctx->read_lock = std::make_unique<std::shared_lock<std::shared_mutex>>(actor->mutex);
    } else {
        ctx->write_lock = std::make_unique<std::unique_lock<std::shared_mutex>>(actor->mutex);
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
    delete actor_ctx;
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

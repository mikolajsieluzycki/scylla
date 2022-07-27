/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <memory>
#include <seastar/core/memory.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/expiring_fifo.hh>
#include "allocation_strategy.hh"
#include "seastarx.hh"
#include "db/timeout_clock.hh"
#include "utils/entangled.hh"

namespace logalloc {

struct occupancy_stats;
class region;
class region_impl;
class allocating_section;

constexpr int segment_size_shift = 17; // 128K; see #151, #152
constexpr size_t segment_size = 1 << segment_size_shift;
constexpr size_t max_zone_segments = 256;

//
// Frees some amount of objects from the region to which it's attached.
//
// This should eventually stop given no new objects are added:
//
//     while (eviction_fn() == memory::reclaiming_result::reclaimed_something) ;
//
using eviction_fn = std::function<memory::reclaiming_result()>;

region* region_impl_to_region(region_impl* ri);
region_impl* region_to_region_impl(region* r);

// Listens for events from a region
class region_listener {
public:
    virtual ~region_listener();
    virtual void add(region* r) = 0;
    virtual void del(region* r) = 0;
    virtual void moved(region* old_address, region* new_address) = 0;
    virtual void increase_usage(region* r, ssize_t delta) = 0;
    virtual void decrease_evictable_usage(region* r) = 0;
    virtual void decrease_usage(region* r, ssize_t delta) = 0;
};

// Controller for all LSA regions. There's one per shard.
class tracker {
public:
    class impl;

    struct config {
        bool defragment_on_idle;
        bool abort_on_lsa_bad_alloc;
        bool sanitizer_report_backtrace = false; // Better reports but slower
        size_t lsa_reclamation_step;
        scheduling_group background_reclaim_sched_group;
    };

    void configure(const config& cfg);
    future<> stop();

private:
    std::unique_ptr<impl> _impl;
    memory::reclaimer _reclaimer;
    friend class region;
    friend class region_impl;
    memory::reclaiming_result reclaim(seastar::memory::reclaimer::request);

public:
    tracker();
    ~tracker();

    //
    // Tries to reclaim given amount of bytes in total using all compactible
    // and evictable regions. Returns the number of bytes actually reclaimed.
    // That value may be smaller than requested when evictable pools are empty
    // and compactible pools can't compact any more.
    //
    // Invalidates references to objects in all compactible and evictable regions.
    //
    size_t reclaim(size_t bytes);

    // Compacts as much as possible. Very expensive, mainly for testing.
    // Guarantees that every live object from reclaimable regions will be moved.
    // Invalidates references to objects in all compactible and evictable regions.
    void full_compaction();

    void reclaim_all_free_segments();

    // Returns aggregate statistics for all pools.
    occupancy_stats region_occupancy();

    // Returns statistics for all segments allocated by LSA on this shard.
    occupancy_stats occupancy();

    // Returns amount of allocated memory not managed by LSA
    size_t non_lsa_used_space() const;

    impl& get_impl() { return *_impl; }

    // Returns the minimum number of segments reclaimed during single reclamation cycle.
    size_t reclamation_step() const;

    bool should_abort_on_bad_alloc();
};

class tracker_reclaimer_lock {
    tracker::impl& _tracker_impl;
public:
    tracker_reclaimer_lock();
    ~tracker_reclaimer_lock();
};

tracker& shard_tracker();

class segment_descriptor;

/// A unique pointer to a chunk of memory allocated inside an LSA region.
///
/// The pointer can be in disengaged state in which case it doesn't point at any buffer (nullptr state).
/// When the pointer points at some buffer, it is said to be engaged.
///
/// The pointer owns the object.
/// When the pointer is destroyed or it transitions from engaged to disengaged state, the buffer is freed.
/// The buffer is never leaked when operating by the API of lsa_buffer.
/// The pointer object can be safely destroyed in any allocator context.
///
/// The pointer object is never invalidated.
/// The pointed-to buffer can be moved around by LSA, so the pointer returned by get() can be
/// invalidated, but the pointer object itself is updated automatically and get() always returns
/// a pointer which is valid at the time of the call.
///
/// Must not outlive the region.
class lsa_buffer {
    friend class region_impl;
    entangled _link;           // Paired with segment_descriptor::_buf_pointers[...]
    segment_descriptor* _desc; // Valid only when engaged
    char* _buf = nullptr;      // Valid only when engaged
    size_t _size = 0;
public:
    using char_type = char;

    lsa_buffer() = default;
    lsa_buffer(lsa_buffer&&) noexcept = default;
    ~lsa_buffer();

    /// Makes this instance point to the buffer pointed to by the other pointer.
    /// If this pointer was engaged before, the owned buffer is freed.
    /// The other pointer will be in disengaged state after this.
    lsa_buffer& operator=(lsa_buffer&& other) noexcept {
        if (this != &other) {
            this->~lsa_buffer();
            new (this) lsa_buffer(std::move(other));
        }
        return *this;
    }

    /// Disengages the pointer.
    /// If the pointer was engaged before, the owned buffer is freed.
    /// Postcondition: !bool(*this)
    lsa_buffer& operator=(std::nullptr_t) noexcept {
        this->~lsa_buffer();
        return *this;
    }

    /// Returns a pointer to the first element of the buffer.
    /// Valid only when engaged.
    char_type* get() { return _buf; }
    const char_type* get() const { return _buf; }

    /// Returns the number of bytes in the buffer.
    size_t size() const { return _size; }

    /// Returns true iff the pointer is engaged.
    explicit operator bool() const noexcept { return bool(_link); }
};

// Monoid representing pool occupancy statistics.
// Naturally ordered so that sparser pools come fist.
// All sizes in bytes.
class occupancy_stats {
    size_t _free_space;
    size_t _total_space;
public:
    occupancy_stats() : _free_space(0), _total_space(0) {}

    occupancy_stats(size_t free_space, size_t total_space)
        : _free_space(free_space), _total_space(total_space) { }

    bool operator<(const occupancy_stats& other) const {
        return used_fraction() < other.used_fraction();
    }

    friend occupancy_stats operator+(const occupancy_stats& s1, const occupancy_stats& s2) {
        occupancy_stats result(s1);
        result += s2;
        return result;
    }

    friend occupancy_stats operator-(const occupancy_stats& s1, const occupancy_stats& s2) {
        occupancy_stats result(s1);
        result -= s2;
        return result;
    }

    occupancy_stats& operator+=(const occupancy_stats& other) {
        _total_space += other._total_space;
        _free_space += other._free_space;
        return *this;
    }

    occupancy_stats& operator-=(const occupancy_stats& other) {
        _total_space -= other._total_space;
        _free_space -= other._free_space;
        return *this;
    }

    size_t used_space() const {
        return _total_space - _free_space;
    }

    size_t free_space() const {
        return _free_space;
    }

    size_t total_space() const {
        return _total_space;
    }

    float used_fraction() const {
        return _total_space ? float(used_space()) / total_space() : 0;
    }

    explicit operator bool() const {
        return _total_space > 0;
    }

    friend std::ostream& operator<<(std::ostream&, const occupancy_stats&);
};

class basic_region_impl : public allocation_strategy {
protected:
    bool _reclaiming_enabled = true;
    seastar::shard_id _cpu = this_shard_id();
public:
    void set_reclaiming_enabled(bool enabled) {
        assert(this_shard_id() == _cpu);
        _reclaiming_enabled = enabled;
    }

    bool reclaiming_enabled() const {
        return _reclaiming_enabled;
    }
};

//
// Log-structured allocator region.
//
// Objects allocated using this region are said to be owned by this region.
// Objects must be freed only using the region which owns them. Ownership can
// be transferred across regions using the merge() method. Region must be live
// as long as it owns any objects.
//
// Each region has separate memory accounting and can be compacted
// independently from other regions. To reclaim memory from all regions use
// shard_tracker().
//
// Region is automatically added to the set of
// compactible regions when constructed.
//
class region {
public:
    using impl = region_impl;
private:
    shared_ptr<basic_region_impl> _impl;
private:
    region_impl& get_impl();
    const region_impl& get_impl() const;
public:
    region();
    ~region();
    region(region&& other);
    region& operator=(region&& other);
    region(const region& other) = delete;

    void listen(region_listener* listener);
    void unlisten();

    occupancy_stats occupancy() const;

    allocation_strategy& allocator() noexcept {
        return *_impl;
    }
    const allocation_strategy& allocator() const noexcept {
        return *_impl;
    }

    // Allocates a buffer of a given size.
    // The buffer's pointer will be aligned to 4KB.
    // Note: it is wasteful to allocate buffers of sizes which are not a multiple of the alignment.
    lsa_buffer alloc_buf(size_t buffer_size);

    // Merges another region into this region. The other region is left empty.
    // Doesn't invalidate references to allocated objects.
    void merge(region& other) noexcept;

    // Compacts everything. Mainly for testing.
    // Invalidates references to allocated objects.
    void full_compaction();

    // Runs eviction function once. Mainly for testing.
    memory::reclaiming_result evict_some();

    // Changes the reclaimability state of this region. When region is not
    // reclaimable, it won't be considered by tracker::reclaim(). By default region is
    // reclaimable after construction.
    void set_reclaiming_enabled(bool e) { _impl->set_reclaiming_enabled(e); }

    // Returns the reclaimability state of this region.
    bool reclaiming_enabled() const { return _impl->reclaiming_enabled(); }

    // Returns a value which is increased when this region is either compacted or
    // evicted from, which invalidates references into the region.
    // When the value returned by this method doesn't change, references remain valid.
    uint64_t reclaim_counter() const {
        return allocator().invalidate_counter();
    }

    // Will cause subsequent calls to evictable_occupancy() to report empty occupancy.
    void ground_evictable_occupancy();

    // Follows region's occupancy in the parent region group. Less fine-grained than occupancy().
    // After ground_evictable_occupancy() is called returns 0.
    occupancy_stats evictable_occupancy();

    // Makes this region an evictable region. Supplied function will be called
    // when data from this region needs to be evicted in order to reclaim space.
    // The function should free some space from this region.
    void make_evictable(eviction_fn);

    const eviction_fn& evictor() const;

    uint64_t id() const;

    friend class allocating_section;
    friend region_impl* region_to_region_impl(region* r);
};

// Forces references into the region to remain valid as long as this guard is
// live by disabling compaction and eviction.
// Can be nested.
struct reclaim_lock {
    region& _region;
    bool _prev;
    reclaim_lock(region& r)
        : _region(r)
        , _prev(r.reclaiming_enabled())
    {
        _region.set_reclaiming_enabled(false);
    }
    ~reclaim_lock() {
        _region.set_reclaiming_enabled(_prev);
    }
};

// Utility for running critical sections which need to lock some region and
// also allocate LSA memory. The object learns from failures how much it
// should reserve up front in order to not cause allocation failures.
class allocating_section {
    // Do not decay below these minimal values
    static constexpr size_t s_min_lsa_reserve = 1;
    static constexpr size_t s_min_std_reserve = 1024;
    static constexpr uint64_t s_bytes_per_decay = 10'000'000'000;
    static constexpr unsigned s_segments_per_decay = 100'000;
    size_t _lsa_reserve = s_min_lsa_reserve; // in segments
    size_t _std_reserve = s_min_std_reserve; // in bytes
    size_t _minimum_lsa_emergency_reserve = 0;
    int64_t _remaining_std_bytes_until_decay = s_bytes_per_decay;
    int _remaining_lsa_segments_until_decay = s_segments_per_decay;
private:
    struct guard {
        size_t _prev;
        guard();
        ~guard();
    };
    void reserve();
    void maybe_decay_reserve();
    void on_alloc_failure(logalloc::region&);
public:

    void set_lsa_reserve(size_t);
    void set_std_reserve(size_t);

    //
    // Reserves standard allocator and LSA memory for subsequent operations that
    // have to be performed with memory reclamation disabled.
    //
    // Throws std::bad_alloc when reserves can't be increased to a sufficient level.
    //
    template<typename Func>
    decltype(auto) with_reserve(Func&& fn) {
        auto prev_lsa_reserve = _lsa_reserve;
        auto prev_std_reserve = _std_reserve;
        try {
            guard g;
            _minimum_lsa_emergency_reserve = g._prev;
            reserve();
            return fn();
        } catch (const std::bad_alloc&) {
            // roll-back limits to protect against pathological requests
            // preventing future requests from succeeding.
            _lsa_reserve = prev_lsa_reserve;
            _std_reserve = prev_std_reserve;
            throw;
        }
    }

    //
    // Invokes func with reclaim_lock on region r. If LSA allocation fails
    // inside func it is retried after increasing LSA segment reserve. The
    // memory reserves are increased with region lock off allowing for memory
    // reclamation to take place in the region.
    //
    // References in the region are invalidated when allocating section is re-entered
    // on allocation failure.
    //
    // Throws std::bad_alloc when reserves can't be increased to a sufficient level.
    //
    template<typename Func>
    decltype(auto) with_reclaiming_disabled(logalloc::region& r, Func&& fn) {
        assert(r.reclaiming_enabled());
        maybe_decay_reserve();
        while (true) {
            try {
                logalloc::reclaim_lock _(r);
                memory::disable_abort_on_alloc_failure_temporarily dfg;
                return fn();
            } catch (const std::bad_alloc&) {
                on_alloc_failure(r);
            }
        }
    }

    //
    // Reserves standard allocator and LSA memory and
    // invokes func with reclaim_lock on region r. If LSA allocation fails
    // inside func it is retried after increasing LSA segment reserve. The
    // memory reserves are increased with region lock off allowing for memory
    // reclamation to take place in the region.
    //
    // References in the region are invalidated when allocating section is re-entered
    // on allocation failure.
    //
    // Throws std::bad_alloc when reserves can't be increased to a sufficient level.
    //
    template<typename Func>
    decltype(auto) operator()(logalloc::region& r, Func&& func) {
        return with_reserve([this, &r, &func] {
            return with_reclaiming_disabled(r, func);
        });
    }
};

future<> prime_segment_pool(size_t available_memory, size_t min_free_memory);

uint64_t memory_allocated();
uint64_t memory_freed();
uint64_t memory_compacted();
uint64_t memory_evicted();

occupancy_stats lsa_global_occupancy_stats();

}

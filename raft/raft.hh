/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <vector>
#include <unordered_set>
#include <functional>
#include <experimental/source_location>
#include <boost/container/deque.hpp>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/future.hh>
#include <seastar/util/log.hh>
#include <seastar/core/abort_source.hh>
#include "bytes_ostream.hh"
#include "utils/UUID.hh"
#include "internal.hh"
#include "logical_clock.hh"

namespace raft {
// Keeps user defined command. A user is responsible to serialize
// a state machine operation into it before passing to raft and
// deserialize in apply() before applying.
using command = bytes_ostream;
using command_cref = std::reference_wrapper<const command>;

extern seastar::logger logger;

// This is user provided id for a snapshot
using snapshot_id = internal::tagged_id<struct snapshot_id_tag>;
// Unique identifier of a server in a Raft group
using server_id = internal::tagged_id<struct server_id_tag>;
// Unique identifier of a Raft group
using group_id = raft::internal::tagged_id<struct group_id_tag>;

// This type represents the raft term
using term_t = internal::tagged_uint64<struct term_tag>;
// This type represensts the index into the raft log
using index_t = internal::tagged_uint64<struct index_tag>;
// Identifier for a read barrier request
using read_id = internal::tagged_uint64<struct read_id_tag>;

// Opaque connection properties. May contain ip:port pair for instance.
// This value is disseminated between cluster member
// through regular log replication as part of a configuration
// log entry. Upon receiving it a server passes it down to
// RPC module through add_server() call where it is deserialized
// and used to obtain connection info for the node `id`. After a server
// is added to the RPC module RPC's send functions can be used to communicate
// with it using its `id`.
using server_info = bytes;

struct server_address {
    server_id id;
    server_info info;

    server_address(server_id id, server_info info)
        : id(std::move(id)), info(std::move(info)) {
    }

    bool operator==(const server_address& rhs) const {
        return id == rhs.id;
    }

    bool operator==(const raft::server_id& rhs) const {
        return id == rhs;
    }

    bool operator<(const server_address& rhs) const {
        return id < rhs.id;
    }

    friend std::ostream& operator<<(std::ostream&, const server_address&);
};

struct config_member {
    server_address addr;
    bool can_vote;

    config_member(server_address addr, bool can_vote)
        : addr(std::move(addr)), can_vote(can_vote) {
    }

    bool operator==(const config_member& rhs) const {
        return addr == rhs.addr;
    }

    bool operator==(const raft::server_id& rhs) const {
        return addr.id == rhs;
    }

    bool operator<(const config_member& rhs) const {
        return addr < rhs.addr;
    }

    friend std::ostream& operator<<(std::ostream&, const config_member&);
};

struct server_address_hash {
    using is_transparent = void;

    size_t operator()(const raft::server_id& id) const {
        return std::hash<raft::server_id>{}(id);
    }

    size_t operator()(const raft::server_address& address) const {
        return operator()(address.id);
    }
};

struct config_member_hash {
    using is_transparent = void;

    size_t operator()(const raft::server_id& id) const {
        return std::hash<raft::server_id>{}(id);
    }

    size_t operator()(const raft::server_address& address) const {
        return operator()(address.id);
    }

    size_t operator()(const raft::config_member& s) const {
        return operator()(s.addr);
    }
};

using server_address_set = std::unordered_set<server_address, server_address_hash, std::equal_to<>>;
using config_member_set = std::unordered_set<config_member, config_member_hash, std::equal_to<>>;

// A configuration change decomposed to joining and leaving
// servers. Helps validate the configuration and update RPC.
struct configuration_diff {
    config_member_set joining, leaving;
};

struct configuration {
    // Contains the current configuration. When configuration
    // change is in progress, contains the new configuration.
    config_member_set current;
    // Used during the transitioning period of configuration
    // changes.
    config_member_set previous;

    explicit configuration(config_member_set current_arg = {}, config_member_set previous_arg = {})
        : current(std::move(current_arg)), previous(std::move(previous_arg)) {
            if (current.count(server_id{}) || previous.count(server_id{})) {
                throw std::invalid_argument("raft::configuration: id zero is not supported");
            }
        }

    // Return true if the previous configuration is still
    // in use
    bool is_joint() const {
        return !previous.empty();
    }

    // Count the number of voters in a configuration
    static size_t voter_count(const config_member_set& c_new) {
        return std::count_if(c_new.begin(), c_new.end(), [] (const config_member& s) { return s.can_vote; });
    }

    // Check if transitioning to a proposed configuration is safe.
    static void check(const config_member_set& c_new) {
        // We must have at least one voting member in the config.
        if (c_new.empty()) {
            throw std::invalid_argument("Attempt to transition to an empty Raft configuration");
        }
        if (voter_count(c_new) == 0) {
            throw std::invalid_argument("The configuration must have at least one voter");
        }
    }

    // Compute a diff between a proposed configuration and the current one.
    configuration_diff diff(const config_member_set& c_new) const {
        configuration_diff diff;
        // joining
        for (const auto& s : c_new) {
            auto it = current.find(s);
            // a node is added to a joining set if it is not yet known or its voting status changes
            if (it == current.end() || it->can_vote != s.can_vote) {
                diff.joining.insert(s);
            }
        }

        // leaving
        for (const auto& s : current) {
            if (!c_new.contains(s)) {
                diff.leaving.insert(s);
            }
        }
        return diff;
    }

    // True if the current or previous configuration contains
    // this server.
    bool contains(server_id id) const {
        auto it = current.find(id);
        if (it != current.end()) {
            return true;
        }
        return previous.find(id) != previous.end();
    }

    // Same as contains() but true only if the member can vote.
    bool can_vote(server_id id) const {
        bool can_vote = false;
        auto it = current.find(id);
        if (it != current.end()) {
            can_vote |= it->can_vote;
        }

        it = previous.find(id);
        if (it != previous.end()) {
            can_vote |= it->can_vote;
        }

        return can_vote;
    }

    // Enter a joint configuration given a new set of servers.
    void enter_joint(config_member_set c_new) {
        if (c_new.empty()) {
            throw std::invalid_argument("Attempt to transition to an empty Raft configuration");
        }
        previous = std::move(current);
        current = std::move(c_new);
    }

    // Transition from C_old + C_new to C_new.
    void leave_joint() {
        assert(is_joint());
        previous.clear();
    }

    friend std::ostream& operator<<(std::ostream&, const configuration&);
};

struct log_entry {
    // Dummy entry is used when a leader needs to commit an entry
    // (after leadership change for instance) but there is nothing
    // else to commit.
    struct dummy {};
    term_t term;
    index_t idx;
    std::variant<command, configuration, dummy> data;
};

using log_entry_ptr = seastar::lw_shared_ptr<const log_entry>;

struct error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct not_a_leader : public error {
    server_id leader;
    explicit not_a_leader(server_id l) : error(format("Not a leader, leader: {}", l)), leader(l) {}
};

struct dropped_entry : public error {
    dropped_entry() : error("Entry was dropped because of a leader change") {}
};

struct commit_status_unknown : public error {
    commit_status_unknown() : error("Commit status of the entry is unknown") {}
};

struct stopped_error : public error {
    stopped_error() : error("Raft instance is stopped") {}
};

struct conf_change_in_progress : public error {
    conf_change_in_progress() : error("A configuration change is already in progress") {}
};

struct config_error : public error {
    using error::error;
};


struct timeout_error : public error {
    using error::error;
};

struct state_machine_error: public error {
    state_machine_error(std::experimental::source_location l = std::experimental::source_location::current())
        : error(fmt::format("State machine error at {}:{}", l.file_name(), l.line())) {}
};

struct no_other_voting_member : public error {
    no_other_voting_member() : error("Cannot stepdown because there is no other voting member") {}
};

struct request_aborted : public error {
    request_aborted() : error("Request is aborted by a caller") {}
};

// True if a failure to execute a Raft operation can be re-tried,
// perhaps with a different server.
inline bool is_transient_error(const std::exception& e) {
    return dynamic_cast<const not_a_leader*>(&e) ||
           dynamic_cast<const dropped_entry*>(&e) ||
           dynamic_cast<const conf_change_in_progress*>(&e);
}

inline bool is_uncertainty(const std::exception& e) {
    return dynamic_cast<const commit_status_unknown*>(&e) ||
           dynamic_cast<const stopped_error*>(&e);
}

struct snapshot_descriptor {
    // Index and term of last entry in the snapshot
    index_t idx = index_t(0);
    term_t term = term_t(0);
    // The committed configuration in the snapshot
    configuration config{};
    // Id of the snapshot.
    snapshot_id id;
};

struct append_request {
    // The leader's term.
    term_t current_term;
    // Index of the log entry immediately preceding new ones
    index_t prev_log_idx;
    // Term of prev_log_idx entry.
    term_t prev_log_term;
    // The leader's commit_idx.
    index_t leader_commit_idx;
    // Log entries to store (empty vector for heartbeat; may send more
    // than one entry for efficiency).
    std::vector<log_entry_ptr> entries;
};

struct append_reply {
    struct rejected {
        // Index of non matching entry that caused the request
        // to be rejected.
        index_t non_matching_idx;
        // Last index in the follower's log, can be used to find next
        // matching index more efficiently.
        index_t last_idx;
    };
    struct accepted {
        // Last entry that was appended (may be smaller than max log index
        // in case follower's log is longer and appended entries match).
        index_t last_new_idx;
    };
    // Current term, for leader to update itself.
    term_t current_term;
    // Contains an index of the last committed entry on the follower
    // It is used by a leader to know if a follower is behind and issuing
    // empty append entry with updates commit_idx if it is
    // Regular RAFT handles this by always sending enoty append requests
    // as a heartbeat.
    index_t commit_idx;
    std::variant<rejected, accepted> result;
};

struct vote_request {
    // The candidate’s term.
    term_t current_term;
    // The index of the candidate's last log entry.
    index_t last_log_idx;
    // The term of the candidate's last log entry.
    term_t last_log_term;
    // True if this is prevote request
    bool is_prevote;
    // If the flag is set the request will not be ignored even
    // if there is an active leader. Used during leadership transfer.
    bool force;
};

struct vote_reply {
    // Current term, for the candidate to update itself.
    term_t current_term;
    // True means the candidate received a vote.
    bool vote_granted;
    // True if it is a reply to prevote request
    bool is_prevote;
};

struct install_snapshot {
    // Current term on a leader
    term_t current_term;
    // A snapshot to install
    snapshot_descriptor snp;
};

struct snapshot_reply {
    // Follower current term
    term_t current_term;
    // True if the snapshot was applied, false otherwise.
    bool success;
};

// 3.10 section from PhD Leadership transfer extension
struct timeout_now {
    // Current term on a leader
    term_t current_term;
};

struct read_quorum {
    // The leader's term.
    term_t current_term;
    // The leader's commit_idx. Has the same semantics
    // as in append_entries.
    index_t leader_commit_idx;
    // The id of the read barrier. Only valid within this term.
    read_id id;
};

struct read_quorum_reply {
    // The leader's term, as sent in the read_quorum request.
    // read_id is only valid (and unique) within a given term.
    term_t current_term;
    // Piggy-back follower's commit_idx, for the same purposes
    // as in append_reply::commit_idx
    index_t commit_idx;
    // Copy of the id from a read_quorum request
    read_id id;
};

struct entry_id {
    // Added entry term
    term_t term;
    // Added entry log index
    index_t idx;
};

// Response to add_entry or modify_config RPC.
// Carries either entry id (the entry is not committed yet),
// not_a_leader (the entry is not added to Raft log), or, for
// modify_config, commit_status_unknown (commit status is
// unknown).
using add_entry_reply = std::variant<entry_id, not_a_leader, commit_status_unknown>;

// std::monostate {} if the leader cannot execute the barrier because
// it did not commit any entries yet
// raft::not_a_leader if the node is not a leader
// index_t index that is safe to read without breaking linearizability
using read_barrier_reply = std::variant<std::monostate, index_t, raft::not_a_leader>;

using rpc_message = std::variant<append_request,
      append_reply,
      vote_request,
      vote_reply,
      install_snapshot,
      snapshot_reply,
      timeout_now,
      read_quorum,
      read_quorum_reply>;

// we need something that can be truncated form both sides.
// std::deque move constructor is not nothrow hence cannot be used
using log_entries = boost::container::deque<log_entry_ptr>;

// 3.4 Leader election
// If a follower receives no communication over a period of
// time called the election timeout, then it assumes there is
// no viable leader and begins an election to choose a new
// leader.
static constexpr logical_clock::duration ELECTION_TIMEOUT = logical_clock::duration{10};

// rpc, persistence and state_machine classes will have to be implemented by the
// raft user to provide network, persistency and busyness logic support
// repectively.
class rpc;
class persistence;

// Any of the functions may return an error, but it will kill the
// raft instance that uses it. Depending on what state the failure
// leaves the state is the raft instance will either have to be recreated
// with the same state machine and rejoined the cluster with the same server_id
// or it new raft instance will have to be created with empty state machine and
// it will have to rejoin to the cluster with different server_id through
// configuration change.
class state_machine {
public:
    virtual ~state_machine() {}

    // This is called after entries are committed (replicated to
    // at least quorum of servers). If a provided vector contains
    // more than one entry all of them will be committed simultaneously.
    // Will be eventually called on all replicas, for all committed commands.
    // Raft owns the data since it may be still replicating.
    // Raft will not call another apply until the retuned future
    // will not become ready.
    virtual future<> apply(std::vector<command_cref> command) = 0;

    // The function suppose to take a snapshot of a state machine
    // To be called during log compaction or when a leader brings
    // a lagging follower up-to-date
    virtual future<snapshot_id> take_snapshot() = 0;

    // The function drops a snapshot with a provided id
    virtual void drop_snapshot(snapshot_id id) = 0;

    // reload state machine from a snapshot id
    // To be used by a restarting server or by a follower that
    // catches up to a leader
    virtual future<> load_snapshot(snapshot_id id) = 0;

    // stops the state machine instance by aborting the work
    // that can be aborted and waiting for all the rest to complete
    // any unfinished apply/snapshot operation may return an error after
    // this function is called
    virtual future<> abort() = 0;
};

class rpc_server;

// It is safe for for rpc implementation to drop any message.
// Error returned by send function will be ignored. All send_()
// functions can be called concurrently, returned future should be
// waited only for back pressure purposes (unless specified otherwise in
// the function's comment). Values passed by reference may be freed as soon
// as function returns.
class rpc {
protected:
    // Pointer to Raft server. Needed for passing RPC messages.
    rpc_server* _client = nullptr;
public:
    virtual ~rpc() {}

    // Send a snapshot snap to a server server_id.
    //
    // Unlike other RPC, this is a synchronous call:
    //
    // A returned future is resolved when snapshot is sent and
    // successfully applied by a receiver. Will be waited to
    // know if a snapshot transfer succeeded.
    virtual future<snapshot_reply> send_snapshot(server_id server_id, const install_snapshot& snap, seastar::abort_source& as) = 0;

    // Send provided append_request to the supplied server, does
    // not wait for reply. The returned future resolves when
    // message is sent. It does not mean it was received.
    virtual future<> send_append_entries(server_id id, const append_request& append_request) = 0;

    // Send a reply to an append_request.
    virtual void send_append_entries_reply(server_id id, const append_reply& reply) = 0;

    // Send a vote request.
    virtual void send_vote_request(server_id id, const vote_request& vote_request) = 0;

    // Sends a reply to a vote request.
    virtual void send_vote_reply(server_id id, const vote_reply& vote_reply) = 0;

    // Send a request to start leader election.
    virtual void send_timeout_now(server_id, const timeout_now& timeout_now) = 0;

    // Send a read barrier request.
    virtual void send_read_quorum(server_id id, const read_quorum& read_quorum) = 0;

    // Send a reply to read barrier request.
    virtual void send_read_quorum_reply(server_id id, const read_quorum_reply& read_quorum_reply) = 0;

    // Forward a read barrier request to the leader.
    virtual future<read_barrier_reply> execute_read_barrier_on_leader(server_id id) = 0;

    // Two-way RPC for adding an entry on the leader
    // @param id the leader
    // @param cmd raft::command to be added to the leader's log
    // @retval either term and index of the committed entry or
    // not_a_leader exception.
    virtual future<add_entry_reply> send_add_entry(server_id id, const command& cmd) = 0;

    // Send a configuration change request to the leader. Block until the
    // leader replies.
    virtual future<add_entry_reply> send_modify_config(server_id id,
        const std::vector<config_member>& add,
        const std::vector<server_id>& del) = 0;

    // When a new server is learn this function is called with the
    // info about the server.
    virtual void add_server(server_address) = 0;

    // When a server is removed from local config this call is
    // executed.
    virtual void remove_server(server_id id) = 0;

    // Stop the RPC instance by aborting the work that can be
    // aborted and waiting for all the rest to complete any
    // unfinished send operation may return an error after this
    // function is called.
    //
    // The implementation must ensure that `_client->apply_snapshot`, `_client->execute_add_entry`,
    // `_client->execute_modify_config` and `_client->execute_read_barrier` are not called
    // after `abort()` is called (even before `abort()` future resolves).
    virtual future<> abort() = 0;
private:
    friend rpc_server;
};

// Each Raft server is a receiver of RPC messages.
// Defines the API specific to receiving RPC input.
class rpc_server {
public:
    virtual ~rpc_server() {};

    // This function is called by append_entries RPC
    virtual void append_entries(server_id from, append_request append_request) = 0;

    // This function is called by append_entries_reply RPC
    virtual void append_entries_reply(server_id from, append_reply reply) = 0;

    // This function is called to handle RequestVote RPC.
    virtual void request_vote(server_id from, vote_request vote_request) = 0;
    // Handle response to RequestVote RPC
    virtual void request_vote_reply(server_id from, vote_reply vote_reply) = 0;

    virtual void timeout_now_request(server_id from, timeout_now timeout_now) = 0;

    virtual void read_quorum_request(server_id from, read_quorum read_quorum) = 0;

    virtual void read_quorum_reply(server_id from, read_quorum_reply read_quorum_reply) = 0;

    // Apply incoming snapshot, future resolves when application is complete
    virtual future<snapshot_reply> apply_snapshot(server_id from, install_snapshot snp) = 0;

    // Try to execute read barrier, future resolves when the barrier is completed or error happens
    virtual future<read_barrier_reply> execute_read_barrier(server_id from, seastar::abort_source* as) = 0;

    // An endpoint on the leader to add an entry to the raft log,
    // as requested by a remote follower.
    virtual future<add_entry_reply> execute_add_entry(server_id from, command cmd, seastar::abort_source* as) = 0;

    // An endpoint on the leader to change configuration,
    // as requested by a remote follower.
    // If the future resolves successfully, a dummy entry was committed after the configuration change.
    virtual future<add_entry_reply> execute_modify_config(server_id from,
        std::vector<config_member> add,
        std::vector<server_id> del, seastar::abort_source* as) = 0;

    // Update RPC implementation with this client as
    // the receiver of RPC input.
    void set_rpc_server(class rpc *rpc) { rpc->_client = this; }
};

// This class represents persistent storage state for the internal fsm. If any of the
// function returns an error the Raft instance will be aborted.
class persistence {
public:
    virtual ~persistence() {}

    // Persist given term and vote.
    // Can be called concurrently with other save-* functions in
    // the persistence and with itself but an implementation has to
    // make sure that the result is returned back in the calling order.
    virtual future<> store_term_and_vote(term_t term, server_id vote) = 0;

    // Load persisted term and vote.
    // Called during Raft server initialization only, is not run
    // in parallel with store.
    virtual future<std::pair<term_t, server_id>> load_term_and_vote() = 0;

    // Persist given commit index.
    // Cannot be called conccurrently with itself.
    // Persisting a commit index is optional.
    virtual future<> store_commit_idx(index_t idx) = 0;

    // Load persisted commit index.
    // Called during Raft server initialization only, is not run
    // in parallel with store. If no commit index was storred zero
    // will be returned.
    virtual future<index_t> load_commit_idx() = 0;

    // Persist given snapshot and drop all but 'preserve_log_entries'
    // entries from the Raft log starting from the beginning.
    // This can overwrite a previously persisted snapshot.
    // Is called only after the previous invocation completes.
    // In other words, it's the caller's responsibility to serialize
    // calls to this function. Can be called in parallel with
    // store_log_entries() but snap.index should belong to an already
    // persisted entry.
    virtual future<> store_snapshot_descriptor(const snapshot_descriptor& snap, size_t preserve_log_entries) = 0;

    // Load a saved snapshot.
    // This only loads it into memory, but does not apply yet. To
    // apply call 'state_machine::load_snapshot(snapshot::id)'
    // Called during Raft server initialization only, should not
    // run in parallel with store.
    virtual future<snapshot_descriptor> load_snapshot_descriptor() = 0;

    // Persist given log entries.
    // Can be called without waiting for previous call to resolve,
    // but internally all writes should be serialized into forming
    // one contiguous log that holds entries in order of the
    // function invocation.
    virtual future<> store_log_entries(const std::vector<log_entry_ptr>& entries) = 0;

    // Load saved Raft log. Called during Raft server
    // initialization only, should not run in parallel with store.
    virtual future<log_entries> load_log() = 0;

    // Truncate all entries with an index greater or equal than
    // the given index in the log and persist the truncation. Can be
    // called in parallel with store_log_entries() but internally
    // should be linearized vs store_log_entries():
    // store_log_entries() called after truncate_log() should wait
    // for truncation to complete internally before persisting its
    // entries.
    virtual future<> truncate_log(index_t idx) = 0;

    // Stop the persistence instance by aborting the work that can be
    // aborted and waiting for all the rest to complete. Any
    // unfinished store/load operation may return an error after
    // this function is called.
    virtual future<> abort() = 0;
};

// To support many Raft groups per server, Seastar Raft
// extends original Raft with a shared failure detector.
// It is used instead of empty AppendEntries PRCs in idle
// cluster.
// This allows multiple Raft groups to share heartbeat traffic.
class failure_detector {
public:
    virtual ~failure_detector() {}
    // Called by each server on each tick, which defaults to 10
    // per second. Should return true if the server is
    // alive. False results may impact liveness.
    virtual bool is_alive(server_id server) = 0;
};

} // namespace raft


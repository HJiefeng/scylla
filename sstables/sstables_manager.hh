/*
 * Copyright (C) 2019-present ScyllaDB
 *
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>

#include "utils/disk-error-handler.hh"
#include "gc_clock.hh"
#include "sstables/sstables.hh"
#include "sstables/shareable_components.hh"
#include "sstables/shared_sstable.hh"
#include "sstables/sstables.hh"
#include "sstables/version.hh"
#include "sstables/component_type.hh"
#include "db/cache_tracker.hh"

#include <boost/intrusive/list.hpp>

namespace db {

class large_data_handler;
class config;

}   // namespace db

namespace gms { class feature_service; }

namespace sstables {

using schema_ptr = lw_shared_ptr<const schema>;
using shareable_components_ptr = lw_shared_ptr<shareable_components>;

static constexpr size_t default_sstable_buffer_size = 128 * 1024;

class sstables_manager {
    using list_type = boost::intrusive::list<sstable,
            boost::intrusive::member_hook<sstable, sstable::manager_link_type, &sstable::_manager_link>,
            boost::intrusive::constant_time_size<false>>;
private:
    db::large_data_handler& _large_data_handler;
    const db::config& _db_config;
    gms::feature_service& _features;
    // _sstables_format is the format used for writing new sstables.
    // Here we set its default value, but if we discover that all the nodes
    // in the cluster support a newer format, _sstables_format will be set to
    // that format. read_sstables_format() also overwrites _sstables_format
    // if an sstable format was chosen earlier (and this choice was persisted
    // in the system table).
    sstable_version_types _format = sstable_version_types::mc;

    // _active and _undergoing_close are used in scylla-gdb.py to fetch all sstables
    // on current shard using "scylla sstables" command. If those fields are renamed,
    // update scylla-gdb.py as well.
    list_type _active;
    list_type _undergoing_close;

    bool _closing = false;
    promise<> _done;
    cache_tracker& _cache_tracker;
public:
    explicit sstables_manager(db::large_data_handler& large_data_handler, const db::config& dbcfg, gms::feature_service& feat, cache_tracker&);
    virtual ~sstables_manager();

    // Constructs a shared sstable
    shared_sstable make_sstable(schema_ptr schema,
            sstring dir,
            generation_type generation,
            sstable_version_types v,
            sstable_format_types f,
            gc_clock::time_point now = gc_clock::now(),
            io_error_handler_gen error_handler_gen = default_io_error_handler_gen(),
            size_t buffer_size = default_sstable_buffer_size);

    virtual sstable_writer_config configure_writer(sstring origin) const;
    const db::config& config() const { return _db_config; }
    cache_tracker& get_cache_tracker() { return _cache_tracker; }

    void set_format(sstable_version_types format) noexcept { _format = format; }
    sstables::sstable::version_types get_highest_supported_format() const noexcept { return _format; }

    const utils::UUID& get_local_host_id() const;

    // Wait until all sstables managed by this sstables_manager instance
    // (previously created by make_sstable()) have been disposed of:
    //   - if they were marked for deletion, the files are deleted
    //   - in any case, the open file handles are closed
    //   - all memory resources are freed
    //
    // Note that close() will not complete until all references to all
    // sstables have been destroyed.
    future<> close();
private:
    void add(sstable* sst);
    // Transition the sstable to the "inactive" state. It has no
    // visible references at this point, and only waits for its
    // files to be deleted (if necessary) and closed.
    void deactivate(sstable* sst);
    void remove(sstable* sst);
    void maybe_done();
private:
    db::large_data_handler& get_large_data_handler() const {
        return _large_data_handler;
    }
    friend class sstable;
};

}   // namespace sstables

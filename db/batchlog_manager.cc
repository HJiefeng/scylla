/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include <chrono>
#include <exception>
#include <seastar/core/future-util.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/sliced.hpp>

#include "batchlog_manager.hh"
#include "canonical_mutation.hh"
#include "service/storage_proxy.hh"
#include "system_keyspace.hh"
#include "utils/rate_limiter.hh"
#include "log.hh"
#include "serializer.hh"
#include "db_clock.hh"
#include "unimplemented.hh"
#include "gms/failure_detector.hh"
#include "gms/gossiper.hh"
#include "schema_registry.hh"
#include "idl/uuid.dist.hh"
#include "idl/frozen_schema.dist.hh"
#include "serializer_impl.hh"
#include "serialization_visitors.hh"
#include "db/schema_tables.hh"
#include "idl/uuid.dist.impl.hh"
#include "idl/frozen_schema.dist.impl.hh"
#include "message/messaging_service.hh"
#include "cql3/untyped_result_set.hh"
#include "service_permit.hh"
#include "cql3/query_processor.hh"

static logging::logger blogger("batchlog_manager");

const uint32_t db::batchlog_manager::replay_interval;
const uint32_t db::batchlog_manager::page_size;

db::batchlog_manager::batchlog_manager(cql3::query_processor& qp, batchlog_manager_config config)
        : _qp(qp)
        , _write_request_timeout(std::chrono::duration_cast<db_clock::duration>(config.write_request_timeout))
        , _replay_rate(config.replay_rate)
        , _started(make_ready_future<>())
        , _delay(config.delay) {
    namespace sm = seastar::metrics;

    _metrics.add_group("batchlog_manager", {
        sm::make_counter("total_write_replay_attempts", _stats.write_attempts,
                        sm::description("Counts write operations issued in a batchlog replay flow. "
                                        "The high value of this metric indicates that we have a long batch replay list.")),
    });
}

future<> db::batchlog_manager::do_batch_log_replay() {
    return container().invoke_on(0, [] (auto& bm) -> future<> {
        auto gate_holder = bm._gate.hold();
        auto sem_units = co_await get_units(bm._sem, 1);

        auto dest = bm._cpu++ % smp::count;
        blogger.debug("Batchlog replay on shard {}: starts", dest);
        if (dest == 0) {
            co_await bm.replay_all_failed_batches();
        } else {
            co_await bm.container().invoke_on(dest, [] (auto& bm) {
                return with_gate(bm._gate, [&bm] {
                    return bm.replay_all_failed_batches();
                });
            });
        }
        blogger.debug("Batchlog replay on shard {}: done", dest);
    });
}

future<> db::batchlog_manager::batchlog_replay_loop() {
    assert (this_shard_id() == 0);
    auto delay = _delay;
    while (!_stop.abort_requested()) {
        try {
            co_await sleep_abortable(delay, _stop);
        } catch (sleep_aborted&) {
            co_return;
        }
        try {
            co_await do_batch_log_replay();
        } catch (seastar::broken_semaphore&) {
            if (_stop.abort_requested()) {
                co_return;
            }
            on_internal_error_noexcept(blogger, fmt::format("Unexcepted exception in batchlog reply: {}", std::current_exception()));
        } catch (...) {
            blogger.error("Exception in batch replay: {}", std::current_exception());
        }
        delay = std::chrono::milliseconds(replay_interval);
    }
}

future<> db::batchlog_manager::start() {
    // Since replay is a "node global" operation, we should not attempt to do
    // it in parallel on each shard. It will just overlap/interfere.  To
    // simplify syncing between batchlog_replay_loop and user initiated replay operations,
    // we use the _sem on shard zero only. Replaying batchlog can
    // generate a lot of work, so we distrute the real work on all cpus with
    // round-robin scheduling.
    if (this_shard_id() == 0) {
        _started = batchlog_replay_loop();
    }
    return make_ready_future<>();
}

future<> db::batchlog_manager::drain() {
    if (!_stop.abort_requested()) {
        _stop.request_abort();
    }
    if (this_shard_id() == 0) {
        // Abort do_batch_log_replay if waiting on the semaphore.
        _sem.broken();
    }
    return with_gate(_gate, [this] {
        return std::exchange(_started, make_ready_future<>());
    });
}

future<> db::batchlog_manager::stop() {
    return drain().finally([this] {
        return _gate.close();
    });
}

future<size_t> db::batchlog_manager::count_all_batches() const {
    sstring query = format("SELECT count(*) FROM {}.{}", system_keyspace::NAME, system_keyspace::BATCHLOG);
    return _qp.execute_internal(query, cql3::query_processor::cache_internal::yes).then([](::shared_ptr<cql3::untyped_result_set> rs) {
       return size_t(rs->one().get_as<int64_t>("count"));
    });
}

db_clock::duration db::batchlog_manager::get_batch_log_timeout() const {
    // enough time for the actual write + BM removal mutation
    return _write_request_timeout * 2;
}

future<> db::batchlog_manager::replay_all_failed_batches() {
    typedef db_clock::rep clock_type;

    // rate limit is in bytes per second. Uses Double.MAX_VALUE if disabled (set to 0 in cassandra.yaml).
    // max rate is scaled by the number of nodes in the cluster (same as for HHOM - see CASSANDRA-5272).
    auto throttle = _replay_rate / _qp.proxy().get_token_metadata_ptr()->count_normal_token_owners();
    auto limiter = make_lw_shared<utils::rate_limiter>(throttle);

    auto batch = [this, limiter](const cql3::untyped_result_set::row& row) {
        auto written_at = row.get_as<db_clock::time_point>("written_at");
        auto id = row.get_as<utils::UUID>("id");
        // enough time for the actual write + batchlog entry mutation delivery (two separate requests).
        auto timeout = get_batch_log_timeout();
        if (db_clock::now() < written_at + timeout) {
            blogger.debug("Skipping replay of {}, too fresh", id);
            return make_ready_future<>();
        }

        // check version of serialization format
        if (!row.has("version")) {
            blogger.warn("Skipping logged batch because of unknown version");
            return make_ready_future<>();
        }

        auto version = row.get_as<int32_t>("version");
        if (version != netw::messaging_service::current_version) {
            blogger.warn("Skipping logged batch because of incorrect version");
            return make_ready_future<>();
        }

        auto data = row.get_blob("data");

        blogger.debug("Replaying batch {}", id);

        auto fms = make_lw_shared<std::deque<canonical_mutation>>();
        auto in = ser::as_input_stream(data);
        while (in.size()) {
            fms->emplace_back(ser::deserialize(in, boost::type<canonical_mutation>()));
        }

        auto size = data.size();

        return map_reduce(*fms, [this, written_at] (canonical_mutation& fm) {
            return system_keyspace::get_truncated_at(fm.column_family_id()).then([written_at, &fm] (db_clock::time_point t) ->
                    std::optional<std::reference_wrapper<canonical_mutation>> {
                if (written_at > t) {
                    return { std::ref(fm) };
                } else {
                    return {};
                }
            });
        },
        std::vector<mutation>(),
        [this] (std::vector<mutation> mutations, std::optional<std::reference_wrapper<canonical_mutation>> fm) {
            if (fm) {
                schema_ptr s = _qp.db().find_schema(fm.value().get().column_family_id());
                mutations.emplace_back(fm.value().get().to_mutation(s));
            }
            return mutations;
        }).then([this, id, limiter, written_at, size, fms] (std::vector<mutation> mutations) {
            if (mutations.empty()) {
                return make_ready_future<>();
            }
            const auto ttl = [this, &mutations, written_at]() -> clock_type {
                /*
                 * Calculate ttl for the mutations' hints (and reduce ttl by the time the mutations spent in the batchlog).
                 * This ensures that deletes aren't "undone" by an old batch replay.
                 */
                auto unadjusted_ttl = std::numeric_limits<gc_clock::rep>::max();
                warn(unimplemented::cause::HINT);
#if 0
                for (auto& m : *mutations) {
                    unadjustedTTL = Math.min(unadjustedTTL, HintedHandOffManager.calculateHintTTL(mutation));
                }
#endif
                return unadjusted_ttl - std::chrono::duration_cast<gc_clock::duration>(db_clock::now() - written_at).count();
            }();

            if (ttl <= 0) {
                return make_ready_future<>();
            }
            // Origin does the send manually, however I can't see a super great reason to do so.
            // Our normal write path does not add much redundancy to the dispatch, and rate is handled after send
            // in both cases.
            // FIXME: verify that the above is reasonably true.
            return limiter->reserve(size).then([this, mutations = std::move(mutations), id] {
                _stats.write_attempts += mutations.size();
                // #1222 - change cl level to ALL, emulating origins behaviour of sending/hinting
                // to all natural end points.
                // Note however that origin uses hints here, and actually allows for this
                // send to partially or wholly fail in actually sending stuff. Since we don't
                // have hints (yet), send with CL=ALL, and hope we can re-do this soon.
                // See below, we use retry on write failure.
                return _qp.proxy().mutate(mutations, db::consistency_level::ALL, db::no_timeout, nullptr, empty_service_permit());
            });
        }).then_wrapped([this, id](future<> batch_result) {
            try {
                batch_result.get();
            } catch (data_dictionary::no_such_keyspace& ex) {
                // should probably ignore and drop the batch
            } catch (...) {
                blogger.warn("Replay failed (will retry): {}", std::current_exception());
                // timeout, overload etc.
                // Do _not_ remove the batch, assuning we got a node write error.
                // Since we don't have hints (which origin is satisfied with),
                // we have to resort to keeping this batch to next lap.
                return make_ready_future<>();
            }
            // delete batch
            auto schema = _qp.db().find_schema(system_keyspace::NAME, system_keyspace::BATCHLOG);
            auto key = partition_key::from_singular(*schema, id);
            mutation m(schema, key);
            auto now = service::client_state(service::client_state::internal_tag()).get_timestamp();
            m.partition().apply_delete(*schema, clustering_key_prefix::make_empty(), tombstone(now, gc_clock::now()));
            return _qp.proxy().mutate_locally(m, tracing::trace_state_ptr(), db::commitlog::force_sync::no);
        });
    };

    return seastar::with_gate(_gate, [this, batch = std::move(batch)] {
        blogger.debug("Started replayAllFailedBatches (cpu {})", this_shard_id());

        typedef ::shared_ptr<cql3::untyped_result_set> page_ptr;
        sstring query = format("SELECT id, data, written_at, version FROM {}.{} LIMIT {:d}", system_keyspace::NAME, system_keyspace::BATCHLOG, page_size);
        return _qp.execute_internal(query, cql3::query_processor::cache_internal::yes).then([this, batch = std::move(batch)](page_ptr page) {
            return do_with(std::move(page), [this, batch = std::move(batch)](page_ptr & page) mutable {
                return repeat([this, &page, batch = std::move(batch)]() mutable {
                    if (page->empty()) {
                        return make_ready_future<stop_iteration>(stop_iteration::yes);
                    }
                    auto id = page->back().get_as<utils::UUID>("id");
                    return parallel_for_each(*page, batch).then([this, &page, id]() {
                        if (page->size() < page_size) {
                            return make_ready_future<stop_iteration>(stop_iteration::yes); // we've exhausted the batchlog, next query would be empty.
                        }
                        sstring query = format("SELECT id, data, written_at, version FROM {}.{} WHERE token(id) > token(?) LIMIT {:d}",
                                system_keyspace::NAME,
                                system_keyspace::BATCHLOG,
                                page_size);
                        return _qp.execute_internal(query, {id}, cql3::query_processor::cache_internal::yes).then([&page](auto res) {
                                    page = std::move(res);
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                });
                    });
                });
            });
        }).then([this] {
        // TODO FIXME : cleanup()
#if 0
            ColumnFamilyStore cfs = Keyspace.open(SystemKeyspace.NAME).getColumnFamilyStore(SystemKeyspace.BATCHLOG);
            cfs.forceBlockingFlush();
            Collection<Descriptor> descriptors = new ArrayList<>();
            for (SSTableReader sstr : cfs.getSSTables())
            descriptors.add(sstr.descriptor);
            if (!descriptors.isEmpty()) // don't pollute the logs if there is nothing to compact.
            CompactionManager.instance.submitUserDefined(cfs, descriptors, Integer.MAX_VALUE).get();

#endif

        }).then([this] {
            blogger.debug("Finished replayAllFailedBatches");
        });
    });
}

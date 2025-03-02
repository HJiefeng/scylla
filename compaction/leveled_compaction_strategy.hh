/*
 * Copyright (C) 2017-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <optional>
#include <vector>
#include <map>
#include <memory>

#include <seastar/core/sstring.hh>

#include "compaction_strategy_type.hh"
#include "size_tiered_compaction_strategy.hh"
#include "compaction_strategy_impl.hh"
#include "compaction_backlog_manager.hh"
#include "sstables/shared_sstable.hh"

class leveled_manifest;

namespace sstables {

class sstable_set_impl;

class leveled_compaction_strategy : public compaction_strategy_impl {
    static constexpr int32_t DEFAULT_MAX_SSTABLE_SIZE_IN_MB = 160;
    const sstring SSTABLE_SIZE_OPTION = "sstable_size_in_mb";

    int32_t _max_sstable_size_in_mb = DEFAULT_MAX_SSTABLE_SIZE_IN_MB;
    std::optional<std::vector<std::optional<dht::decorated_key>>> _last_compacted_keys;
    std::vector<int> _compaction_counter;
    size_tiered_compaction_strategy_options _stcs_options;
    compaction_backlog_tracker _backlog_tracker;
    int32_t calculate_max_sstable_size_in_mb(std::optional<sstring> option_value) const;
public:
    static unsigned ideal_level_for_input(const std::vector<sstables::shared_sstable>& input, uint64_t max_sstable_size);

    leveled_compaction_strategy(const std::map<sstring, sstring>& options);
    virtual compaction_descriptor get_sstables_for_compaction(table_state& table_s, strategy_control& control, std::vector<sstables::shared_sstable> candidates) override;

    virtual std::vector<compaction_descriptor> get_cleanup_compaction_jobs(table_state& table_s, std::vector<shared_sstable> candidates) const override;

    virtual compaction_descriptor get_major_compaction_job(table_state& table_s, std::vector<sstables::shared_sstable> candidates) override;

    virtual void notify_completion(const std::vector<shared_sstable>& removed, const std::vector<shared_sstable>& added) override;

    // for each level > 0, get newest sstable and use its last key as last
    // compacted key for the previous level.
    void generate_last_compacted_keys(leveled_manifest& manifest);

    virtual int64_t estimated_pending_compactions(table_state& table_s) const override;

    virtual bool parallel_compaction() const override {
        return false;
    }

    virtual compaction_strategy_type type() const override {
        return compaction_strategy_type::leveled;
    }
    virtual std::unique_ptr<sstable_set_impl> make_sstable_set(schema_ptr schema) const override;

    virtual compaction_backlog_tracker& get_backlog_tracker() override {
        return _backlog_tracker;
    }

    virtual compaction_descriptor get_reshaping_job(std::vector<shared_sstable> input, schema_ptr schema, const ::io_priority_class& iop, reshape_mode mode) override;
};

}

/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include "cql3/attributes.hh"
#include "cql3/column_identifier.hh"

namespace cql3 {

std::unique_ptr<attributes> attributes::none() {
    return std::unique_ptr<attributes>{new attributes{{}, {}, {}}};
}

attributes::attributes(std::optional<cql3::expr::expression>&& timestamp,
                       std::optional<cql3::expr::expression>&& time_to_live,
                       std::optional<cql3::expr::expression>&& timeout)
    : _timestamp{std::move(timestamp)}
    , _time_to_live{std::move(time_to_live)}
    , _timeout{std::move(timeout)}
{ }

bool attributes::is_timestamp_set() const {
    return bool(_timestamp);
}

bool attributes::is_time_to_live_set() const {
    return bool(_time_to_live);
}

bool attributes::is_timeout_set() const {
    return bool(_timeout);
}

int64_t attributes::get_timestamp(int64_t now, const query_options& options) {
    if (!_timestamp.has_value()) {
        return now;
    }

    expr::constant tval = expr::evaluate(*_timestamp, options);
    if (tval.is_null()) {
        throw exceptions::invalid_request_exception("Invalid null value of timestamp");
    }
    if (tval.is_unset_value()) {
        return now;
    }
    try {
        return tval.view().validate_and_deserialize<int64_t>(*long_type, cql_serialization_format::internal());
    } catch (marshal_exception& e) {
        throw exceptions::invalid_request_exception("Invalid timestamp value");
    }
}

int32_t attributes::get_time_to_live(const query_options& options) {
    if (!_time_to_live.has_value())
        return 0;

    expr::constant tval = expr::evaluate(*_time_to_live, options);
    if (tval.is_null()) {
        throw exceptions::invalid_request_exception("Invalid null value of TTL");
    }
    if (tval.is_unset_value()) {
        return 0;
    }

    int32_t ttl;
    try {
        ttl = tval.view().validate_and_deserialize<int32_t>(*int32_type, cql_serialization_format::internal());
    }
    catch (marshal_exception& e) {
        throw exceptions::invalid_request_exception("Invalid TTL value");
    }

    if (ttl < 0) {
        throw exceptions::invalid_request_exception("A TTL must be greater or equal to 0");
    }

    if (ttl > max_ttl.count()) {
        throw exceptions::invalid_request_exception("ttl is too large. requested (" + std::to_string(ttl) +
            ") maximum (" + std::to_string(max_ttl.count()) + ")");
    }

    return ttl;
}


db::timeout_clock::duration attributes::get_timeout(const query_options& options) const {
    expr::constant timeout = expr::evaluate(*_timeout, options);
    if (timeout.is_null() || timeout.is_unset_value()) {
        throw exceptions::invalid_request_exception("Timeout value cannot be unset/null");
    }
    cql_duration duration = timeout.view().deserialize<cql_duration>(*duration_type);
    if (duration.months || duration.days) {
        throw exceptions::invalid_request_exception("Timeout values cannot be expressed in days/months");
    }
    if (duration.nanoseconds % 1'000'000 != 0) {
        throw exceptions::invalid_request_exception("Timeout values cannot have granularity finer than milliseconds");
    }
    if (duration.nanoseconds < 0) {
        throw exceptions::invalid_request_exception("Timeout values must be non-negative");
    }
    return std::chrono::duration_cast<db::timeout_clock::duration>(std::chrono::nanoseconds(duration.nanoseconds));
}

void attributes::fill_prepare_context(prepare_context& ctx) {
    if (_timestamp.has_value()) {
        expr::fill_prepare_context(*_timestamp, ctx);
    }
    if (_time_to_live.has_value()) {
        expr::fill_prepare_context(*_time_to_live, ctx);
    }
    if (_timeout.has_value()) {
        expr::fill_prepare_context(*_timeout, ctx);
    }
}

std::unique_ptr<attributes> attributes::raw::prepare(data_dictionary::database db, const sstring& ks_name, const sstring& cf_name) const {
    std::optional<expr::expression> ts, ttl, to;

    if (timestamp.has_value()) {
        ts = prepare_expression(*timestamp, db, ks_name, nullptr, timestamp_receiver(ks_name, cf_name));
    }

    if (time_to_live.has_value()) {
        ttl = prepare_expression(*time_to_live, db, ks_name, nullptr, time_to_live_receiver(ks_name, cf_name));
    }

    if (timeout.has_value()) {
        to = prepare_expression(*timeout, db, ks_name, nullptr, timeout_receiver(ks_name, cf_name));
    }

    return std::unique_ptr<attributes>{new attributes{std::move(ts), std::move(ttl), std::move(to)}};
}

lw_shared_ptr<column_specification> attributes::raw::timestamp_receiver(const sstring& ks_name, const sstring& cf_name) const {
    return make_lw_shared<column_specification>(ks_name, cf_name, ::make_shared<column_identifier>("[timestamp]", true), data_type_for<int64_t>());
}

lw_shared_ptr<column_specification> attributes::raw::time_to_live_receiver(const sstring& ks_name, const sstring& cf_name) const {
    return make_lw_shared<column_specification>(ks_name, cf_name, ::make_shared<column_identifier>("[ttl]", true), data_type_for<int32_t>());
}

lw_shared_ptr<column_specification> attributes::raw::timeout_receiver(const sstring& ks_name, const sstring& cf_name) const {
    return make_lw_shared<column_specification>(ks_name, cf_name, ::make_shared<column_identifier>("[timeout]", true), duration_type);
}

}

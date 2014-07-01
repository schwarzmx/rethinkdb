// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <map>
#include <string>

#include "clustering/administration/main/ports.hpp"
#include "clustering/administration/suggester.hpp"
#include "containers/wire_string.hpp"
#include "rdb_protocol/meta_utils.hpp"
#include "rdb_protocol/op.hpp"
#include "rpc/directory/read_manager.hpp"

namespace ql {

durability_requirement_t parse_durability_optarg(counted_t<val_t> arg,
                                                 const pb_rcheckable_t *target);

name_string_t get_name(counted_t<val_t> val, const term_t *caller,
        const char *type_str) {
    r_sanity_check(val.has());
    const wire_string_t &raw_name = val->as_str();
    name_string_t name;
    bool assignment_successful = name.assign_value(raw_name);
    rcheck_target(caller, base_exc_t::GENERIC, assignment_successful,
                  strprintf("%s name `%s` invalid (%s).",
                            type_str, raw_name.c_str(), name_string_t::valid_char_msg));
    return name;
}

// Meta operations (BUT NOT TABLE TERMS) should inherit from this.  It will
// handle a lot of the nasty semilattice initialization stuff for them,
// including the thread switching.
class meta_op_term_t : public op_term_t {
public:
    meta_op_term_t(compile_env_t *env, protob_t<const Term> term, argspec_t argspec,
              optargspec_t optargspec = optargspec_t({}))
        : op_term_t(env, std::move(term), std::move(argspec), std::move(optargspec)) { }

private:
    bool op_is_deterministic() const FINAL { return false; }
    // op_term_t's op_is_blocking is overridden in all the subclasses.
};

// If you don't have to modify any of the data, use
// `const_rethreading_metadata_accessor_t` instead which is more efficient.
struct rethreading_metadata_accessor_t : public on_thread_t {
    explicit rethreading_metadata_accessor_t(scope_env_t *env)
    : on_thread_t(env->env->cluster_metadata()->home_thread()),
      metadata(env->env->cluster_metadata()->get()),
      ns_change(&metadata.rdb_namespaces),
      ns_searcher(&ns_change.get()->namespaces),
      db_searcher(&metadata.databases.databases),
      dc_searcher(&metadata.datacenters.datacenters)
    { }
    cluster_semilattice_metadata_t metadata;
    cow_ptr_t<namespaces_semilattice_metadata_t>::change_t ns_change;
    metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher;
    metadata_searcher_t<database_semilattice_metadata_t> db_searcher;
    metadata_searcher_t<datacenter_semilattice_metadata_t> dc_searcher;
};

struct const_rethreading_metadata_accessor_t : public on_thread_t {
    explicit const_rethreading_metadata_accessor_t(scope_env_t *env)
    : on_thread_t(env->env->cluster_metadata()->home_thread()),
      metadata(env->env->cluster_metadata()->get()),
      dc_searcher(&metadata.datacenters.datacenters)
    { }
    cluster_semilattice_metadata_t metadata;
    const_metadata_searcher_t<datacenter_semilattice_metadata_t> dc_searcher;
};


class meta_write_op_t : public meta_op_term_t {
public:
    meta_write_op_t(compile_env_t *env, protob_t<const Term> term, argspec_t argspec,
                    optargspec_t optargspec = optargspec_t({}))
        : meta_op_term_t(env, std::move(term), std::move(argspec), std::move(optargspec)) { }

protected:
    clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t, cluster_directory_metadata_t> > >
    directory_metadata(env_t *env) const {
        r_sanity_check(env->directory_read_manager()->home_thread() == get_thread_id());
        return env->directory_read_manager()->get_root_view();
    }

private:
    bool op_is_blocking() const FINAL {
        // Metadata write operations usually involve thread-switching (unless we
        // happen to be on the same thread).
        return true;
    }

    virtual std::string write_eval_impl(scope_env_t *env,
                                        args_t *args,
                                        eval_flags_t flags) const = 0;
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t flags) const FINAL {
        std::string op = write_eval_impl(env, args, flags);
        datum_ptr_t res(datum_t::R_OBJECT);
        UNUSED bool b = res.add(op, make_counted<datum_t>(1.0));
        return new_val(res.to_counted());
    }
};

class db_term_t : public meta_op_term_t {
public:
    db_term_t(compile_env_t *env, const protob_t<const Term> &term) : meta_op_term_t(env, term, argspec_t(1)) { }
private:
    bool op_is_blocking() const FINAL {
        return false;
    }

    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        name_string_t db_name = get_name(args->arg(env, 0), this, "Database");
        uuid_u uuid;
        {
            databases_semilattice_metadata_t db_metadata;
            env->env->get_databases_metadata(&db_metadata);
            const_metadata_searcher_t<database_semilattice_metadata_t>
                db_searcher(&db_metadata.databases);

            uuid = meta_get_uuid(&db_searcher, db_name,
                                 strprintf("Database `%s` does not exist.",
                                           db_name.c_str()), this);
        }
        return new_val(make_counted<const db_t>(uuid, db_name.str()));
    }
    const char *name() const FINAL { return "db"; }
};

class db_create_term_t : public meta_write_op_t {
public:
    db_create_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_write_op_t(env, term, argspec_t(1)) { }
private:
    virtual std::string write_eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        name_string_t db_name = get_name(args->arg(env, 0), this, "Database");

        rethreading_metadata_accessor_t meta(env);

        // Ensure database doesn't already exist.
        metadata_search_status_t status;
        meta.db_searcher.find_uniq(db_name, &status);
        rcheck(status == METADATA_ERR_NONE,
               base_exc_t::GENERIC,
               strprintf("Database `%s` already exists.", db_name.c_str()));

        // Create database, insert into metadata, then join into real metadata.
        database_semilattice_metadata_t db;
        db.name = vclock_t<name_string_t>(db_name, env->env->this_machine());
        meta.metadata.databases.databases.insert(
            std::make_pair(generate_uuid(), make_deletable(db)));
        try {
            fill_in_blueprints(&meta.metadata,
                               directory_metadata(env->env)->get().get_inner(),
                               env->env->this_machine(),
                               boost::optional<namespace_id_t>());
        } catch (const missing_machine_exc_t &e) {
            rfail(base_exc_t::GENERIC, "%s", e.what());
        }
        env->env->join_and_wait_to_propagate(meta.metadata);

        return "created";
    }
    virtual const char *name() const { return "db_create"; }
};

bool is_hard(durability_requirement_t requirement) {
    switch (requirement) {
    case DURABILITY_REQUIREMENT_DEFAULT:
    case DURABILITY_REQUIREMENT_HARD:
        return true;
    case DURABILITY_REQUIREMENT_SOFT:
        return false;
    default:
        unreachable();
    }
}

class table_create_term_t : public meta_write_op_t {
public:
    table_create_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_write_op_t(env, term, argspec_t(1, 2),
                        optargspec_t({"datacenter", "primary_key", "durability"})) { }
private:
    virtual std::string write_eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        uuid_u dc_id = nil_uuid();
        if (counted_t<val_t> v = args->optarg(env, "datacenter")) {
            name_string_t name = get_name(v, this, "Table");
            {
                const_rethreading_metadata_accessor_t meta(env);
                dc_id = meta_get_uuid(&meta.dc_searcher, name,
                                      strprintf("Datacenter `%s` does not exist.",
                                                name.str().c_str()),
                                      this);
            }
        }

        const bool hard_durability
            = is_hard(parse_durability_optarg(args->optarg(env, "durability"), this));

        std::string primary_key = "id";
        if (counted_t<val_t> v = args->optarg(env, "primary_key")) {
            primary_key = v->as_str().to_std();
        }

        uuid_u db_id;
        std::string db_name;
        name_string_t tbl_name;
        if (args->num_args() == 1) {
            counted_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv);
            db_name = dbv->as_db()->name;
            db_id = dbv->as_db()->id;
            tbl_name = get_name(args->arg(env, 0), this, "Table");
        } else {
            auto db = args->arg(env, 0)->as_db();
            db_name = db->name;
            db_id = db->id;
            tbl_name = get_name(args->arg(env, 1), this, "Table");
        }

        // Ensure table doesn't already exist.
        metadata_search_status_t status;
        namespace_predicate_t pred(&tbl_name, &db_id);

        const uuid_u namespace_id = generate_uuid();

        {
            rethreading_metadata_accessor_t meta(env);
            meta.ns_searcher.find_uniq(pred, &status);
            rcheck(status == METADATA_ERR_NONE,
                   base_exc_t::GENERIC,
                   strprintf("Table `%s` already exists.",
                             (db_name + "." + tbl_name.c_str()).c_str()));

            // Create namespace (DB + table pair) and insert into metadata.
            namespace_semilattice_metadata_t ns =
                new_namespace(
                        env->env->this_machine(), db_id, dc_id, tbl_name,
                        primary_key);

            // Set Durability
            std::map<datacenter_id_t, ack_expectation_t> *ack_map =
                &ns.ack_expectations.get_mutable();
            for (auto it = ack_map->begin(); it != ack_map->end(); ++it) {
                it->second = ack_expectation_t(
                    it->second.expectation(), hard_durability);
            }
            ns.ack_expectations.upgrade_version(env->env->this_machine());

            meta.ns_change.get()->namespaces.insert(
                                                    std::make_pair(namespace_id, make_deletable(ns)));
            try {
                fill_in_blueprints(&meta.metadata,
                                   directory_metadata(env->env)->get().get_inner(),
                                   env->env->this_machine(),
                                   boost::optional<namespace_id_t>());
            } catch (const missing_machine_exc_t &e) {
                rfail(base_exc_t::GENERIC, "%s", e.what());
            }
            env->env->join_and_wait_to_propagate(meta.metadata);
        }

        // UGLY HACK BELOW (see wait_for_rdb_table_readiness)

        try {
            wait_for_rdb_table_readiness(env->env->ns_repo(), namespace_id,
                                         env->env->interruptor,
                                         env->env->cluster_metadata());
        } catch (const interrupted_exc_t &e) {
            rfail(base_exc_t::GENERIC, "Query interrupted, probably by user.");
        }

        return "created";
    }
    virtual const char *name() const { return "table_create"; }
};

class db_drop_term_t : public meta_write_op_t {
public:
    db_drop_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_write_op_t(env, term, argspec_t(1)) { }
private:
    virtual std::string write_eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        name_string_t db_name = get_name(args->arg(env, 0), this, "Database");

        rethreading_metadata_accessor_t meta(env);

        // Get database metadata.
        metadata_search_status_t status;
        metadata_searcher_t<database_semilattice_metadata_t>::iterator
            db_metadata = meta.db_searcher.find_uniq(db_name, &status);
        rcheck(status == METADATA_SUCCESS, base_exc_t::GENERIC,
               strprintf("Database `%s` does not exist.", db_name.c_str()));
        guarantee(!db_metadata->second.is_deleted());
        uuid_u db_id = db_metadata->first;

        // Delete all tables in database.
        namespace_predicate_t pred(&db_id);
        for (auto it = meta.ns_searcher.find_next(meta.ns_searcher.begin(), pred);
             it != meta.ns_searcher.end();
             it = meta.ns_searcher.find_next(++it, pred)) {
            guarantee(!it->second.is_deleted());
            it->second.mark_deleted();
        }

        // Delete database
        db_metadata->second.mark_deleted();

        // Join
        try {
            fill_in_blueprints(&meta.metadata,
                               directory_metadata(env->env)->get().get_inner(),
                               env->env->this_machine(),
                               boost::optional<namespace_id_t>());
        } catch (const missing_machine_exc_t &e) {
            rfail(base_exc_t::GENERIC, "%s", e.what());
        }
        env->env->join_and_wait_to_propagate(meta.metadata);

        return "dropped";
    }
    virtual const char *name() const { return "db_drop"; }
};

class table_drop_term_t : public meta_write_op_t {
public:
    table_drop_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_write_op_t(env, term, argspec_t(1, 2)) { }
private:
    virtual std::string write_eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        uuid_u db_id;
        std::string db_name;
        name_string_t tbl_name;
        if (args->num_args() == 1) {
            counted_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv);
            db_name = dbv->as_db()->name;
            db_id = dbv->as_db()->id;
            tbl_name = get_name(args->arg(env, 0), this, "Table");
        } else {
            auto db = args->arg(env, 0)->as_db();
            db_name = db->name;
            db_id = db->id;
            tbl_name = get_name(args->arg(env, 1), this, "Table");
        }

        rethreading_metadata_accessor_t meta(env);

        // Get table metadata.
        metadata_search_status_t status;
        namespace_predicate_t pred(&tbl_name, &db_id);
        metadata_searcher_t<namespace_semilattice_metadata_t>::iterator
            ns_metadata = meta.ns_searcher.find_uniq(pred, &status);
        rcheck(status == METADATA_SUCCESS, base_exc_t::GENERIC,
               strprintf("Table `%s` does not exist.",
                         (db_name + "." + tbl_name.c_str()).c_str()));
        guarantee(!ns_metadata->second.is_deleted());

        // Delete table and join.
        ns_metadata->second.mark_deleted();
        try {
            fill_in_blueprints(&meta.metadata,
                               directory_metadata(env->env)->get().get_inner(),
                               env->env->this_machine(),
                               boost::optional<namespace_id_t>());
        } catch (const missing_machine_exc_t &e) {
            rfail(base_exc_t::GENERIC, "%s", e.what());
        }
        env->env->join_and_wait_to_propagate(meta.metadata);

        return "dropped";
    }
    virtual const char *name() const { return "table_drop"; }
};

class db_list_term_t : public meta_op_term_t {
public:
    db_list_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(0)) { }
private:
    bool op_is_blocking() const FINAL {
        return false;
    }

    counted_t<val_t> eval_impl(scope_env_t *env, args_t *, eval_flags_t) const FINAL {
        std::vector<std::string> dbs;
        {
            databases_semilattice_metadata_t db_metadata;
            env->env->get_databases_metadata(&db_metadata);
            const_metadata_searcher_t<database_semilattice_metadata_t>
                db_searcher(&db_metadata.databases);

            for (auto it = db_searcher.find_next(db_searcher.begin());
                 it != db_searcher.end();
                 it = db_searcher.find_next(++it)) {
                guarantee(!it->second.is_deleted());
                if (it->second.get_ref().name.in_conflict()) {
                    continue;
                }
                dbs.push_back(it->second.get_ref().name.get().c_str());
            }
        }

        std::vector<counted_t<const datum_t> > arr;
        arr.reserve(dbs.size());
        for (auto it = dbs.begin(); it != dbs.end(); ++it) {
            arr.push_back(make_counted<datum_t>(std::move(*it)));
        }

        return new_val(make_counted<const datum_t>(std::move(arr)));
    }
    const char *name() const FINAL { return "db_list"; }
};

class table_list_term_t : public meta_op_term_t {
public:
    table_list_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(0, 1)) { }
private:
    bool op_is_blocking() const FINAL {
        return false;
    }

    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        uuid_u db_id;
        if (args->num_args() == 0) {
            counted_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv);
            db_id = dbv->as_db()->id;
        } else {
            db_id = args->arg(env, 0)->as_db()->id;
        }
        std::vector<std::string> tables;
        namespace_predicate_t pred(&db_id);
        {
            cow_ptr_t<namespaces_semilattice_metadata_t> ns_metadata
                = env->env->get_namespaces_metadata();
            const_metadata_searcher_t<namespace_semilattice_metadata_t>
                ns_searcher(&ns_metadata->namespaces);

            for (auto it = ns_searcher.find_next(ns_searcher.begin(), pred);
                 it != ns_searcher.end();
                 it = ns_searcher.find_next(++it, pred)) {
                guarantee(!it->second.is_deleted());
                if (it->second.get_ref().name.in_conflict()) continue;
                tables.push_back(it->second.get_ref().name.get().c_str());
            }
        }

        std::vector<counted_t<const datum_t> > arr;
        arr.reserve(tables.size());
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            arr.push_back(make_counted<datum_t>(std::move(*it)));
        }
        return new_val(make_counted<const datum_t>(std::move(arr)));
    }
    const char *name() const FINAL { return "table_list"; }
};

class sync_term_t : public meta_write_op_t {
public:
    sync_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : meta_write_op_t(env, term, argspec_t(1)) { }

private:
    virtual std::string write_eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> t = args->arg(env, 0)->as_table();
        bool success = t->sync(env->env, this);
        r_sanity_check(success);
        return "synced";
    }
    virtual const char *name() const { return "sync"; }
};

class table_term_t : public op_term_t {
public:
    table_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1, 2), optargspec_t({ "use_outdated" })) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        counted_t<val_t> t = args->optarg(env, "use_outdated");
        bool use_outdated = t ? t->as_bool() : false;
        counted_t<const db_t> db;
        std::string name;
        if (args->num_args() == 1) {
            counted_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv.has());
            db = dbv->as_db();
            name = args->arg(env, 0)->as_str().to_std();
        } else {
            r_sanity_check(args->num_args() == 2);
            db = args->arg(env, 0)->as_db();
            name = args->arg(env, 1)->as_str().to_std();
        }
        return new_val(make_counted<table_t>(
                           env->env, db, name, use_outdated, backtrace()));
    }
    bool op_is_deterministic() const FINAL { return false; }
    bool op_is_blocking() const FINAL {
        // We construct a datum stream that, when iterated, loads a table.  That
        // means this is a blocking operation (even if we don't block when
        // constructing the datum stream itself).
        return true;
    }
    const char *name() const FINAL { return "table"; }
};

class get_term_t : public op_term_t {
public:
    get_term_t(compile_env_t *env, const protob_t<const Term> &term) : op_term_t(env, term, argspec_t(2)) { }
private:
    virtual counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        counted_t<const datum_t> pkey = args->arg(env, 1)->as_datum();
        counted_t<const datum_t> row = table->get_row(env->env, pkey);
        return new_val(row, pkey, table);
    }
    virtual const char *name() const { return "get"; }
};

class get_all_term_t : public op_term_t {
public:
    get_all_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(2, -1), optargspec_t({ "index" })) { }
private:
    virtual counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        counted_t<val_t> index = args->optarg(env, "index");
        std::string index_str = index ? index->as_str().to_std() : "";
        if (index && index_str != table->get_pkey()) {
            std::vector<counted_t<datum_stream_t> > streams;
            for (size_t i = 1; i < args->num_args(); ++i) {
                counted_t<const datum_t> key = args->arg(env, i)->as_datum();
                counted_t<datum_stream_t> seq =
                    table->get_all(env->env, key, index_str, backtrace());
                streams.push_back(seq);
            }
            counted_t<datum_stream_t> stream
                = make_counted<union_datum_stream_t>(std::move(streams), backtrace());
            return new_val(stream, table);
        } else {
            datum_ptr_t arr(datum_t::R_ARRAY);
            for (size_t i = 1; i < args->num_args(); ++i) {
                counted_t<const datum_t> key = args->arg(env, i)->as_datum();
                counted_t<const datum_t> row = table->get_row(env->env, key);
                if (row->get_type() != datum_t::R_NULL) {
                    arr.add(row);
                }
            }
            counted_t<datum_stream_t> stream
                = make_counted<array_datum_stream_t>(arr.to_counted(), backtrace());
            return new_val(stream, table);
        }
    }
    virtual const char *name() const { return "get_all"; }
};

counted_t<term_t> make_db_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_term_t>(env, term);
}

counted_t<term_t> make_table_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_term_t>(env, term);
}

counted_t<term_t> make_get_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<get_term_t>(env, term);
}

counted_t<term_t> make_get_all_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<get_all_term_t>(env, term);
}

counted_t<term_t> make_db_create_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_create_term_t>(env, term);
}

counted_t<term_t> make_db_drop_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_drop_term_t>(env, term);
}

counted_t<term_t> make_db_list_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_list_term_t>(env, term);
}

counted_t<term_t> make_table_create_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_create_term_t>(env, term);
}

counted_t<term_t> make_table_drop_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_drop_term_t>(env, term);
}

counted_t<term_t> make_table_list_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_list_term_t>(env, term);
}

counted_t<term_t> make_sync_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<sync_term_t>(env, term);
}



} // namespace ql

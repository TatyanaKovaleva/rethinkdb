// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/tables/db_config.hpp"

#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/metadata.hpp"
#include "clustering/administration/real_reql_cluster_interface.hpp"
#include "concurrency/cross_thread_signal.hpp"

ql::datum_t convert_db_config_and_name_to_datum(
        name_string_t db_name,
        namespace_id_t id) {
    ql::datum_object_builder_t builder;
    builder.overwrite("name", convert_name_to_datum(db_name));
    builder.overwrite("id", convert_uuid_to_datum(id));
    return std::move(builder).to_datum();
}

bool convert_db_config_and_name_from_datum(
        ql::datum_t datum,
        name_string_t *db_name_out,
        namespace_id_t *id_out,
        admin_err_t *error_out) {
    /* In practice, the input will always be an object and the `id` field will always
    be valid, because `artificial_table_t` will check those thing before passing the
    row to `db_config_artificial_table_backend_t`. But we check them anyway for
    consistency. */
    converter_from_datum_object_t converter;
    if (!converter.init(datum, error_out)) {
        return false;
    }

    ql::datum_t name_datum;
    if (!converter.get("name", &name_datum, error_out)) {
        return false;
    }
    if (!convert_name_from_datum(name_datum, "db name", db_name_out, error_out)) {
        error_out->msg = "In `name`: " + error_out->msg;
        return false;
    }

    ql::datum_t id_datum;
    if (!converter.get("id", &id_datum, error_out)) {
        return false;
    }
    if (!convert_uuid_from_datum(id_datum, id_out, error_out)) {
        error_out->msg = "In `id`: " + error_out->msg;
        return false;
    }

    if (!converter.check_no_extra_keys(error_out)) {
        return false;
    }

    return true;
}

db_config_artificial_table_backend_t::db_config_artificial_table_backend_t(
        boost::shared_ptr< semilattice_readwrite_view_t<
        databases_semilattice_metadata_t> > _database_sl_view,
        real_reql_cluster_interface_t *_reql_cluster_interface) :
    database_sl_view(_database_sl_view),
    subs([this]() { notify_all(); }, database_sl_view),
    reql_cluster_interface(_reql_cluster_interface)
    { }

db_config_artificial_table_backend_t::~db_config_artificial_table_backend_t() {
    begin_changefeed_destruction();
}

std::string db_config_artificial_table_backend_t::get_primary_key_name() {
    return "id";
}

bool db_config_artificial_table_backend_t::read_all_rows_as_vector(
        UNUSED signal_t *interruptor_on_caller,
        std::vector<ql::datum_t> *rows_out,
        UNUSED admin_err_t *error_out) {
    on_thread_t thread_switcher(home_thread());
    rows_out->clear();
    databases_semilattice_metadata_t md = database_sl_view->get();
    for (auto it = md.databases.begin();
              it != md.databases.end();
            ++it) {
        if (it->second.is_deleted()) {
            continue;
        }

        name_string_t db_name = it->second.get_ref().name.get_ref();
        rows_out->push_back(convert_db_config_and_name_to_datum(db_name, it->first));
    }
    return true;
}

bool db_config_artificial_table_backend_t::read_row(
        ql::datum_t primary_key,
        UNUSED signal_t *interruptor_on_caller,
        ql::datum_t *row_out,
        UNUSED admin_err_t *error_out) {
    on_thread_t thread_switcher(home_thread());
    databases_semilattice_metadata_t md = database_sl_view->get();
    database_id_t database_id;
    admin_err_t dummy_error;
    if (!convert_uuid_from_datum(primary_key, &database_id, &dummy_error)) {
        /* If the primary key was not a valid UUID, then it must refer to a nonexistent
        row. */
        database_id = nil_uuid();
    }
    std::map<database_id_t, deletable_t<database_semilattice_metadata_t> >::iterator it;
    if (search_metadata_by_uuid(&md.databases, database_id, &it)) {
        name_string_t db_name = it->second.get_ref().name.get_ref();
        *row_out = convert_db_config_and_name_to_datum(db_name, database_id);
    } else {
        *row_out = ql::datum_t();
    }
    return true;
}

bool db_config_artificial_table_backend_t::write_row(
        ql::datum_t primary_key,
        bool pkey_was_autogenerated,
        ql::datum_t *new_value_inout,
        signal_t *interruptor_on_caller,
        admin_err_t *error_out) {
    cross_thread_signal_t interruptor_on_home_thread(interruptor_on_caller, home_thread());
    on_thread_t thread_switcher(home_thread());
    databases_semilattice_metadata_t md = database_sl_view->get();

    /* Look for an existing DB with the given UUID */
    namespace_id_t database_id;
    admin_err_t dummy_error;
    if (!convert_uuid_from_datum(primary_key, &database_id, &dummy_error)) {
        /* If the primary key was not a valid UUID, then it must refer to a nonexistent
        row. */
        database_id = nil_uuid();
    }
    std::map<database_id_t, deletable_t<database_semilattice_metadata_t> >::iterator it;
    bool existed_before = search_metadata_by_uuid(&md.databases, database_id, &it);

    if (new_value_inout->has()) {
        /* We're updating an existing database (if `existed_before == true`) or creating
        a new one (if `existed_before == false`) */

        /* Parse the new value the user provided for the database */
        name_string_t new_db_name;
        namespace_id_t new_database_id;
        if (!convert_db_config_and_name_from_datum(*new_value_inout,
                &new_db_name, &new_database_id, error_out)) {
            error_out->msg = "The change you're trying to make to "
                "`rethinkdb.db_config` has the wrong format. " + error_out->msg;
            return false;
        }
        guarantee(new_database_id == database_id, "artificial_table_t should ensure "
            "that the primary key doesn't change.");

        if (existed_before) {
            guarantee(!pkey_was_autogenerated, "UUID collision happened");
        } else {
            if (!pkey_was_autogenerated) {
                *error_out = admin_err_t{
                    "If you want to create a new table by inserting into "
                    "`rethinkdb.db_config`, you must use an auto-generated primary key.",
                    query_state_t::FAILED};
                return false;
            }
            /* Assert that we didn't randomly generate the UUID of a database that used
            to exist but was deleted */
            guarantee(md.databases.count(database_id) == 0, "UUID collision happened");
        }

        name_string_t old_db_name;
        if (existed_before) {
            old_db_name = it->second.get_ref().name.get_ref();
        }

        if (!existed_before || new_db_name != old_db_name) {
            /* Reserve the `rethinkdb` database name */
            if (new_db_name == name_string_t::guarantee_valid("rethinkdb")) {
                if (!existed_before) {
                    *error_out = admin_err_t{
                        "Database `rethinkdb` already exists.",
                        query_state_t::FAILED};
                } else {
                    *error_out = admin_err_t{
                        strprintf("Cannot rename database `%s` to `rethinkdb` "
                                  "because database `rethinkdb` already exists.",
                                  old_db_name.c_str()),
                        query_state_t::FAILED};
                }
                return false;
            }
            /* Prevent name collisions if possible */
            for (const auto &pair : md.databases) {
                if (!pair.second.is_deleted() &&
                        pair.second.get_ref().name.get_ref() == new_db_name) {
                    if (!existed_before) {
                        /* This message looks weird in the context of the variable named
                        `existed_before`, but it's correct. `existed_before` is true if a
                        database with the specified UUID already exists; but we're
                        showing the user an error if a database with the specified name
                        already exists. */
                        *error_out = admin_err_t{
                            strprintf("Database `%s` already exists.",
                                      new_db_name.c_str()),
                            query_state_t::FAILED};
                    } else {
                        *error_out = admin_err_t{
                            strprintf("Cannot rename database `%s` to `%s` "
                                      "because database `%s` already exists.",
                                      old_db_name.c_str(),
                                      new_db_name.c_str(),
                                      new_db_name.c_str()),
                            query_state_t::FAILED};
                    }
                    return false;
                }
            }
        }

        /* Update `md`. The change will be committed to the semilattices at the end of
        this function. */
        if (existed_before) {
            it->second.get_mutable()->name.set(new_db_name);
        } else {
            database_semilattice_metadata_t db_md;
            db_md.name = versioned_t<name_string_t>(new_db_name);
            md.databases[database_id] =
                deletable_t<database_semilattice_metadata_t>(db_md);
        }
    } else {
        /* We're deleting a database (or it was already deleted) */
        if (existed_before) {
            guarantee(!pkey_was_autogenerated, "UUID collision happened");

            // `db_drop_uuid` asserts we're on its home thread
            // https://github.com/rethinkdb/rethinkdb/issues/5598
            reql_cluster_interface->db_drop_uuid(
                auth::user_context_t(auth::permissions_t(false, false, true, false)),
                database_id,
                it->second.get_ref().name.get_ref(),
                &interruptor_on_home_thread,
                nullptr,
                error_out);
        }
    }

    database_sl_view->join(md);

    return true;
}


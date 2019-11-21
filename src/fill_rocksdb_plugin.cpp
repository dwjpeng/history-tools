// copyright defined in LICENSE.txt

#include "fill_rocksdb_plugin.hpp"
#include "../wasms/state_history_kv_tables.hpp" // todo: move
#include "state_history_connection.hpp"
#include "state_history_kv.hpp"
#include "state_history_rocksdb.hpp"
#include "util.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <fc/exception/exception.hpp>

using namespace appbase;
using namespace std::literals;
using namespace state_history;

namespace asio      = boost::asio;
namespace bpo       = boost::program_options;
namespace kv        = state_history::kv;
namespace rdb       = state_history::rdb;
namespace websocket = boost::beast::websocket;

using asio::ip::tcp;
using boost::beast::flat_buffer;
using boost::system::error_code;

using abieos::abi_type;
using abieos::checksum256;
using abieos::input_buffer;

struct flm_session;

struct fill_rocksdb_config : connection_config {
    uint32_t                skip_to     = 0;
    uint32_t                stop_before = 0;
    std::vector<trx_filter> trx_filters = {};
};

struct fill_rocksdb_plugin_impl : std::enable_shared_from_this<fill_rocksdb_plugin_impl> {
    std::shared_ptr<fill_rocksdb_config> config = std::make_shared<fill_rocksdb_config>();
    std::shared_ptr<::flm_session>       session;
    boost::asio::deadline_timer          timer;

    fill_rocksdb_plugin_impl()
        : timer(app().get_io_service()) {}

    ~fill_rocksdb_plugin_impl();

    void schedule_retry() {
        timer.expires_from_now(boost::posix_time::seconds(1));
        timer.async_wait([this](auto&) {
            ilog("retry...");
            start();
        });
    }

    void start();
};

struct flm_session : connection_callbacks, std::enable_shared_from_this<flm_session> {
    fill_rocksdb_plugin_impl*                     my = nullptr;
    std::shared_ptr<fill_rocksdb_config>          config;
    std::shared_ptr<state_history::rdb::database> db   = app().find_plugin<rocksdb_plugin>()->get_db();
    state_history::rdb::db_view                   view = {*db};
    std::shared_ptr<state_history::connection>    connection;
    std::optional<state_history::fill_status_v0>  current_db_status = {};
    uint32_t                                      head              = 0;
    abieos::checksum256                           head_id           = {};
    uint32_t                                      irreversible      = 0;
    abieos::checksum256                           irreversible_id   = {};
    uint32_t                                      first             = 0;

    flm_session(fill_rocksdb_plugin_impl* my)
        : my(my)
        , config(my->config) {}

    void connect(asio::io_context& ioc) {
        connection = std::make_shared<state_history::connection>(ioc, *config, shared_from_this());
        connection->connect();
    }

    void received_abi() override {
        load_fill_status();
        ilog("clean up stale records");
        end_write(true);
        // truncate(head + 1);
        end_write(true);
        db->flush(true, true);

        ilog("request status");
        connection->send(get_status_request_v0{});
    }

    bool received(get_status_result_v0& status, eosio::input_stream bin) override {
        ilog("request blocks");
        connection->request_blocks(status, std::max(config->skip_to, head + 1), get_positions());
        return true;
    }

    void load_fill_status() {
        rdb::db_view_state view_state{view};
        fill_status_kv     table{{view_state}};
        auto               it = table.begin();
        if (it == table.end())
            return;
        current_db_status = std::get<0>(it.get());
        if (!current_db_status)
            return;
        head            = current_db_status->head;
        head_id         = current_db_status->head_id;
        irreversible    = current_db_status->irreversible;
        irreversible_id = current_db_status->irreversible_id;
        first           = current_db_status->first;
    }

    std::vector<block_position> get_positions() {
        std::vector<block_position> result;
        /*
        if (head) {
            for (uint32_t i = irreversible; i <= head; ++i) {
                auto rb = rdb::get<kv::received_block>(*db, kv::make_received_block_key(i), true);
                result.push_back({rb->block_num, rb->block_id});
            }
        }
        */
        return result;
    }

    void write_fill_status() {
        if (irreversible < head)
            current_db_status = state_history::fill_status_v0{
                .head = head, .head_id = head_id, .irreversible = irreversible, .irreversible_id = irreversible_id, .first = first};
        else
            current_db_status = state_history::fill_status_v0{
                .head = head, .head_id = head_id, .irreversible = head, .irreversible_id = head_id, .first = first};

        rdb::db_view_state view_state{view};
        fill_status_kv     table{{view_state}};
        table.insert(*current_db_status);
    }

    void end_write(bool write_fill) {
        if (write_fill)
            write_fill_status();
        view.write_changes();
    }

    bool received(get_blocks_result_v0& result, eosio::input_stream bin) override {
        if (!result.this_block)
            return true;
        if (config->stop_before && result.this_block->block_num >= config->stop_before) {
            ilog("block ${b}: stop requested", ("b", result.this_block->block_num));
            end_write(true);
            db->flush(false, false);
            return false;
        }

        try {
            if (result.this_block->block_num <= head) {
                ilog("switch forks at block ${b}", ("b", result.this_block->block_num));
                throw std::runtime_error("truncate not implemented");
            }

            bool near       = result.this_block->block_num + 4 >= result.last_irreversible.block_num;
            bool commit_now = !(result.this_block->block_num % 200) || near;
            if (commit_now)
                ilog("block ${b}", ("b", result.this_block->block_num));

            if (head_id != abieos::checksum256{} && (!result.prev_block || result.prev_block->block_id != head_id))
                throw std::runtime_error("prev_block does not match");
            if (result.deltas)
                receive_deltas(result.this_block->block_num, *result.deltas);

            head            = result.this_block->block_num;
            head_id         = result.this_block->block_id;
            irreversible    = result.last_irreversible.block_num;
            irreversible_id = result.last_irreversible.block_id;
            if (!first)
                first = head;

            // rdb::put(
            //     active_content_batch, kv::make_received_block_key(result.this_block->block_num),
            //     kv::received_block{result.this_block->block_num, result.this_block->block_id});

            if (commit_now) {
                end_write(true);
            }
            if (near)
                db->flush(false, false);
        } catch (...) {
            throw;
        }

        return true;
    } // receive_result()

    void receive_deltas(uint32_t block_num, eosio::input_stream bin) {
        rdb::db_view_state view_state{view};
        uint32_t           num;
        eosio::check_discard(eosio::varuint32_from_bin(num, bin));
        for (uint32_t i = 0; i < num; ++i) {
            state_history::table_delta delta;
            eosio::check_discard(from_bin(delta, bin));
            auto&  delta_v0      = std::get<0>(delta);
            size_t num_processed = 0;
            store_delta({view_state}, delta_v0, head == 0, [&]() {
                if (delta_v0.rows.size() > 10000 && !(num_processed % 10000)) {
                    ilog(
                        "block ${b} ${t} ${n} of ${r}",
                        ("b", block_num)("t", delta_v0.name)("n", num_processed)("r", delta_v0.rows.size()));
                    if (head == 0)
                        end_write(false);
                }
                ++num_processed;
            });
        }
    } // receive_deltas

    const abi_type& get_type(const std::string& name) { return connection->get_type(name); }

    void closed(bool retry) override {
        if (my) {
            my->session.reset();
            if (retry)
                my->schedule_retry();
        }
    }

    ~flm_session() {}
}; // flm_session

static abstract_plugin& _fill_rocksdb_plugin = app().register_plugin<fill_rocksdb_plugin>();

fill_rocksdb_plugin_impl::~fill_rocksdb_plugin_impl() {
    if (session)
        session->my = nullptr;
}

void fill_rocksdb_plugin_impl::start() {
    session = std::make_shared<flm_session>(this);
    session->connect(app().get_io_service());
}

fill_rocksdb_plugin::fill_rocksdb_plugin()
    : my(std::make_shared<fill_rocksdb_plugin_impl>()) {}

fill_rocksdb_plugin::~fill_rocksdb_plugin() {}

void fill_rocksdb_plugin::set_program_options(options_description& cli, options_description& cfg) { auto clop = cli.add_options(); }

void fill_rocksdb_plugin::plugin_initialize(const variables_map& options) {
    try {
        auto endpoint = options.at("fill-connect-to").as<std::string>();
        if (endpoint.find(':') == std::string::npos)
            throw std::runtime_error("invalid endpoint: " + endpoint);

        auto port               = endpoint.substr(endpoint.find(':') + 1, endpoint.size());
        auto host               = endpoint.substr(0, endpoint.find(':'));
        my->config->host        = host;
        my->config->port        = port;
        my->config->skip_to     = options.count("fill-skip-to") ? options["fill-skip-to"].as<uint32_t>() : 0;
        my->config->stop_before = options.count("fill-stop") ? options["fill-stop"].as<uint32_t>() : 0;
        my->config->trx_filters = fill_plugin::get_trx_filters(options);
    }
    FC_LOG_AND_RETHROW()
}

void fill_rocksdb_plugin::plugin_startup() { my->start(); }

void fill_rocksdb_plugin::plugin_shutdown() {
    if (my->session)
        my->session->connection->close(false);
    my->timer.cancel();
    ilog("fill_rocksdb_plugin stopped");
}

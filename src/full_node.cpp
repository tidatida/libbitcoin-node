/*
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-node.
 *
 * libbitcoin-node is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/full_node.hpp>

#include <future>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/node/full_node.hpp>
#include <bitcoin/node/logging.hpp>

// Localizable messages.

// Session errors.
#define BN_CONNECTION_START_ERROR \
    "Error starting connection : %1%"
#define BN_SESSION_START_ERROR \
    "Error starting session : %1%"
#define BN_SESSION_STOP_ERROR \
    "Error stopping session : %1%"

// Transaction successes.
#define BN_TX_ACCEPTED \
    "Accepted transaction into memory pool [%1%]"
#define BN_TX_ACCEPTED_WITH_INPUTS \
    "Accepted transaction into memory pool [%1%] with unconfirmed inputs (%2%)"
#define BN_TX_CONFIRMED \
    "Confirmed transaction into blockchain [%1%]"

// Transaction errors.
#define BN_TX_ACCEPT_ERROR \
    "Error accepting transaction in memory pool [%1%] : %2%"
#define BN_TX_CONFIRM_ERROR \
    "Error confirming transaction into blockchain [%1%] : %2%"
#define BN_TX_DEINDEX_ERROR \
    "Error deindexing transaction [%1%] : %2%"
#define BN_TX_INDEX_ERROR \
    "Error indexing transaction [%1%] : %2%"
#define BN_TX_RECEIVE_ERROR \
    "Error receiving transaction [%1%] : %2%"

namespace libbitcoin {
namespace node {

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using boost::format;
using namespace boost::filesystem;
using namespace bc::network;

full_node::full_node(/* configuration */)
  : network_threads_(BN_THREADS_NETWORK, thread_priority::normal),
    database_threads_(BN_THREADS_DISK, thread_priority::low),
    memory_threads_(BN_THREADS_MEMORY, thread_priority::low),
    host_pool_(network_threads_, BN_HOSTS_FILENAME, BN_P2P_HOST_POOL),
    handshake_(network_threads_, BN_LISTEN_PORT),
    network_(network_threads_),
    protocol_(network_threads_, host_pool_, handshake_, network_,
        protocol::default_seeds, BN_LISTEN_PORT, BN_LISTEN, BN_P2P_OUTBOUND),
    blockchain_(database_threads_, BN_DIRECTORY, { BN_HISTORY_START },
        BN_P2P_ORPHAN_POOL),
    poller_(memory_threads_, blockchain_),
    tx_pool_(memory_threads_, blockchain_, BN_P2P_TX_POOL),
    tx_indexer_(memory_threads_),
    session_(network_threads_, handshake_, protocol_, blockchain_, poller_,
        tx_pool_)
{
}
 
bool full_node::start()
{
    // Subscribe to new connections.
    protocol_.subscribe_channel(
        std::bind(&full_node::connection_started,
            this, _1, _2));

    // Start blockchain
    if (!blockchain_.start())
        return false;

    // Start transaction pool
    tx_pool_.start();

    // Fire off app.
    const auto handle_start = std::bind(&full_node::handle_start, this, _1);
    session_.start(handle_start);
    return true;
}

void full_node::stop()
{
    std::promise<std::error_code> code_promise;
    const auto session_stopped = [&code_promise](const std::error_code& ec)
    {
        code_promise.set_value(ec);
    };

    session_.stop(session_stopped);
    const auto ec = code_promise.get_future().get();
    if (ec)
        log_error(LOG_NODE) << format(BN_SESSION_STOP_ERROR) % ec.message();

    // Safely close blockchain database.
    blockchain_.stop();

    // Stop threadpools.
    network_threads_.stop();
    database_threads_.stop();
    memory_threads_.stop();

    // Join threadpools. Wait for them to finish.
    network_threads_.join();
    database_threads_.join();
    memory_threads_.join();
}

chain::blockchain& full_node::chain()
{
    return blockchain_;
}

transaction_indexer& full_node::indexer()
{
    return tx_indexer_;
}

void full_node::handle_start(const std::error_code& ec)
{
    if (ec)
        log_error(LOG_NODE)
            << format(BN_SESSION_START_ERROR) % ec.message();
}

void full_node::connection_started(const std::error_code& ec,
    channel_ptr node)
{
    if (ec)
    {
        log_warning(LOG_NODE)
            << format(BN_CONNECTION_START_ERROR) % ec.message();
        return;
    }

    // Subscribe to transaction messages from this node.
    node->subscribe_transaction(
        std::bind(&full_node::recieve_tx,
            this, _1, _2, node));

    // Stay subscribed to new connections.
    protocol_.subscribe_channel(
        std::bind(&full_node::connection_started,
            this, _1, _2));
}

void full_node::recieve_tx(const std::error_code& ec,
    const transaction_type& tx, channel_ptr node)
{
    if (ec)
    {
        // TODO: format the hash/output so it matches the txid.
        const auto hash = encode_hash(hash_transaction(tx));
        log_error(LOG_NODE)
            << format(BN_TX_RECEIVE_ERROR) % hash % ec.message();
        return;
    }

    // Called when the transaction becomes confirmed in a block.
    const auto handle_confirm = [this, tx](const std::error_code& ec)
    {
        // TODO: format the hash/output so it matches the txid.
        const auto hash = encode_hash(hash_transaction(tx));

        if (ec)
            log_error(LOG_NODE)
                << format(BN_TX_CONFIRM_ERROR) % hash % ec.message();
        else
            log_debug(LOG_NODE)
                << format(BN_TX_CONFIRMED) % hash;

        const auto handle_deindex = [hash](const std::error_code& ec)
        {
            if (ec)
                log_error(LOG_NODE)
                    << format(BN_TX_DEINDEX_ERROR) % hash % ec.message();
        };

        tx_indexer_.deindex(tx, handle_deindex);
    };

    // Validate and store the tx in the transaction mempool.
    tx_pool_.store(tx, handle_confirm,
        std::bind(&full_node::new_unconfirm_valid_tx,
            this, _1, _2, tx));

    // Resubscribe to receive transaction messages from this node.
    node->subscribe_transaction(
        std::bind(&full_node::recieve_tx,
            this, _1, _2, node));
}

static std::string format_unconfirmed_inputs(const index_list& unconfirmed)
{
    if (unconfirmed.empty())
        return "";

    std::vector<std::string> inputs;
    for (const auto input: unconfirmed)
        inputs.push_back(boost::lexical_cast<std::string>(input));

    return bc::join(inputs, ",");
}

void full_node::new_unconfirm_valid_tx(const std::error_code& ec,
    const index_list& unconfirmed, const transaction_type& tx)
{
    // TODO: format the hash/output so it matches the txid.
    const auto hash = encode_hash(hash_transaction(tx));

    const auto handle_index = [hash](const std::error_code& ec)
    {
        if (ec)
            log_error(LOG_NODE)
                << format(BN_TX_INDEX_ERROR) % hash % ec.message();
    };

    if (ec)
        log_warning(LOG_NODE)
            << format(BN_TX_ACCEPT_ERROR) % hash % ec.message();
    else if (unconfirmed.empty())
        log_debug(LOG_NODE)
            << format(BN_TX_ACCEPTED) % hash;
    else
        log_debug(LOG_NODE)
            << format(BN_TX_ACCEPTED_WITH_INPUTS) % hash % format_unconfirmed_inputs(unconfirmed);
        
    if (!ec)
        tx_indexer_.index(tx, handle_index);
}

} // namspace node
} //namespace libbitcoin
/*
 * Copyright (c) 2011-2014 libbitcoin developers (see AUTHORS)
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
#include <bitcoin/node/session.hpp>

#include <bitcoin/blockchain.hpp>

namespace libbitcoin {
namespace node {

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;
using namespace bc::chain;
using namespace bc::network;

session::session(threadpool& pool, handshake& handshake,
    protocol& protocol, blockchain& blockchain, poller& poller,
    transaction_pool& transaction_pool)
  : strand_(pool.service()),
    handshake_(handshake),
    protocol_(protocol),
    chain_(blockchain),
    poll_(poller),
    tx_pool_(transaction_pool)
{
}

void session::start(completion_handler handle_complete)
{
    protocol_.start(handle_complete);

    protocol_.subscribe_channel(
        std::bind(&session::new_channel,
            this, _1, _2));

    // Start height now set in handshake, so do nothing.
    const auto handle_set_height = [](const std::error_code&) {};

    chain_.fetch_last_height(
        std::bind(&handshake::set_start_height,
            &handshake_, _2, handle_set_height));

    chain_.subscribe_reorganize(
        std::bind(&session::set_start_height,
            this, _1, _2, _3, _4));
}

void session::stop(completion_handler handle_complete)
{
    protocol_.stop(handle_complete);
}

void session::new_channel(const std::error_code& ec, channel_ptr node)
{
    if (ec)
    {
        if (node)
            log_error(LOG_SESSION)
                << "Failure to establish channel with ["
                << node->address().to_string() << "] " << ec.message();
        else
            log_error(LOG_SESSION)
                << "Failure to establish channel: " << ec.message();
        return;
    }

    BITCOIN_ASSERT(node);
    node->subscribe_inventory(
        std::bind(&session::inventory,
            this, _1, _2, node));

    node->subscribe_get_blocks(
        std::bind(&session::get_blocks,
            this, _1, _2, node));

    // tx, block
    protocol_.subscribe_channel(
        std::bind(&session::new_channel,
            this, _1, _2));

    poll_.query(node);
    poll_.monitor(node);
}

void session::set_start_height(const std::error_code& ec,
    uint32_t fork_point, const blockchain::block_list& new_blocks,
    const blockchain::block_list& /* replaced_blocks */)
{
    if (ec)
    {
        BITCOIN_ASSERT(ec == error::service_stopped);
        return;
    }

    // Start height now set in handshake, so do nothing.
    static const auto handle_set_height = [](const std::error_code&) {};

    // Start height is limited to max_uint32 by satoshi protocol (version).
    BITCOIN_ASSERT((bc::max_uint32 - fork_point) >= new_blocks.size());
    const auto height = static_cast<uint32_t>(fork_point + new_blocks.size());
    handshake_.set_start_height(height, handle_set_height);

    chain_.subscribe_reorganize(
        std::bind(&session::set_start_height,
            this, _1, _2, _3, _4));

    // Broadcast invs of new blocks.
    inventory_type blocks_inventory;
    for (const auto& block: new_blocks)
        blocks_inventory.inventories.push_back(
        {
            inventory_type_id::block, 
            hash_block_header(block->header)
        });

    const auto ignore_handler = [](const std::error_code&, size_t) {};
    protocol_.broadcast(blocks_inventory, ignore_handler);
}

void session::inventory(const std::error_code& ec,
    const inventory_type& packet, channel_ptr node)
{
    if (ec)
    {
        if (node)
            log_error(LOG_SESSION)
                << "Failure in get inventory from ["
                << node->address().to_string() << "] " << ec.message();
        else
            log_error(LOG_SESSION)
                << "Failure in get inventory: " << ec.message();
        return;
    }

    for (const inventory_vector_type& inventory: packet.inventories)
    {
        if (inventory.type == inventory_type_id::transaction)
        {
            strand_.post(std::bind(&session::new_tx_inventory,
                this, inventory.hash, node));
        }
        else if (inventory.type != inventory_type_id::block)
        {
            // inventory_type_id::block is handled by poller.
            log_warning(LOG_SESSION)
                << "Ignoring unknown inventory type from ["
                << node->address().to_string() << "]";
        }
    }

    BITCOIN_ASSERT(node);
    node->subscribe_inventory(
        std::bind(&session::inventory,
            this, _1, _2, node));
}

void session::new_tx_inventory(const hash_digest& tx_hash, channel_ptr node)
{
    // If the tx doesn't exist, issue getdata.
    tx_pool_.exists(tx_hash, 
        std::bind(&session::request_tx_data,
            this, _1, tx_hash, node));
}

void session::get_blocks(const std::error_code& ec, const get_blocks_type&,
    channel_ptr node)
{
    if (ec)
    {
        if (node)
            log_error(LOG_SESSION) 
                << "Failure in get blocks from [" 
                << node->address().to_string() << "] " << ec.message();
        else
            log_error(LOG_SESSION)
                << "Failure in get blocks: " << ec.message();
        return;
    }

    // TODO: Implement.
    // send 500 invs from last fork point
    // have memory of last inv, ready to trigger send next 500 once
    // getdata done for it.
    BITCOIN_ASSERT(node);
    node->subscribe_get_blocks(
        std::bind(&session::get_blocks,
            this, _1, _2, node));
}

void session::request_tx_data(bool tx_exists, const hash_digest& tx_hash,
    channel_ptr node)
{
    if (tx_exists)
        return;

    get_data_type request_tx;
    request_tx.inventories.push_back(
    {
        inventory_type_id::transaction,
        tx_hash
    });

    const auto handle_request = [node](const std::error_code& ec)
    {
        if (ec)
        {
            if (node)
                log_error(LOG_SESSION) 
                    << "Failure in get tx from [" 
                    << node->address().to_string() << "] " << ec.message();
            else
                log_error(LOG_SESSION)
                    << "Failure in get tx: " << ec.message();
        }
    };

    BITCOIN_ASSERT(node);
    node->send(request_tx, handle_request);
}

} // namespace node
} // namespace libbitcoin

/**
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
#ifndef LIBBITCOIN_NODE_P2P_NODE_HPP
#define LIBBITCOIN_NODE_P2P_NODE_HPP

#include <cstdint>
#include <future>
#include <memory>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/network.hpp>
#include <bitcoin/node/configuration.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/utility/header_queue.hpp>

namespace libbitcoin {
namespace node {

/// A full node on the Bitcoin P2P network.
class BCN_API p2p_node
  : public network::p2p
{
public:
    typedef std::shared_ptr<p2p_node> ptr;
    typedef blockchain::organizer::reorganize_handler reorganize_handler;
    typedef blockchain::transaction_pool::transaction_handler
        transaction_handler;

    /// Construct the full node.
    p2p_node(const configuration& configuration);

    /// Ensure all threads are coalesced.
    virtual ~p2p_node();

    // Start/Run/Stop/Close sequences.
    // ------------------------------------------------------------------------

    /// Invoke startup and seeding sequence, call from constructing thread.
    virtual void start(result_handler handler) override;

    /// Synchronize the blockchain and then begin long running sessions,
    /// call from start result handler. Call base method to skip sync.
    virtual void run(result_handler handler) override;

    /// Non-blocking call to coalesce all work, start may be reinvoked after.
    /// Handler returns the result of file save operations.
    virtual void stop(result_handler handler) override;

    /// Blocking call to coalesce all work and then terminate all threads.
    /// Call from thread that constructed this class, or don't call at all.
    /// This calls stop, and start may be reinvoked after calling this.
    virtual void close() override;

    // Properties.
    // ------------------------------------------------------------------------

    /// Node configuration settings.
    virtual const settings& node_settings() const;

    /// Blockchain query interface.
    virtual blockchain::block_chain& chain();

    /// Transaction pool interface.
    virtual blockchain::transaction_pool& pool();

    // Subscriptions.
    // ------------------------------------------------------------------------

    /// Subscribe to blockchain reorganization and stop events.
    virtual void subscribe_blockchain(reorganize_handler handler);

    /// Subscribe to transaction pool acceptance and stop events.
    virtual void subscribe_transaction_pool(transaction_handler handler);

private:
    void handle_fetch_header(const code& ec, const chain::header& block_header,
        size_t block_height, result_handler handler);
    void handle_headers_synchronized(const code& ec, result_handler handler);
    void handle_network_stopped(const code& ec, result_handler handler);

    void handle_started(const code& ec, result_handler handler);
    void handle_running(const code& ec, result_handler handler);
    void handle_stopped(const code& ec, result_handler handler);
    void handle_closing(const code& ec, std::promise<code>& wait);

    // These are thread safe.
    header_queue hashes_;
    blockchain::block_chain_impl blockchain_;
    const settings& settings_;
};

} // namspace node
} //namespace libbitcoin

#endif

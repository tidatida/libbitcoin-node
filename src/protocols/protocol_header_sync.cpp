/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/protocols/protocol_header_sync.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <bitcoin/network.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/full_node.hpp>
#include <bitcoin/node/utility/header_list.hpp>

namespace libbitcoin {
namespace node {

#define NAME "header_sync"
#define CLASS protocol_header_sync

using namespace bc::config;
using namespace bc::message;
using namespace bc::network;
using namespace std::placeholders;

// The interval in which header download rate is measured and tested.
static const asio::seconds expiry_interval(5);

// This class requires protocol version 31800.
protocol_header_sync::protocol_header_sync(full_node& network,
    channel::ptr channel, header_list::ptr headers, uint32_t minimum_rate)
  : protocol_timer(network, channel, true, NAME),
    headers_(headers),

    // TODO: replace rate backoff with peer competition.
    //=========================================================================
    current_second_(0),
    minimum_rate_(minimum_rate),
    start_size_(headers->previous_height() - headers->first_height()),
    //=========================================================================

    CONSTRUCT_TRACK(protocol_header_sync)
{
}

// Start sequence.
// ----------------------------------------------------------------------------

void protocol_header_sync::start(event_handler handler)
{
    const auto complete = synchronize<event_handler>(
        BIND2(headers_complete, _1, handler), 1, NAME);

    protocol_timer::start(expiry_interval, BIND2(handle_event, _1, complete));

    SUBSCRIBE3(headers, handle_receive_headers, _1, _2, complete);

    // This is the end of the start sequence.
    send_get_headers(complete);
}

// Header sync sequence.
// ----------------------------------------------------------------------------

void protocol_header_sync::send_get_headers(event_handler complete)
{
    if (stopped())
        return;

    const get_headers request
    {
        { headers_->previous_hash() },
        headers_->stop_hash()
    };

    SEND2(request, handle_send, _1, complete);
}

void protocol_header_sync::handle_send(const code& ec, event_handler complete)
{
    if (stopped())
        return;

    if (ec)
    {
        LOG_DEBUG(LOG_NODE)
            << "Failure sending get headers to sync [" << authority() << "] "
            << ec.message();
        complete(ec);
    }
}

bool protocol_header_sync::handle_receive_headers(const code& ec,
    headers_const_ptr message, event_handler complete)
{
    if (stopped())
        return false;

    if (ec)
    {
        LOG_DEBUG(LOG_NODE)
            << "Failure receiving headers from sync ["
            << authority() << "] " << ec.message();
        complete(ec);
        return false;
    }

    const auto start = headers_->previous_height() + 1;

    // A merge failure resets the headers list.
    if (!headers_->merge(message))
    {
        LOG_WARNING(LOG_NODE)
            << "Failure merging headers from [" << authority() << "]";
        complete(error::invalid_previous_block);
        return false;
    }

    const auto end = headers_->previous_height();

    LOG_INFO(LOG_NODE)
        << "Synced headers " << start << "-" << end << " from ["
        << authority() << "]";

    if (headers_->complete())
    {
        complete(error::success);
        return false;
    }

    // If we received fewer than 2000 the peer is exhausted, try another.
    if (message->elements().size() < max_get_headers)
    {
        complete(error::operation_failed);
        return false;
    }

    // This peer has more headers.
    send_get_headers(complete);
    return true;
}

// This is fired by the base timer and stop handler.
void protocol_header_sync::handle_event(const code& ec, event_handler complete)
{
    if (ec == error::channel_stopped)
    {
        complete(ec);
        return;
    }

    if (ec && ec != error::channel_timeout)
    {
        LOG_WARNING(LOG_NODE)
            << "Failure in header sync timer for [" << authority() << "] "
            << ec.message();
        complete(ec);
        return;
    }

    // TODO: replace rate backoff with peer competition.
    //=========================================================================
    // It was a timeout so another expiry period has passed (overflow ok here).
    current_second_ += static_cast<size_t>(expiry_interval.count());
    auto rate = (headers_->previous_height() - start_size_) / current_second_;
    //=========================================================================

    // Drop the channel if it falls below the min sync rate averaged over all.
    if (rate < minimum_rate_)
    {
        LOG_DEBUG(LOG_NODE)
            << "Header sync rate (" << rate << "/sec) from ["
            << authority() << "]";
        complete(error::channel_timeout);
        return;
    }
}

void protocol_header_sync::headers_complete(const code& ec,
    event_handler handler)
{
    // This is end of the header sync sequence.
    handler(ec);

    // The session does not need to handle the stop.
    stop(error::channel_stopped);
}

} // namespace node
} // namespace libbitcoin

/* ****************************************************************************
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 *
 *
 * This file is part of Microsoft R Host.
 *
 * Microsoft R Host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * Microsoft R Host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Microsoft R Host.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ***************************************************************************/

#pragma once
#include "stdafx.h"
#include "Rapi.h"
#include "util.h"
#include "blobs.h"
#include "message.h"
#include "log.h"

namespace rhost {
    namespace host {
        class eval_cancel_error : std::exception {
        };

        std::future<void> wait_for_client(const boost::asio::ip::tcp::endpoint& endpoint);
        std::future<void> connect_to_server(const websocketpp::uri& uri);
        void register_callbacks(structRstart& rp);
        void terminate_if_closed();

        extern "C" void ShowMessage(const char* s);
        extern "C" int YesNoCancel(const char* s);
        extern "C" int OkCancel(const char* s);
        extern "C" int YesNo(const char* s);

        extern boost::signals2::signal<void()> callback_started;
        extern boost::signals2::signal<void()> readconsole_done;

        __declspec(noreturn) void propagate_cancellation();

        template <class F>
        auto with_cancellation(F body) -> decltype(body()) {
            terminate_if_closed();
            try {
                return body();
            } catch (const eval_cancel_error&) {
                propagate_cancellation();
            }
        }

        protocol::message_id send_notification(const std::string& name, const picojson::array& args, const blobs::blob& blob);

        template<class... Args>
        inline protocol::message_id send_notification(const char* name, const Args&... args) {
            picojson::array args_array;
            rhost::util::append(args_array, args...);
            return send_notification(name, args_array);
        }

        template<class... Args>
        inline protocol::message_id send_notification(const std::string&, const blobs::blob& blob, const Args&... args) {
            picojson::array args_array;
            rhost::util::append(args_array, args...);
            return send_notification(name, blob, args_array);
        }

        protocol::message send_request_and_get_response(const std::string&, const picojson::array& args);

        template<class... Args>
        inline protocol::message send_request_and_get_response(const std::string&, const Args&... args) {
            picojson::array args_array;
            rhost::util::append(args_array, args...);
            return send_request_and_get_response(name, args_array);
        }

        typedef uint64_t blob_id;

        blob_id create_blob(blobs::blob&& blob);

        inline blob_id create_blob(const blobs::blob& blob) {
            auto copy = blob;
            return create_blob(copy);
        }

        void get_blob(blob_id id, blobs::blob& blob);

        inline blobs::blob get_blob(blob_id id) {
            blobs::blob result;
            get_blob(id, result);
            return result;
        }

        void destroy_blob(blob_id id);
    }
}

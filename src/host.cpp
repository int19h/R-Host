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

#include "host.h"
#include "log.h"
#include "msvcrt.h"
#include "eval.h"
#include "util.h"
#include "json.h"
#include "blobs.h"

using namespace std::literals;
using namespace boost::endian;
using namespace rhost::log;
using namespace rhost::util;
using namespace rhost::eval;
using namespace rhost::json;
using namespace rhost::blobs;

namespace rhost {
    namespace host {
        const char subprotocol[] = "Microsoft.R.Host";
        const long heartbeat_timeout =
#ifdef NDEBUG
            5'000;
#else 
            // In debug mode, make the timeout much longer, so that debugging the host client won't cause
            // the host to disconnect quickly because the client is not responding when paused in debugger.
            600'000;
#endif

        boost::signals2::signal<void()> callback_started;
        boost::signals2::signal<void()> readconsole_done;

        struct message_header {
            little_uint32_buf_t flags;
            little_uint64_buf_t id;
            little_uint64_buf_t request_id;
            little_int8_buf_t name_length;
            char name[];
        };

        enum class message_flags : uint32_t {
            request = 1 << 1
        };
        FLAGS_ENUM(message_flags);

        typedef websocketpp::connection<websocketpp::config::asio> ws_connection_type;

        // There's no std::atomic<std::shared_ptr<...>>, but there are free-standing atomic_* functions that can do 
        // the same manually - wrap ws_conn into a class that ensures that all access goes through those functions.
        // TODO: replace with C++17 atomic_shared_ptr once it is available.
        struct {
            std::shared_ptr<ws_connection_type> operator-> () const {
                return std::atomic_load(&ptr);
            }

            operator std::shared_ptr<ws_connection_type>() const {
                return operator->();
            }

            explicit operator bool() const {
                return static_cast<bool>(operator->());
            }

            void operator= (std::shared_ptr<ws_connection_type> value) {
                std::atomic_store(&ptr, value);
            }

        private:
            std::shared_ptr<ws_connection_type> ptr;
        } ws_conn;

        DWORD main_thread_id;
        std::promise<void> connected_promise;
        std::atomic<bool> is_connection_closed = false;
        std::atomic<bool> is_waiting_for_wm = false;
        long long next_message_id = 2;
        bool allow_callbacks = true, allow_intr_in_CallBack = true;

        // Specifies whether the host is currently expecting a response message to some earlier request that it had sent.
        // The host can always receive eval and cancellation requests, and they aren't considered responses. If any other
        // message is received, state must be RESPONSE_EXPECTED; it is then changed to RESPONSE_RECEIVED, and message is
        // saved in response. If state was not RESPONSE_EXPECTED when message was received, it is considered a fatal error.
        enum response_state_t { RESPONSE_UNEXPECTED, RESPONSE_EXPECTED, RESPONSE_RECEIVED } response_state;
        // Most recent message received in response to RESPONSE_EXPECTED.
        message response;
        std::mutex response_mutex;

        // Eval requests queued for execution. When eval begins executing, it is removed from this queue, and placed onto eval_stack.
        std::queue<message> eval_requests;
        std::mutex eval_requests_mutex;

        enum class eval_kind : uint32_t {
            normal = 0,
            reentrant = 1 << 1,
            base_env = 1 << 3,
            empty_env = 1 << 4,
            cancelable = 1 << 5,
            mutating = 1 << 7,
            no_result = 1 << 8,
            blob_result = 1 << 9
        };
        FLAGS_ENUM(eval_kind);

        enum class eval_result_kind : uint8_t {
            error,
            cancel,
            none,
            json,
            blob,
        };
        FLAGS_ENUM(eval_result_kind);

        struct eval_info {
            const uint64_t id;
            const bool is_cancelable;

            eval_info(uint64_t id, bool is_cancelable)
                : id(id), is_cancelable(is_cancelable) {
            }
        };

        // Keeps track of evals that are currently being executed (as opposed to queued - that is tracked by eval_requests).
        // The first item is always dummy eval representing evaluation of input on the last ReadConsole prompt. Following it
        // is the current real top-level eval, and then any nested evals are appended at the end, in order of their nesting. 
        // For example, if an eval request for "x" came in (and it was re-entrant, thus permitting nested evals); and then,
        // while it was executing, an eval request for "y" came in; and then while that was executing, "z" came in, then the
        // stack will look like this:
        //
        //   <dummy> x y z
        // 
        // When cancellation for any eval on the stack is requested, all evals that follow it on the stack are also canceled,
        // since execution will not return to the eval unless all nested evals are terminated. When cancellation of all evals
        // is requested, it is implemented as cancellation of the topmost dummy eval. 
        std::vector<eval_info> eval_stack({ eval_info(0, true) });
        bool canceling_eval; // whether we're currently processing a cancellation request by unwinding the eval stack
        uint64_t eval_cancel_target; // ID of the eval on the stack that is the cancellation target
        std::mutex eval_stack_mutex;

        uint64_t next_blob_id = 1;
        std::map<uint64_t, std::vector<char>> blobs;
        std::mutex blobs_mutex;

        void terminate_if_closed() {
            // terminate invokes R_Suicide, which may invoke WriteConsole and/or ShowMessage, which will
            // then call terminate again, so we need to prevent infinite recursion here.
            static bool is_terminating;
            if (is_terminating) {
                return;
            }

            if (is_connection_closed) {
                is_terminating = true;
                terminate("Lost connection to client.");
            }
        }

        std::error_code send_json(const picojson::value& value) {
            std::string json = value.serialize();

#ifdef TRACE_JSON
            logf("<== %s\n\n", json.c_str());
#endif

            if (!is_connection_closed) {
                auto err = ws_conn->send(json, websocketpp::frame::opcode::text);
                if (err) {
                    fatal_error("Send failed: [%d] %s", err.value(), err.message().c_str());
                }
                return err;
            } else {
                return std::error_code();
            }
        }

        std::error_code send_blob(const rhost::util::blob_slice& blob_slice) {
#ifdef TRACE_JSON
            logf("<== blob[%lld]\n\n", blob_slice.size());
#endif
            if (!is_connection_closed) {
                auto err = ws_conn->send(static_cast<const void*>(blob_slice.data()), blob_slice.size(), websocketpp::frame::opcode::binary);
                if (err) {
                    fatal_error("Send failed: [%d] %s", err.value(), err.message().c_str());
                }
                return err;
            } else {
                return std::error_code();
            }
        }

        std::string make_message_header(picojson::array& array, const char* name, const size_t blob_slices, const char* request_id) {
            char id[0x20];
            sprintf_s(id, "#%lld#", next_message_id);
            next_message_id += 2;

            if (request_id) {
                append(array, id, name, static_cast<double>(blob_slices), request_id);
            } else {
                append(array, id, name, static_cast<double>(blob_slices));
            }
            return id;
        }

        void make_message_json(picojson::array& array, picojson::array& header, const picojson::array& args = picojson::array()) {
            append(array, header);
            array.insert(array.end(), args.begin(), args.end());
        }

        std::string send_message(const char* name, const picojson::array& args, const rhost::util::blob blob = rhost::util::blob()) {
            assert(name[0] == '!' || name[0] == '?' || name[0] == ':');
            picojson::array header;
            auto id = make_message_header(header, name, blob.size(), nullptr);

            picojson::value value(picojson::array_type, false);
            auto& array = value.get<picojson::array>();

            make_message_json(array, header, args);
            send_json(value);

            for (rhost::util::blob::const_iterator iter = blob.begin(); iter != blob.end(); ++iter) {
                send_blob(*iter);
            }

            return id;
        }

        uint64_t send_message(const char* name, message_kind kind, uint64_t request_id, const std::string& body) {
            if (!is_connection_closed) {
                auto err = ws_conn->send(json, websocketpp::frame::opcode::text);
                if (err) {
                    fatal_error("Send failed: [%d] %s", err.value(), err.message().c_str());
                }
                return err;
            } else {
                return std::error_code();
            }
        }

        std::string send_notification(const char* name, const rhost::util::blob& blob, const picojson::array& args) {
            assert(name[0] == '!');
            return send_message(name, args, blob);
        }

        std::string send_notification(const char* name, const picojson::array& args) {
            assert(name[0] == '!');
            return send_message(name, args);
        }

        template<class... Args>
        std::string respond_to_message(const message& request, const rhost::util::blob& blob, Args... args) {
            assert(request.name[0] == '?');

            picojson::array header;
            auto id = make_message_header(header, (':' + request.name.substr(1)).c_str(), blob.size(), request.id.c_str());

            picojson::value value(picojson::array_type, false);
            auto& array = value.get<picojson::array>();
            append(array, header);
            append(array, args...);
            send_json(value);

            for (rhost::util::blob::const_iterator iter = blob.begin(); iter != blob.end(); ++iter) {
                send_blob(*iter);
            }

            return id;
        }

        template<class... Args>
        std::string respond_to_message(const message& request, Args... args) {
            return respond_to_message(request, rhost::util::blob(), args...);
        }

        bool query_interrupt() {
            std::lock_guard<std::mutex> lock(eval_stack_mutex);
            if (!canceling_eval) {
                return false;
            }

            // If there is a non-cancellable eval on the stack, do not allow to interrupt it or anything nested.
            auto it = std::find_if(eval_stack.begin(), eval_stack.end(), [](auto ei) { return !ei.is_cancelable; });
            return it == eval_stack.end();
        }


        // Unblock any pending with_response call that is waiting in a message loop.
        void unblock_message_loop() {
            // Because PeekMessage can dispatch messages that were sent, which may in turn result 
            // in nested evaluation of R code and nested message loops, sending a single WM_NULL
            // may not be sufficient, so keep sending them until the waiting flag is cleared - 
            // because WM_NULL is no-op, posting extra ones is harmless.
            // However, we need to pause and give the other thread some time to process, otherwise
            // we can flood its WM queue faster than it can process it, and it might never stop
            // pumping events and return to PeekMessage.
            auto delay = 10ms;
            for (; is_waiting_for_wm; std::this_thread::sleep_for(delay)) {
                PostThreadMessage(main_thread_id, WM_NULL, 0, 0);

                // Further guard against overflowing the queue by posting to it too aggressively.
                // If previous wait didn't help, give it a little more time to process next message,
                // up to a reasonable limit.
                if (delay < 5000ms) {
                    delay *= 2;
                }
            }
        }

        uint64_t create_blob(std::vector<char>&& blob) {
            std::lock_guard<std::mutex> lock(blobs_mutex);
            uint64_t blob_id = ++next_blob_id;
            blobs[blob_id] = blob;
            return blob_id;
        }

        void create_blob(const message& msg) {
            assert(msg.name == "CreateBlob");

            struct create_blob_request {
                little_uint64_buf_t size;
                char data[];
            } const* request = reinterpret_cast<const create_blob_request*>(msg.body.data());

            std::vector<char> blob(request->data, request->data + request->size.value());
            uint64_t blob_id = create_blob(std::move(blob));
            respond_to_message(msg, static_cast<double>(blob_id));
        }

        void get_blob(uint64_t id, std::vector<char>& result) {
            std::lock_guard<std::mutex> lock(blobs_mutex);

            auto it = blobs.find(id);
            if (it == blobs.end()) {
                fatal_error("ReadBlob: no blob with ID %" PRIu64, id);
            }

            result = it->second;
        }

        void get_blob(const message& msg) {
            assert(msg.name == "GetBlob");

            struct read_blob_request {
                little_uint64_buf_t blob_id;
            } const* request = reinterpret_cast<const read_blob_request*>(msg.body.data());

            respond_to_message(msg, get_blob(request->blob_id.value()));
        }

        void destroy_blob(uint64_t blob_id) {
            std::lock_guard<std::mutex> lock(blobs_mutex);
            blobs.erase(blob_id);
        }

        void destroy_blobs(const message& msg) {
            assert(msg.name == "DestroyBlob");

            struct destroy_blobs_request {
                little_uint64_buf_t blob_ids[];
            } const* request = reinterpret_cast<const destroy_blobs_request*>(msg.body.data());

            std::lock_guard<std::mutex> lock(blobs_mutex);
            for (size_t i = 0;; ++i) {
                uint64_t blob_id = request->blob_ids[i].value();
                if (!blob_id) {
                    break;
                }
                destroy_blob(blob_id);
            }
        }

        void handle_eval(const message& msg) {
            assert(msg.name == "=");

            struct eval_request {
                little_uint32_buf_t kind;
                char expr[];
            } const* request = reinterpret_cast<const eval_request*>(msg.body.data());

            SCOPE_WARDEN_RESTORE(allow_callbacks);
            allow_callbacks = false;

            auto kind = static_cast<eval_kind>(request->kind.value());
            const auto& expr = from_utf8(request->expr);
            log::logf("%s = %s\n\n", msg.id.c_str(), expr.c_str());

            SEXP env =
                has_flag(kind, eval_kind::base_env) ? R_BaseEnv :
                has_flag(kind, eval_kind::empty_env) ? R_EmptyEnv :
                R_GlobalEnv;
            r_eval_result<protected_sexp> result = {};
            ParseStatus ps;
            {
                // We must not register this eval as a potential cancellation target before it gets a chance to establish
                // the restart context; otherwise, there is a possibility that a cancellation request will arrive during
                // that interval, and abort the outer eval instead. Similarly, we must remove this eval from the eval stack
                // before the restart context is torn down, so that untimely cancellation request for the outer eval doesn't
                // cancel his one.

                bool was_before_invoked = false;
                auto before = [&] {
                    std::lock_guard<std::mutex> lock(eval_stack_mutex);
                    eval_stack.push_back(eval_info(msg.id, has_flag(kind, eval_kind::cancelable)));
                    was_before_invoked = true;
                };

                bool was_after_invoked = false;
                auto after = [&] {
                    std::lock_guard<std::mutex> lock(eval_stack_mutex);

                    if (was_before_invoked) {
                        assert(!eval_stack.empty());
                        assert(eval_stack.end()[-1].id == msg.id);
                    }

                    if (canceling_eval && msg.id == eval_cancel_target) {
                        // If we were unwinding the stack for cancellation purposes, and this eval was the target
                        // of the cancellation, then we're done and should stop unwinding. Otherwise, we should 
                        // continue unwinding after reporting the result of the evaluation, which we'll do at the
                        // end of handle_eval if this flag is still set.
                        canceling_eval = false;
                    }

                    if (was_before_invoked) {
                        eval_stack.pop_back();
                    }

                    was_after_invoked = true;
                };

                auto results = r_try_eval(expr, env, ps, before, after);
                if (!results.empty()) {
                    result = results.back();
                }

                // If eval was canceled, the "after" block was never executed (since it is normally run within the eval
                // context, and so cancelation unwinds it along with everything else in that context), so we need to run
                // it manually afterwards. Note that there's no potential race with newly arriving cancellation requests
                // in this case, since we're already servicing one for this eval (or some parent eval).
                if (!was_after_invoked) {
                    after();
                }

                allow_intr_in_CallBack = true;
            }

            std::string data;
            if (result.has_error) {
                data = to_utf8(result.error);
            } else if (result.has_value && !has_flag(kind, eval_kind::no_result)) {
                try {
                    if (has_flag(kind, eval_kind::blob_result)) {
                        errors_to_exceptions([&] { to_blob(result.value.get(), data); });
                    } else {
                        picojson::value json;
                        errors_to_exceptions([&] { to_json(result.value.get(), json); });
                        data = json.serialize();
                    }
                } catch (r_error& err) {
                    fatal_error("%s", err.what());
                }
            }

            struct eval_response {
                little_uint8_buf_t parse_status;
                little_uint8_buf_t kind;
                little_uint64_buf_t length;
                char data[];
            };

            std::unique_ptr<char[]> buf(new char[sizeof eval_response + data.size()]);
            auto response = reinterpret_cast<eval_response*>(buf.get());

            response->parse_status = ps;

            if (result.has_error) {
                response->kind = eval_result_kind::error;
            } else if (result.is_canceled) {
                response->kind = eval_result_kind::cancel;
            } else if (has_flag(kind, eval_kind::no_result)) {
                response->kind = eval_result_kind::none;
            } else if (has_flag(kind, eval_kind::blob_result)) {
                response->kind = eval_result_kind::blob;
            } else {
                response->kind = eval_result_kind::json;
            }

            response->length = data.size();
            memcpy(response->data, data.data(), data.size());

#ifdef TRACE_JSON
            indent_log(+1);
#endif
            if (result.is_canceled) {
                respond_to_message(msg, picojson::value());
            } else {
                respond_to_message(msg, blob, parse_status, error, value);
            }
#ifdef TRACE_JSON
            indent_log(-1);
#endif

            // If cancellation hasn't finished yet, continue unwinding the context stack. We don't want to call
            // Rf_onintr here, because this would skip over all the local object destructors in this function,
            // as well as the callback that invoked it. Instead, throw an exception and let C++ do unwinding the
            // normal way, and callback will then catch it at the very end, and invoke Rf_onintr just before it 
            // would've normally returned to R; see with_cancellation.
            if (query_interrupt()) {
                throw eval_cancel_error();
            }
        }

        void handle_cancel(const message& msg) {
            if (msg.args.size() != 1) {
                fatal_error("Evaluation cancellation request must be of the form [id, '/', eval_id].");
            }

            std::string eval_id;
            if (!msg.args[0].is<picojson::null>()) {
                if (!msg.args[0].is<std::string>()) {
                    fatal_error("Evaluation cancellation request eval_id must be string or null.");
                }
                eval_id = msg.args[0].get<std::string>();
            }

            std::lock_guard<std::mutex> lock(eval_stack_mutex);

            for (auto eval_info : eval_stack) {
                auto& id = eval_info.id;

                if (canceling_eval && id == eval_cancel_target) {
                    // If we're already in the process of cancelling some eval, and that one is below the
                    // one that we're been asked to cancel in the stack, then we don't need to do anything.
                    break;
                }

                if (id == eval_id) {
                    canceling_eval = true;
                    eval_cancel_target = id;
                    break;
                }
            }

            if (canceling_eval) {
                // Spin the loop in send_request_and_get_response so that it gets a chance to run cancel checks.
                unblock_message_loop();
            } else {
                // If we didn't find the target eval in the stack, it must have completed already, and we've
                // got a belated cancelation request for it, which we can simply ignore.
            }
        }

        void propagate_cancellation() {
            // Prevent CallBack from doing anything if it's called from within Rf_onintr again.
            allow_intr_in_CallBack = false;

            interrupt_eval();

            assert(!"Rf_onintr should never return.");
            throw;
        }

        inline message send_request_and_get_response(const char* name, const picojson::array& args) {
            assert(name[0] == '?');

            response_state_t old_response_state;
            {
                std::lock_guard<std::mutex> lock(response_mutex);
                old_response_state = response_state;
                response_state = RESPONSE_EXPECTED;
            }

            auto id = send_message(name, args);
            terminate_if_closed();

            indent_log(+1);
            SCOPE_WARDEN(dedent_log, { indent_log(-1); });

            for (;;) {
                message msg;
                for (;;) {
                    {
                        // If there's anything in eval queue, break to process that.
                        {
                            std::lock_guard<std::mutex> lock(eval_requests_mutex);
                            if (!eval_requests.empty()) {
                                msg = eval_requests.front();
                                eval_requests.pop();
                                break;
                            }
                        }

                        std::lock_guard<std::mutex> lock(response_mutex);
                        if (response_state == RESPONSE_UNEXPECTED) {
                            assert(false);
                            fatal_error("Invalid response state transition: went from RESPONSE_EXPECTED to RESPONSE_UNEXPECTED.");
                        }
                        if (response_state == RESPONSE_RECEIVED) {
                            msg = response;
                            response_state = old_response_state;
                            break;
                        }
                    }

                    // R_ProcessEvents may invoke CallBack. If there is a pending cancellation request, we do
                    // not want CallBack to call Rf_onintr as it normally does, since it would unwind the stack
                    // using longjmp, which will skip destructors for all our local variables. Instead, make
                    // CallBack a no-op until event processing is done, and then do a manual cancellation check.
                    allow_intr_in_CallBack = false;

                    R_ToplevelExec([](void*) {
                        // Errors can happen during event processing (from GUI windows such as graphs), and
                        // we don't want them to bubble up here, so run these in a fresh execution context.
                        is_waiting_for_wm = true;
                        R_WaitEvent();
                        is_waiting_for_wm = false;
                        R_ProcessEvents();
                    }, nullptr);

                    // In case anything in R_WaitEvent failed and unwound the context before we could reset.
                    is_waiting_for_wm = false;

                    allow_intr_in_CallBack = true;

                    terminate_if_closed();

                    if (query_interrupt()) {
                        throw eval_cancel_error();
                    }
                }

                if (!msg.request_id.empty()) {
                    if (msg.request_id != id) {
                        fatal_error("Received response ['%s','%s'], while awaiting response for ['%s','%s'].",
                            msg.request_id.c_str(), msg.name.c_str(), id.c_str(), name);
                    } else if (strcmp(msg.name.c_str() + 1, name + 1) != 0) {
                        fatal_error("Response to ['%s','%s'] has mismatched name '%s'.",
                            id.c_str(), name, msg.name.c_str());
                    }
                    return msg;
                }

                if (msg.name.size() >= 2 && msg.name[0] == '?' && msg.name[1] == '=') {
                    handle_eval(msg);
                } else {
                    fatal_error("Unrecognized incoming message name '%s'.", msg.name.c_str());
                }
            }
        }

        picojson::array get_context() {
            picojson::array context;
            for (RCNTXT* ctxt = R_GlobalContext; ctxt != nullptr; ctxt = ctxt->nextcontext) {
                context.push_back(picojson::value(double(ctxt->callflag)));
            }
            return context;
        }

        extern "C" void CallBack() {
            // Called periodically by R_ProcessEvents and Rf_eval. This is where we check for various
            // cancellation requests and issue an interrupt (Rf_onintr) if one is applicable in the
            // current context.
            callback_started();

            // Rf_onintr may end up calling CallBack before it returns. We don't want to recursively
            // call it again, so do nothing and let the next eligible callback handle things.
            if (!allow_intr_in_CallBack) {
                return;
            }

            if (query_interrupt()) {
                allow_intr_in_CallBack = false;
                interrupt_eval();
                // Note that allow_intr_in_CallBack is not reset to false here. This is because Rf_onintr
                // does not return (it unwinds via longjmp), and therefore any code here wouldn't run.
                // Instead, we reset the flag where the control will end up after unwinding - either
                // immediately after r_try_eval returns, or else (if we unwound R's own REPL eval) at
                // the beginning of the next ReadConsole.
                assert(!"Rf_onintr should never return.");
            }

            // Process any pending eval requests if reentrancy is allowed.
            if (allow_callbacks) {
                for (;;) {
                    message msg;
                    {
                        std::lock_guard<std::mutex> lock(eval_requests_mutex);
                        if (eval_requests.empty()) {
                            break;

                        } else {
                            msg = eval_requests.front();
                            eval_requests.pop();
                        }
                    }

                    handle_eval(msg);
                }
            }
        }

        extern "C" int R_ReadConsole(const char* prompt, char* buf, int len, int addToHistory) {
            return with_cancellation([&] {
                if (!allow_intr_in_CallBack) {
                    // If we got here, this means that we've just processed a cancellation request that had
                    // unwound the context stack all the way to the bottom, cancelling all the active evals;
                    // otherwise, handle_eval would have allow_intr_in_CallBack set to true immediately after
                    // the targeted eval had returned. Mark everything cancellation-related as done.
                    assert(eval_stack.size() == 1);
                    canceling_eval = false;
                    allow_intr_in_CallBack = true;

                    // Notify client that cancellation has completed. When a specific eval is being canceled,
                    // there will be a corresponding (error) response to the original '?=' message indicating
                    // completion, but for top-level canellation we need a special message.
                    send_notification("!CanceledAll");
                }

                bool is_browser = false;
                for (RCNTXT* ctxt = R_GlobalContext; ctxt != nullptr; ctxt = ctxt->nextcontext) {
                    if (ctxt->callflag & CTXT_BROWSER) {
                        is_browser = true;
                        break;
                    }
                }

                if (!allow_callbacks && len >= 3) {
                    if (is_browser) {
                        // If this is a Browse prompt, raising an error is not a proper way to reject it -
                        // it will simply start an infinite loop with every new error producing such prompt.
                        // Instead, just tell the interpreter to continue execution.
                        buf[0] = 'c';
                        buf[1] = '\n';
                        buf[2] = '\0';
                        return 1;
                    }

                    Rf_error("ReadConsole: blocking callback not allowed during evaluation.");
                }

                // Check for and perform auto-stepping on the current instruction if necessary.
                if (is_browser && R_Srcref && R_Srcref != R_NilValue) {
                    static SEXP auto_step_over_symbol = Rf_install("Microsoft.R.Host::auto_step_over");
                    int auto_step_over = Rf_asLogical(Rf_getAttrib(R_Srcref, auto_step_over_symbol));
                    if (auto_step_over && auto_step_over != R_NaInt) {
                        buf[0] = 'n';
                        buf[1] = '\n';
                        buf[2] = '\0';
                        return 1;
                    }
                }

                readconsole_done();

                for (std::string retry_reason;;) {
                    auto msg = send_request_and_get_response(
                        "?>", get_context(), double(len), addToHistory != 0,
                        retry_reason.empty() ? picojson::value() : picojson::value(retry_reason),
                        to_utf8_json(prompt));

                    if (msg.args.size() != 1) {
                        fatal_error("ReadConsole: response must have a single argument.");
                    }

                    const auto& arg = msg.args[0];
                    if (arg.is<picojson::null>()) {
                        return 0;
                    }

                    if (!arg.is<std::string>()) {
                        fatal_error("ReadConsole: response argument must be string or null.");
                    }

                    auto s = from_utf8(arg.get<std::string>());
                    if (s.size() >= len) {
                        retry_reason = "BUFFER_OVERFLOW";
                        continue;
                    }

                    strcpy_s(buf, len, s.c_str());
                    return 1;
                }
            });
        }

        extern "C" void WriteConsoleEx(const char* buf, int len, int otype) {
            with_cancellation([&] {
                send_notification((otype ? "!!" : "!"), to_utf8_json(buf));
            });
        }

        extern "C" void Busy(int which) {
            with_cancellation([&] {
                send_notification(which ? "!+" : "!-");
            });
        }

        extern "C" void atexit_handler() {
            if (ws_conn) {
                with_cancellation([&] {
                    send_json(picojson::value());
                });
            }
        }

        bool ws_validate_handler(websocketpp::connection_hdl hdl) {
            auto& protos = ws_conn->get_requested_subprotocols();
            logf("Incoming connection requesting subprotocols: [ ");
            for (auto proto : protos) {
                logf("'%s' ", proto.c_str());
            }
            logf("]\n");

            auto it = std::find(protos.begin(), protos.end(), subprotocol);
            if (it == protos.end()) {
                fatal_error("Expected subprotocol %s was not requested", subprotocol);
            }

            ws_conn->select_subprotocol(subprotocol);
            return true;
        }

        void ws_fail_handler(websocketpp::connection_hdl hdl) {
            fatal_error("websocket connection failed: %s", ws_conn->get_ec().message().c_str());
        }

        void ws_open_handler(websocketpp::connection_hdl hdl) {
            send_notification("Microsoft.R.Host", 1.0, getDLLVersion());
            connected_promise.set_value();

            std::error_code ec;
            ws_conn->ping("", ec);
        }

        void ws_message_handler(websocketpp::connection_hdl hdl, ws_connection_type::message_ptr ws_msg) {
            if (ws_msg->get_opcode() != websocketpp::frame::opcode::value::binary) {
                fatal_error("Non-binary websocket message received.");
            }

            const std::string& payload = ws_msg->get_payload();
            const char* body = payload.data();

            auto header = reinterpret_cast<const message_header*>(body);
            auto flags = static_cast<message_flags>(header->flags.value());

            message msg = {};
            msg.id = header->id.value();
            msg.request_id = header->request_id.value();

            int8_t name_length = header->name_length.value();
            if (msg.request_id && name_length) {
                fatal_error("Response message must have a zero-length name.");
            } else if (name_length <= 0) {
                fatal_error("Message name length must be positive.");
            }
            msg.name = std::string(header->name, header->name_length.value());
            msg.body = std::string(body + sizeof(*header) + name_length, body + payload.size());

#ifdef TRACE_JSON
            logf("==> #%" PRIu64 "# ", msg.id);
            if (msg.request_id) {
                logf(": #%" PRIu64 "# ", msg.request_id);
            } else {
                logf("%s ", msg.name.c_str());
            }
            logf("%s\n", msg.body.c_str());
#endif

            if (msg.name == "Disconnect") {
                terminate("Shutdown request received.");
            } else if (msg.name == "/") {
                return handle_cancel(msg);
            } else if (msg.name == "CreateBlob") {
                return create_blob(msg);
            } else if (msg.name == "GetBlob") {
                return get_blobs(msg);
            } else if (msg.name == "DestroyBlob") {
                return destroy_blobs(msg);
            } else if (msg.name == "=") {
                std::lock_guard<std::mutex> lock(eval_requests_mutex);
                eval_requests.push(msg);
                unblock_message_loop();
                return;
            }

            std::lock_guard<std::mutex> lock(response_mutex);
            assert(response_state != RESPONSE_RECEIVED);
            if (response_state == RESPONSE_UNEXPECTED) {
                fatal_error("Unexpected incoming client message - not an eval or cancellation request, and not expecting a response.");
            }

            response = msg;
            response_state = RESPONSE_RECEIVED;
            unblock_message_loop();
        }

        void ws_close_handler(websocketpp::connection_hdl h) {
            is_connection_closed = true;
            unblock_message_loop();
        }

        void ws_pong_handler(websocketpp::connection_hdl hdl, std::string) {
            if (auto conn = ws_conn) {
                conn->set_timer(1'000, [](auto error_code) {
                    if (auto conn = ws_conn) {
                        conn->ping("", error_code);
                    }
                });
            }
        }

        void ws_pong_timeout_handler(websocketpp::connection_hdl, std::string) {
            terminate("Client did not respond to heartbeat ping.");
        }

        void initialize_ws_endpoint(websocketpp::endpoint<websocketpp::connection<websocketpp::config::asio>, websocketpp::config::asio>& endpoint) {
#ifndef TRACE_WEBSOCKET
            endpoint.set_access_channels(websocketpp::log::alevel::none);
            endpoint.set_error_channels(websocketpp::log::elevel::none);
#endif

            endpoint.init_asio();
            endpoint.set_validate_handler(ws_validate_handler);
            endpoint.set_open_handler(ws_open_handler);
            endpoint.set_message_handler(ws_message_handler);
            endpoint.set_close_handler(ws_close_handler);
            endpoint.set_pong_handler(ws_pong_handler);
            endpoint.set_pong_timeout_handler(ws_pong_timeout_handler);
            endpoint.set_pong_timeout(heartbeat_timeout);
        }

        void server_worker(boost::asio::ip::tcp::endpoint endpoint) {
            websocketpp::server<websocketpp::config::asio> server;
            initialize_ws_endpoint(server);

            std::ostringstream endpoint_str;
            endpoint_str << endpoint;
            logf("Waiting for incoming connection on %s ...\n", endpoint_str.str().c_str());

            std::error_code error_code;
            server.listen(endpoint, error_code);
            if (error_code) {
                fatal_error("Could not open server socket for listening: %s", error_code.message().c_str());
            }

            auto conn = server.get_connection();
            ws_conn = conn;
            server.async_accept(conn, [&](auto error_code) {
                if (error_code) {
                    conn->terminate(error_code);
                    fatal_error("Could not establish connection to client: %s", error_code.message().c_str());
                } else {
                    conn->start();
                    conn->ping("", error_code);
                }
            });

            server.run();
        }

        void client_worker(websocketpp::uri uri) {
            websocketpp::client<websocketpp::config::asio> client;
            initialize_ws_endpoint(client);

            logf("Establishing connection to %s ...\n", uri.str().c_str());

            auto uri_ptr = std::make_shared<websocketpp::uri>(uri);
            std::error_code error_code;
            ws_conn = client.get_connection(uri_ptr, error_code);
            if (error_code) {
                ws_conn->terminate(error_code);
                fatal_error("Could not establish connection to server: %s", error_code.message().c_str());
            }

            ws_conn->add_subprotocol(subprotocol);
            client.connect(ws_conn);

            // R itself is built with MinGW, and links to msvcrt.dll, so it uses the latter's exit() to terminate the main loop.
            // To ensure that our code runs during shutdown, we need to use the corresponding atexit().
            msvcrt::atexit(atexit_handler);

            client.run();
        }

        void server_thread_func(const boost::asio::ip::tcp::endpoint& endpoint) {
        }

        void register_atexit_handler() {
            // R itself is built with MinGW, and links to msvcrt.dll, so it uses the latter's exit() to terminate the main loop.
            // To ensure that our code runs during shutdown, we need to use the corresponding atexit().
            msvcrt::atexit(atexit_handler);
        }

        std::future<void> wait_for_client(const boost::asio::ip::tcp::endpoint& endpoint) {
            register_atexit_handler();
            main_thread_id = GetCurrentThreadId();
            std::thread([&] {
                __try {
                    [&] { server_worker(endpoint); } ();
                } __finally {
                    flush_log();
                }
            }).detach();
            return connected_promise.get_future();
        }

        std::future<void> connect_to_server(const websocketpp::uri& uri) {
            register_atexit_handler();
            main_thread_id = GetCurrentThreadId();
            std::thread([&] {
                __try {
                    [&] { client_worker(uri); }();
                } __finally {
                    flush_log();
                }
            }).detach();
            return connected_promise.get_future();
        }

        void register_callbacks(structRstart& rp) {
            rp.ReadConsole = R_ReadConsole;
            rp.WriteConsoleEx = WriteConsoleEx;
            rp.CallBack = CallBack;
            rp.ShowMessage = ShowMessage;
            rp.YesNoCancel = YesNoCancel;
            rp.Busy = Busy;
        }

        extern "C" void ShowMessage(const char* s) {
            with_cancellation([&] {
                send_notification("!ShowMessage", to_utf8_json(s));
            });
        }

        int ShowMessageBox(const char* s, const char* cmd) {
            return with_cancellation([&] {
                if (!allow_callbacks) {
                    Rf_error("ShowMessageBox: blocking callback not allowed during evaluation.");
                }

                auto msg = send_request_and_get_response(cmd, get_context(), to_utf8_json(s));
                if (msg.args.size() != 1 || !msg.args[0].is<std::string>()) {
                    fatal_error("ShowMessageBox: response argument must be a string.");
                }

                auto& r = msg.args[0].get<std::string>();
                if (r == "N") {
                    return -1; // graphapp.h => NO
                } else if (r == "C") {
                    return 0; // graphapp.h => CANCEL
                } else if (r == "Y") {
                    return 1; // graphapp.h => YES
                } else if (r == "O") {
                    return 1; // graphapp.h => YES
                } else {
                    fatal_error("ShowMessageBox: response argument must be 'Y', 'N' or 'C'.");
                }
            });
        }

        extern "C" int YesNoCancel(const char* s) {
            return ShowMessageBox(s, "?YesNoCancel");
        }

        extern "C" int YesNo(const char* s) {
            return ShowMessageBox(s, "?YesNo");
        }

        extern "C" int OkCancel(const char* s) {
            return ShowMessageBox(s, "?OkCancel");
        }
    }
}

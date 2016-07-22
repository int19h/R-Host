#pragma once
#include "stdafx.h"
#include "blobs.h"

namespace rhost {
    namespace protocol {
        typedef uint64_t message_id;

        struct message_repr {
            boost::endian::little_uint64_buf_t id, request_id;
            char data[];
        };

        class message {
        public:
            static const message_id request_marker = std::numeric_limits<message_id>::max();

            message() : _id(0), _request_id(0), _name(nullptr), _json(nullptr), _blob(nullptr) {
            }

            static message parse(std::string&& payload);

            static message parse(const std::string& payload) {
                return parse(std::string(payload));
            }

            message_id id() const {
                return _id;
            }

            message_id request_id() const {
                return _request_id;
            }

            bool is_notification() const {
                return request_id() == 0;
            }

            bool is_request() const {
                return request_id() == request_marker;
            }

            bool is_response() const {
                return !is_notification() && !is_request();
            }

            const char* name() const {
                return _name;
            }

            const char* blob_data() const {
                return _blob;
            }

            size_t blob_size() const {
                return _payload.size() - (_blob - &_payload[0]);
            }

            blobs::blob blob() const {
                return blobs::blob(blob_data(), blob_data() + blob_size());
            }

            picojson::array json() const;

        private:

            message_id _id;
            message_id _request_id;
            std::string _payload;

            // The following pointers all point inside _payload. _name and _json are guaranteed
            // to be null-terminated. Blob spans from from _blob to end of _payload.
            const char* _name;
            const char* _json;
            const char* _blob;

            message(message_id id, message_id request_id, std::string&& payload, const char* name, const char* json, const char* blob) :
                _id(id), _request_id(request_id), _payload(payload), _name(name), _json(json), _blob(blob) {
            }
        };
    }
}

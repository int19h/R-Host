#include "message.h"
#include "log.h"

namespace rhost {
    namespace protocol {
        message message::parse(std::string&& payload) {
            using namespace boost::endian;
            using namespace rhost::log;

            if (payload.size() < sizeof(message_repr)) {
                fatal_error("Malformed message header - missing IDs");
            }
            auto& repr = *reinterpret_cast<const message_repr*>(&payload[0]);

            const char* p = repr.data;
            const char* end = &payload.back() + 1;

            if (p >= end) {
                fatal_error("Malformed message header - missing name");
            }
            const char* name = p;
            p = reinterpret_cast<const char*>(memchr(p, '\0', end - p));
            if (!p) {
                fatal_error("Malformed message header - missing name terminator");
            }

            if (++p >= end) {
                fatal_error("Malformed message body - missing JSON");
            }
            const char* json = p;
            p = reinterpret_cast<const char*>(memchr(p, '\0', end - p));
            if (!p) {
                fatal_error("Malformed message body - missing JSON terminator");
            }

            const char* blob = ++p;

            return message(repr.id.value(), repr.request_id.value(), std::move(payload), name, json, blob);
        }

        picojson::array message::json() const {
            picojson::value result;

            std::string err = picojson::parse(result, _json);
            if (!err.empty()) {
                log::fatal_error("Malformed JSON payload - %s: %s", err.c_str(), _json);
            }
            if (!result.is<picojson::array>()) {
                log::fatal_error("JSON payload must be an array, but got %s", _json);
            }

            return result.get<picojson::array>();
        }
    }
}

#pragma once
#include <cstdio>
#include "shared/json.hpp"

namespace mc {
namespace demo {

// One demo event: a JSON object on one line, prefixed with '@' so the bridge
// can tell it from log output. The object is closed when the Event goes out of
// scope.
class Event : public JsonWriter {
public:
    Event(FILE* f, const char* type) : JsonWriter(f, false), f_(f) {
        fputc('@', f_);
        begin_object().str("t", type);
    }
    ~Event() {
        end_object();
        fputc('\n', f_);
    }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

private:
    FILE* f_;
};

} // namespace demo
} // namespace mc

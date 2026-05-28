// types.h
//
// Common scalar types and the Status enum that flows through the engine.
// Everything in MicroFi returns Status rather than throwing -- exceptions
// are disabled (see platformio.ini build flags).

#pragma once

#include <cstddef>
#include <cstdint>

namespace microfi {

enum class Status : uint8_t {
    Ok = 0,
    Again,         // not-an-error; retry next tick (e.g. queue empty)
    Full,          // sink could not accept; producer should back off
    InvalidArg,
    OutOfMemory,
    NotFound,
    NotImplemented,
    IoError,
    Internal,
    ParseError,    // malformed or unrecognised input (e.g. bad JSON from EFM)
};

constexpr const char* to_string(Status s) {
    switch (s) {
        case Status::Ok:             return "Ok";
        case Status::Again:           return "Again";
        case Status::Full:            return "Full";
        case Status::InvalidArg:      return "InvalidArg";
        case Status::OutOfMemory:     return "OutOfMemory";
        case Status::NotFound:        return "NotFound";
        case Status::NotImplemented:  return "NotImplemented";
        case Status::IoError:         return "IoError";
        case Status::Internal:        return "Internal";
        case Status::ParseError:      return "ParseError";
    }
    return "?";
}

}  // namespace microfi

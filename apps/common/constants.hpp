#pragma once

#include "feed/moldudp64/wire.hpp"

#include <cstdint>

namespace apps::defaults {

inline constexpr const char* kGroup = "239.1.1.1";
inline constexpr uint16_t kPort = 30001;
inline constexpr const char* kIface = "127.0.0.1";
inline constexpr uint16_t kRerequestPort = 30002;
inline constexpr ttt::mold::SessionId kSession{'T', 'T', 'T', '0', '0', '0', '0', '0', '0', '1'};

} // namespace apps::defaults

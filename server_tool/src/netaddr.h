// This machine's IPv4 addresses (what a player on the LAN types to connect), loopback last.
#pragma once

#include <string>
#include <vector>

namespace tool {

struct LocalAddress {
    std::string address, interface_name;
    bool loopback = false;
};
std::vector<LocalAddress> LocalAddresses();

}  // namespace tool

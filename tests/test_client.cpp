#include "L3KVG/RemoteL3KVClient.hpp"
#include <iostream>

int main() {
    try {
        std::cout << "Attempting to create RemoteL3KVClient...\n";
        l3kvg::RemoteL3KVClient client;
        std::cout << "Success!\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    return 0;
}

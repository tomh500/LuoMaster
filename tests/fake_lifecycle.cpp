#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const char* base = std::getenv("LUO_TEST_LIFECYCLE");
    if (!base) return 2;
    const std::filesystem::path root(base);
    const std::string action(argv[1]);
    const std::string node(argv[2]);
    const auto running = root / (node + ".running");
    const auto exists = std::filesystem::exists(running);
    if (std::filesystem::exists(root / (action + "-" + node + ".fail"))) return 2;
    if (action == "status") return exists ? 0 : 3;
    if (action == "save" && !exists) return 2;
    if (action == "stop") std::filesystem::remove(running);
    if (action == "start") std::ofstream(running).put('\n');
    std::ofstream(root / "actions", std::ios::app) << action << ' ' << node << '\n';
    if (std::filesystem::exists(root / (action + "-" + node + ".pause")))
        std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    return 0;
}

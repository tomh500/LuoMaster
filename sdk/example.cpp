#include <luo/master_client.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: luo-master-client PORT TOKEN_FILE topology | status ID | submit ID PROFILE\n";
        return 2;
    }
    try {
        std::ifstream input{std::filesystem::path(argv[2])};
        std::string token;
        if (!std::getline(input, token)) throw std::runtime_error("cannot read token file");
        if (!token.empty() && token.back() == '\r') token.pop_back();
        if (token.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("invalid token");
        luo::sdk::MasterClient client(std::stoi(argv[1]), token);
        luo::sdk::Response response{};
        const std::string action(argv[3]);
        if (action == "topology" && argc == 4) response = client.topology();
        else if (action == "status" && argc == 5) response = client.status(argv[4]);
        else if (action == "submit" && argc == 6) response = client.submit(argv[4], argv[5]);
        else return 2;
        std::cout << response.body.dump(2) << '\n';
        return response.status == 200 || response.status == 202 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

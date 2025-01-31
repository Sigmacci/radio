#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#define SONGS_DIR "songs/"
#define PORT 8080
#define DECODE_LATENCY 100
#define SOUND_CHUNK 16000

std::mutex clients_mutex;
std::vector<int> clients;

std::string urlDecode(const std::string& encoded) {
    std::ostringstream decoded;
    size_t len = encoded.length();

    for (size_t i = 0; i < len; ++i) {
        if (encoded[i] == '%' && i + 2 < len) {
            std::istringstream hexStream(encoded.substr(i + 1, 2));
            int hexValue;
            hexStream >> std::hex >> hexValue;
            decoded << static_cast<char>(hexValue);
            i += 2;
        } else if (encoded[i] == '+') {
            decoded << ' ';
        } else {
            decoded << encoded[i];
        }
    }

    return decoded.str();
}

std::string get_song_list() {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& entry : fs::directory_iterator(SONGS_DIR)) {
        if (fs::is_regular_file(entry)) {
            if (!first) oss << ",";
            oss << "\"" << entry.path().filename().string() << "\"";
            first = false;
        }
    }
    oss << "]";
    return oss.str();
}

int getBitrate(const std::string& filename) {
    const int BITRATES[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1};

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file." << std::endl;
        return -1;
    }

    uint8_t buffer[4];
    while (file.read(reinterpret_cast<char*>(buffer), 4)) {
        if ((buffer[0] == 0xFF) && ((buffer[1] & 0xE0) == 0xE0)) {
            int bitrateIndex = (buffer[2] >> 4) & 0x0F;
            if (bitrateIndex == 0xF) {
                std::cerr << "Invalid bitrate index" << std::endl;
                return -1;
            }
            return BITRATES[bitrateIndex] * 1000;
        }
    }

    std::cerr << "No valid MP3 frame found." << std::endl;
    return -1;
}

void accept_audio_listener(int server_fd, struct sockaddr_in address) {
    while (true) {
        socklen_t addrlen = sizeof(address);
        int client_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_socket != -1) {
            std::cout << "Audio client connected" << std::endl;

            char buffer[1024];
            ssize_t bytes_received = read(client_socket, buffer, sizeof(buffer));
            if (bytes_received <= 0) {
                close(client_socket);
                return;
            }

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n";
            send(client_socket, response.str().c_str(), response.str().length(), 0);
            std::cout << "Sent response to audio client" << std::endl;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                clients.push_back(client_socket);
            }
            std::cout << "Added audio client to list: " << client_socket << std::endl;
        }
    }
}

void audio_server_loop() {
    int opt = 1;
    int server_fd_audio;
    struct sockaddr_in address_audio;
    server_fd_audio = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd_audio, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address_audio.sin_family = AF_INET;
    address_audio.sin_addr.s_addr = INADDR_ANY;
    address_audio.sin_port = htons(PORT + 1);
    bind(server_fd_audio, (struct sockaddr*)&address_audio, sizeof(address_audio));
    std::cout << "Listening for audio connections on port: " << PORT + 1 << std::endl;

    listen(server_fd_audio, 3);

    std::thread accept_audio_thread(accept_audio_listener, server_fd_audio, address_audio);
    accept_audio_thread.detach();

    while (true) {
        for (const auto& song : fs::directory_iterator(SONGS_DIR)) {
            if (fs::is_regular_file(song)) {
                std::string song_path = song.path().string();
                std::cout << "Playing " << song_path << std::endl;
                int bitrate = getBitrate(song_path);
                std::cout << "Bit rate: " << bitrate << std::endl;

                std::ifstream file(song_path, std::ios::in | std::ios::binary);
                char buffer[bitrate];
                while (file.read(buffer, bitrate)) {
                    int n = file.gcount();
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        for (int client_socket : clients) {
                            if (send(client_socket, buffer, n, 0) == -1) {
                                std::cout << "Client disconnected" << std::endl;
                                clients.erase(std::remove(clients.begin(), clients.end(), client_socket), clients.end());
                            }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 * 8));
                }
                std::cout << "Sent to all clients" << std::endl;
                file.close();
            }
        }
    }
}

void handle_request(int client_socket) {
    char buffer[2048] = {0};  // Increased buffer size for larger requests
    ssize_t bytes_received = read(client_socket, buffer, sizeof(buffer));
    if (bytes_received <= 0) {
        close(client_socket);
        return;
    }
    std::string request(buffer);

    if (request.find("GET /songs") != std::string::npos) {
        std::cout << "Sending song list" << std::endl;
        std::string song_list = get_song_list();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nContent-Length: "
                 << song_list.length() << "\r\n\r\n"
                 << song_list;

        send(client_socket, response.str().c_str(), response.str().length(), 0);
    } else {
        std::string not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(client_socket, not_found.c_str(), not_found.length(), 0);
    }

    close(client_socket);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    std::cout << "Server started" << std::endl;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    std::cout << "Listening on port " << PORT << std::endl;

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);
    std::cout << "Listening for connections" << std::endl;

    std::thread audio_server_thread(audio_server_loop);
    audio_server_thread.detach();

    while (true) {
        socklen_t addrlen = sizeof(address);
        int client_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_socket == -1) {
            std::cerr << "Couldn't accept connection" << std::endl;
        }
        std::cout << "Connection accepted" << std::endl;
        std::thread(handle_request, client_socket).detach();
    }
}
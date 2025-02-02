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
std::mutex queue_mutex;
std::mutex skip_mutex;
std::vector<int> clients;
std::vector<std::string> queue;
bool skip = false;

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

void load_song_names() {
    for (const auto& entry : fs::directory_iterator(SONGS_DIR)) {
        if (fs::is_regular_file(entry)) {
            queue.push_back(entry.path().filename().string());
            std::cout << "Loaded " << entry.path().filename() << std::endl;
        }
    }
}

std::string get_song_list() {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        for (const auto& entry : queue) {
            if (!first) oss << ",";
            oss << "\"" << entry << "\"";
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
            char buffer[1024];
            ssize_t bytes_received = read(client_socket, buffer, sizeof(buffer));
            if (bytes_received <= 0) {
                close(client_socket);
                return;
            }

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n";
            send(client_socket, response.str().c_str(), response.str().length(), 0);
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                clients.push_back(client_socket);
            }
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
        std::string song = SONGS_DIR;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (queue.empty()) {
                continue;
            }
            std::string song_name = queue.front();
            song += song_name;
            queue.erase(queue.begin());
            queue.push_back(song_name);
        }
        if (fs::is_regular_file(song)) {
            std::cout << "Playing " << song << std::endl;
            int bitrate = getBitrate(song);
            if (bitrate == -1) {
                bitrate = 192000;
            }
            std::cout << "Bit rate: " << bitrate << std::endl;
            bitrate /= 8;

            std::ifstream file(song, std::ios::in | std::ios::binary);
            char buffer[bitrate];
            while (file.read(buffer, bitrate)) {
                {
                    std::lock_guard<std::mutex> lock(skip_mutex);
                    if (skip) {
                        skip = false;
                        break;
                    }
                }
                int n = file.gcount();
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    for (auto i = clients.begin(); i != clients.end();) {
                        ssize_t byt = recv(*i, nullptr, 0, MSG_PEEK | MSG_DONTWAIT);
                        if (byt == 0) {
                            close(*i);
                            i = clients.erase(i);
                            continue;
                        }
                        send(*i, buffer, n, 0);
                        ++i;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
            file.close();
        }
    }
}

void handle_request(int client_socket) {
    char buffer[2048] = {0};
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
    } else if (request.find("POST /upload") != std::string::npos) {
        std::cout << "Uploading song" << std::endl;

        // Boundary
        std::string boundary;
        size_t boundary_pos = request.find("boundary=");
        if (boundary_pos != std::string::npos) {
            boundary = "--" + request.substr(boundary_pos + 9);
            boundary = boundary.substr(0, boundary.find("\r\n"));
        } else {
            std::cerr << "No boundary" << std::endl;
            close(client_socket);
            return;
        }

        // Filename
        size_t filename_pos = request.find("filename=\"");
        if (filename_pos == std::string::npos) {
            std::cerr << "No filename" << std::endl;
            close(client_socket);
            return;
        }
        filename_pos += 10;
        size_t filename_end = request.find("\"", filename_pos);
        std::string filename = request.substr(filename_pos, filename_end - filename_pos);

        // Content start
        size_t file_start = request.find("\r\n\r\n", filename_end);
        if (file_start == std::string::npos) {
            std::cerr << "Start not found" << std::endl;
            close(client_socket);
            return;
        }
        file_start += 4;

        // Content length
        size_t content_lenght_pos = request.find("Content-Length: ");
        if (content_lenght_pos == std::string::npos) {
            std::cerr << "Content length not found" << std::endl;
            close(client_socket);
            return;
        }
        content_lenght_pos += 16;
        size_t content_length_end = request.find("\r\n", content_lenght_pos);
        std::string content_length_str = request.substr(content_lenght_pos, content_length_end - content_lenght_pos);
        size_t content_length = std::stoul(content_length_str);

        bytes_received -= file_start;
        content_length -= bytes_received;
        while (content_length > 0) {
            bytes_received = recv(client_socket, buffer, std::min(content_length, sizeof(buffer)), MSG_DONTWAIT);
            if (bytes_received <= 0) {
                break;
            }
            request += std::string(buffer, bytes_received);
            content_length -= bytes_received;
        }

        // EOF
        size_t file_end = request.find(boundary, file_start);
        if (file_end == std::string::npos) {
            std::cerr << "End not found" << std::endl;
            close(client_socket);
            return;
        }
        file_end -= 2;

        std::string file_content = request.substr(file_start, file_end - file_start);
        std::ofstream file(SONGS_DIR + filename, std::ios::binary);
        if (!file) {
            std::cerr << "File failed" << std::endl;
            close(client_socket);
            return;
        }
        file.write(file_content.c_str(), file_content.size());
        file.close();

        std::cout << "File saved: " << filename << std::endl;

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.insert(queue.begin(), filename);
        }

        std::string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, response.c_str(), response.length(), 0);
    } else if (request.find("GET /skip") != std::string::npos) {
        std::cout << "Skipping song" << std::endl;
        {
            std::lock_guard<std::mutex> lock(skip_mutex);
            skip = true;
        }
        std::string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
        send(client_socket, response.c_str(), response.length(), 0);
    } else if (request.find("GET /delete/") != std::string::npos) {
        size_t pos = request.find("GET /delete/") + 12;
        std::string song_name = request.substr(pos, request.find(" ", pos) - pos);
        song_name = urlDecode(song_name);
        std::cout << "Deleting song " << song_name << std::endl;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (queue.back() == song_name) {
                {
                    std::lock_guard<std::mutex> lock(skip_mutex);
                    skip = true;
                }
            }
            queue.erase(std::remove(queue.begin(), queue.end(), song_name), queue.end());
        }
        std::string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
        send(client_socket, response.c_str(), response.length(), 0);
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

    load_song_names();
    std::cout << "Loaded " << queue.size() << " songs" << std::endl;

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
            continue;
        }
        std::cout << "Connection accepted" << std::endl;
        std::thread(handle_request, client_socket).detach();
    }
}
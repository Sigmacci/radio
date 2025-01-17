#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <netinet/in.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <fcntl.h>
#include <thread>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <sstream>
#include <iomanip>

#define CONNECTION_QUEUE_LENGTH 50
#define BUFFER_SIZE 256
#define CHUNK_SIZE 8000
#define SONGS_DIR "songs/"
#define HEADAERS "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n"

#define SERVER_PORT 1234
#define CLIENT_PORT 1235

class Client
{
public:
    explicit Client(int fd) : send_fd(fd)
    {
        song_names.insert(std::make_pair("song1", "songs/aye.mp3"));
    }
    int send_fd;
    std::unordered_map<int, bool> clients;
    std::queue<std::string> awaiting_songs;                   // queue of songs to be played
    std::unordered_map<std::string, std::string> song_names;  // maps song names to file names
    std::queue<std::pair<std::string, std::string>> commands; // queue of pairs command-argument
    std::string current_song_name;

    std::unique_ptr<std::ifstream> current_song;

    bool playing = false;
};

class ClientHandler
{
public:
    explicit ClientHandler(int fd) : client(fd) {}

    void handle_client()
    {
        // load_song_names(client.song_names);

        std::thread client_thread(&ClientHandler::accept_clients_thread, this);
        client_thread.detach();
        while (true)
        {
            if (!client.playing && !client.awaiting_songs.empty())
            {
                client.current_song_name = client.awaiting_songs.front();
                client.awaiting_songs.pop();
                client.playing = true;
                std::cout << client.current_song_name << std::endl;
            }

            if (client.playing)
            {
                if (!client.current_song)
                {
                    client.current_song = std::make_unique<std::ifstream>(client.current_song_name, std::ios::in | std::ios::binary);
                }
                client.playing = play_song();
            }

            {
                std::unique_lock<std::mutex> lock(queue_mutex);

                if (client.commands.empty())
                {
                    lock.unlock();
                    continue;
                }
                std::pair<std::string, std::string> current_cmd = client.commands.front();
                client.commands.pop();
                queue_cv.notify_one();

                auto cmd = current_cmd.first.c_str();

                if (strcmp(cmd, "PLAY") == 0)
                {
                    add_song_to_queue(current_cmd.second);
                    std::cout << "Added song to queue" << std::endl;
                }
                else if (strcmp(cmd, "SKIP") == 0)
                {
                    client.current_song.reset();
                }
                else if (strcmp(cmd, "QUEUE") == 0)
                {
                    show_queue();
                }
                else if (strcmp(cmd, "END") == 0)
                {
                    close(client.send_fd);
                    return;
                }
                else if (strcmp(cmd, "UPLOAD") == 0)
                {
                    // upload_song(fd, &(current_cmd.second), &song_names);
                }
                else if (strcmp(cmd, "") != 0)
                {
                    // unknown_cmd();
                }
            }
        }
    }

private:
    Client client;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    std::mutex clients_mutex;
    std::condition_variable clients_cv;

    void command_thread(int fd)
    {
        while (true)
        {
            char buffer[BUFFER_SIZE];
            bzero(buffer, BUFFER_SIZE);
            int bytes_received = recv(fd, buffer, BUFFER_SIZE, 0);
            if (bytes_received == -1)
            {
                perror("Couldn't receive command");
                return;
            }
            else if (bytes_received == 0)
            {
                std::cout << "Client disconnected" << std::endl;
                break;
            }

            std::string cmd(buffer);
            cmd = cmd.substr(0, bytes_received);

            if (strcmp(cmd.c_str(), "ACK") == 0)
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                client.clients.at(fd) = true;
                clients_cv.notify_one();
                continue;
            }

            std::string arg = "";
            if (cmd.find(" ") != std::string::npos)
            {
                arg = cmd.substr(cmd.find(" ") + 1);
                cmd = cmd.substr(0, cmd.find(" "));
            }

            std::cout << "Received command: " << cmd << " " << arg << std::endl;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                client.commands.push(std::make_pair(cmd, arg));
                queue_cv.notify_one();
            }
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            client.clients.erase(fd);
            clients_cv.notify_one();
        }
    }

    void accept_clients_thread()
    {
        int cfd;
        while (true)
        {
            cfd = accept(client.send_fd, NULL, NULL);

            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                client.clients.insert(std::make_pair(cfd, true));
                clients_cv.notify_one();
            }

            if (cfd == -1)
            {
                perror("Couldn't accept connection");
                return;
            }
            std::thread cmd_thread(&ClientHandler::command_thread, this, cfd);
            cmd_thread.detach();
        }
    }

    void add_song_to_queue(const std::string &song_name)
    {
        if (client.song_names.find(song_name) == client.song_names.end())
        {
            perror("Song not found");
            return;
        }
        client.awaiting_songs.push(client.song_names.at(song_name));
    }

    void print_hex(const uint8_t *buffer, int length)
    {
        for (int i = 0; i < length; ++i)
        {
            printf("%02x ", buffer[i]);
        }
    }

    bool play_song()
    {
        uint8_t buffer[CHUNK_SIZE];

        if (client.current_song->eof())
        {
            client.current_song.reset();
            return false;
        }

        client.current_song->read(reinterpret_cast<char *>(buffer), CHUNK_SIZE);
        // client.current_song->read(buffer, CHUNK_SIZE);
        // print_hex(buffer, CHUNK_SIZE);
        int n = client.current_song->gcount();

        {
            std::unique_lock<std::mutex> lock(clients_mutex);
            for (auto &client_addr : client.clients)
            {
                clients_cv.wait(lock, [&] {
                    return client_addr.second; 
                });
                if (send(client_addr.first, buffer, n, 0) == -1)
                {
                    perror("Couldn't send song chunk");
                    return false;
                }
                clients_cv.notify_one();
                client_addr.second = false;
            }
        }

        return true;
    }

    void show_queue()
    {
        int i = 1;
        std::queue<std::string> temp_queue = client.awaiting_songs;
        std::string queue_str = "";
        while (!temp_queue.empty())
        {
            queue_str += std::to_string(i++) + ". " + temp_queue.front() + "\n";
            temp_queue.pop();
        }

        for (auto &client_addr : client.clients)
        {
            if (sendto(client.send_fd, queue_str.c_str(), queue_str.size(), 0, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1)
            {
                perror("Couldn't send queue");
            }
        }
    }
};

int main()
{
    int sfd;

    sfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sfd == -1)
    {
        perror("Couldn't create socket");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    std::cout << "Server started" << std::endl;

    if (bind(sfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("Couldn't bind socket");
        exit(1);
    }

    if (listen(sfd, CONNECTION_QUEUE_LENGTH) == -1)
    {
        perror("Couldn't listen on socket");
        exit(1);
    }

    sleep(2);

    ClientHandler handler(sfd);
    handler.handle_client();

    close(sfd);
    return 0;
}

void load_song_names(std::unordered_map<std::string, std::string> &song_names)
{
    return;
}

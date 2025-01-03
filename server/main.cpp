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

#define CONNECTION_QUEUE_LENGTH 50
#define BUFFER_SIZE 256
#define CHUNK_SIZE 1024
#define SONGS_DIR "songs/"

#define SERVER_PORT 1234
#define CLIENT_PORT 1235
#define BROADCAST_PORT 1236

class Client
{
public:
    explicit Client(int fd) : send_fd(fd)
    {
        song_names.insert(std::make_pair("song1", "songs/aye.mp3"));
    }
    int send_fd;
    int communication_fd;
    struct sockaddr_in broadcast_addr;
    std::queue<std::string> awaiting_songs;                      // queue of songs to be played
    std::unordered_map<std::string, std::string> song_names;  // maps song names to file names
    std::queue<std::pair<std::string, std::string>> commands; // queue of pairs command-argument
    std::string current_song_name;
    std::unique_ptr<std::ifstream> current_song;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    bool playing = false;
};


int main(int argc, char *argv[])
{
    int sfd;

    sfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sfd == -1)
    {
        perror("Couldn't create socket");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = atoi(argv[2]);
    server_addr.sin_addr.s_addr = argv[1];

    if (bind(sfd, &server_addr, sizeof server_addr) == -1)
    {
        perror("Couldn't bind socket");
        exit(1);
    }

    if (listen(sfd, CONNECTION_QUEUE_LENGTH) == -1)
    {
        perror("Couldn't listen on socket");
    }

    int cfd;

    while (1)
    {
        cfd = accept(sfd, NULL, NULL);
        if (cfd == -1)
        {
            perror("Couldn't accept client");
            continue;
        }

        if (fork() == 0) 
        {
            handle_client(cfd);
        }
    }

    close(sfd);
}

void load_song_names(std::unordered_map<std::string, std::string> *song_names)
{
    return;
}

void handle_client(int fd) 
{
    std::queue<std::string> awaiting_songs; // queue of songs to be played
    std::unordered_map<std::string, std::string> song_names; // maps song names to file names
    load_song_names(&song_names);
    
    std::queue<std::pair<std::string, std::string>> commands; // queue of pairs command-argument

    char buffer[BUFFER_SIZE];
    while (1)
    {
        // while (1){}
        std::pair<std::string, std::string> current_cmd = commands.pop();

        switch (current_cmd.first)
        {
            case "PLAY":
                // send_song(fd, &(current_cmd.second), &awaiting_songs, &song_names);
                break;
            case "SKIP":
                // skip_song(&awaiting_songs);
                break;
            case "QUEUE":
                // show_queue(fd, &awaiting_songs);
                break;
            case "END":
                close(fd);
                return;
            default:
                // no_args();
                break;
        }    
    }
}
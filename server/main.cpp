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
	std::queue<std::string> awaiting_songs;					  // queue of songs to be played
	std::unordered_map<std::string, std::string> song_names;  // maps song names to file names
	std::queue<std::pair<std::string, std::string>> commands; // queue of pairs command-argument
	std::string current_song_name;
	std::unique_ptr<std::ifstream> current_song;
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	bool playing = false;
};

void receive_cmds(Client &client);
void add_song_to_queue(const std::string &song_name, Client &client);
bool play_song(Client &client);
void show_queue(Client &client);
void handle_client(int fd);

int main()
{
    int sfd;

	sfd = socket(PF_INET, SOCK_DGRAM, 0);
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

	sleep(2);

	handle_client(sfd);

	close(sfd);
	return 0;
}

void load_song_names(std::unordered_map<std::string, std::string> &song_names)
{
    return;
}

void receive_cmds(Client &client)
{
	char buffer[BUFFER_SIZE];

	int cfd;
	while (true)
	{
		cfd = accept(client.communication_fd, NULL, NULL);
		if (cfd == -1)
		{
			perror("Couldn't accept connection");
			return;
		}

		bzero(buffer, BUFFER_SIZE);
		int bytes_received = recv(cfd, buffer, BUFFER_SIZE, 0);
		if (bytes_received == -1)
		{
			perror("Couldn't receive command");
			return;
		}
		close(cfd);

		std::string cmd(buffer);
		cmd = cmd.substr(0, bytes_received);
		std::string arg = "";
		if (cmd.find(" ") != std::string::npos)
		{
			arg = cmd.substr(cmd.find(" ") + 1);
			cmd = cmd.substr(0, cmd.find(" "));
		}

		{
			std::lock_guard<std::mutex> lock(client.queue_mutex);
			client.commands.push(std::make_pair(cmd, arg));
			client.queue_cv.notify_one();
		}
	}

}

void add_song_to_queue(const std::string &song_name, Client &client)
{
	if (client.song_names.find(song_name) == client.song_names.end())
	{
		perror("Song not found");
		return;
	}
	client.awaiting_songs.push(client.song_names.at(song_name));
}

bool play_song(Client &client)
{
	char buffer[CHUNK_SIZE];

	if (client.current_song->eof())
	{
		client.current_song.reset();
		return false;
	}

	client.current_song->read(buffer, CHUNK_SIZE);
	int n = client.current_song->gcount();
	std::cout << "Sending " << n << " bytes" << std::endl;

	if (sendto(client.send_fd, buffer, n, 0, (struct sockaddr *) &(client.broadcast_addr), sizeof(client.broadcast_addr)) == -1)
	{
		perror("Couldn't send song chunk");
		return false;
	}

	return true;
}

void show_queue(Client &client)
{
	int i = 1;
	std::queue<std::string> temp_queue = client.awaiting_songs;
	std::string queue_str = "";
	while (!temp_queue.empty())
	{
		queue_str += std::to_string(i++) + ". " + temp_queue.front() + "\n";
		temp_queue.pop();
	}

	if (sendto(client.send_fd, queue_str.c_str(), queue_str.size(), 0, NULL, 0) == -1)
	{
		perror("Couldn't send queue");
	}
}

void handle_client(int fd)
{
	Client client(fd);
	
	client.broadcast_addr.sin_family = AF_INET;
	client.broadcast_addr.sin_port = htons(BROADCAST_PORT);
	client.broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
	load_song_names(client.song_names);

	client.communication_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (client.communication_fd == -1)
	{
		perror("Couldn't create communication socket");
		return;
	}

	struct sockaddr_in communication_addr;
	communication_addr.sin_family = AF_INET;
	communication_addr.sin_port = htons(CLIENT_PORT);
	communication_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(client.communication_fd, (struct sockaddr *)&communication_addr, sizeof(communication_addr)) == -1)
	{
		perror("Couldn't bind communication socket");
		return;
	}

	if (listen(client.communication_fd, CONNECTION_QUEUE_LENGTH) == -1)
	{
		perror("Couldn't listen on communication socket");
		return;
	}

	std::thread cmd_thread(receive_cmds, std::ref(client));
	cmd_thread.detach();
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
				client.current_song = std::make_unique<std::ifstream>(client.current_song_name, std::ios::binary);
			}
			client.playing = play_song(client);
		}

		if (client.commands.empty())
		{
			continue;
		}
		std::pair<std::string, std::string> current_cmd = client.commands.front();
		client.commands.pop();

		auto cmd = current_cmd.first.c_str();

		if (strcmp(cmd, "PLAY") == 0)
		{
			add_song_to_queue(current_cmd.second, client);
			std::cout << "Added song to queue" << std::endl;
		}
		else if (strcmp(cmd, "SKIP") == 0)
		{
			client.current_song.reset();
		}
		else if (strcmp(cmd, "QUEUE") == 0)
		{
			show_queue(client);
		}
		else if (strcmp(cmd, "END") == 0)
		{
			close(client.communication_fd);
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
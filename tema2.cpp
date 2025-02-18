#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <set>
#include <utility>
#include <iostream>
#include <algorithm>  

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILENAME 15
#define HASH_SIZE 33
#define MAX_CHUNKS 100
#define MSG_SIZE 5

#define MSG_READY_TO_START "REDY"
#define MSG_TRACKER_FINISH "TFIN"

#define STATUS_SUCCESS 1
#define STATUS_FAILURE 0

#define TAG_INIT              1
#define TAG_FILE_REQUEST      2
#define TAG_FINISH            3
#define TAG_UPLOAD            4
#define TAG_HASH_REQUEST      5
#define TAG_HASH_RESPONSE     6
#define TAG_DOWNLOAD_COMPLETE 7
#define TAG_SWARM_UPDATE     8

#define CLIENT_SEED 1
#define CLIENT_PEER 2

struct SwarmClient {
    int rank;
    int type;
    char filename[MAX_FILENAME];

    bool operator<(const SwarmClient& other) const {
        if (rank != other.rank) 
            return rank < other.rank;
        if (type != other.type)
            return type < other.type;
        return strcmp(filename, other.filename) < 0;
    }
};

unordered_map<string, int> next_owner_index;
unordered_map<string, vector<string>> files_available;
unordered_map<string, vector<string>> achived_files;

set<SwarmClient> swarm;
set<string> wanted_files;

void read_init_file(int rank) {
    char filename[MAX_FILENAME];
    memset(filename, 0, MAX_FILENAME);
    snprintf(filename, sizeof(filename), "in%d.txt", rank);
    
    FILE* file = fopen(filename, "r");
    if (!file) {
        return;
    }

    int no_fields_held;
    fscanf(file, "%d", &no_fields_held);

    for (int i = 0; i < no_fields_held; i++) {
        char actual_held_file[MAX_FILENAME];
        memset(actual_held_file, 0, MAX_FILENAME);

        int no_segments_held_file;
        fscanf(file, "%s %d", actual_held_file, &no_segments_held_file);

        vector<string> hashes;
        hashes.reserve(no_segments_held_file);
        for (int j = 0; j < no_segments_held_file; j++) {
            char hash[HASH_SIZE];
            memset(hash, 0, HASH_SIZE);
            fscanf(file, "%s", hash);
            hashes.push_back(string(hash));
        }
        achived_files[string(actual_held_file)] = hashes;
    }

    int no_wanted_files;
    fscanf(file, "%d", &no_wanted_files);
    for (int i = 0; i < no_wanted_files; i++) {
        char wanted_file[MAX_FILENAME];
        fscanf(file, "%s", wanted_file);
        wanted_files.insert(string(wanted_file));
    }

    fclose(file);
}

void write_output_file(int rank, const vector<string>& hashes, const string& filename) {
    char out_filename[MAX_FILENAME];
    memset(out_filename, 0, MAX_FILENAME);
    snprintf(out_filename, sizeof(out_filename), "client%d_%s", rank, filename.c_str());
    
    FILE* file = fopen(out_filename, "w");
    if (!file) {
        return;
    }

    for (size_t i = 0; i < hashes.size(); i++) {
        fprintf(file, "%s", hashes[i].c_str());
        if (i < hashes.size() - 1) {
            fprintf(file, "\n");
        }
    }

    fclose(file);
}

int get_next_owner(const string& filename, const vector<int>& owners, int selfRank) {
   if (owners.empty()) {
       return -1;
   }
   
   if (next_owner_index.find(filename) == next_owner_index.end()) {
       next_owner_index[filename] = 0;
   }

   for (int i = 0; i < owners.size(); i++) {
       int index = (next_owner_index[filename] + i) % owners.size();
       int candidate = owners[index];
       
       if (candidate != selfRank) {
           next_owner_index[filename] = (index + 1) % owners.size();
           return candidate;
       }
   }

   return -1;
}

void send_swarm_update(const string& filename, int rank, unordered_map<string, vector<int>>& file_owners) {
    MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TAG_SWARM_UPDATE, MPI_COMM_WORLD);

    int num_clients;
    MPI_Recv(&num_clients, 1, MPI_INT, TRACKER_RANK, TAG_SWARM_UPDATE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    vector<int> clients(num_clients);
    for (int i = 0; i < num_clients; i++) {
        MPI_Recv(&clients[i], 1, MPI_INT, TRACKER_RANK, TAG_SWARM_UPDATE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    file_owners[filename] = clients;
}

void request_file_info(const string& filename, unordered_map<string, vector<int>>& file_owners, unordered_map<string, vector<string>>& file_hashes) {
    MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TAG_FILE_REQUEST, MPI_COMM_WORLD);

    int num_owners;
    MPI_Recv(&num_owners, 1, MPI_INT, TRACKER_RANK, TAG_FILE_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    vector<int> owners(num_owners);
    for (int i = 0; i < num_owners; i++) {
        MPI_Recv(&owners[i], 1, MPI_INT, TRACKER_RANK, TAG_FILE_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    file_owners[filename] = owners;

    int num_hashes;
    MPI_Recv(&num_hashes, 1, MPI_INT, TRACKER_RANK, TAG_FILE_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    vector<string> hashes(num_hashes);
    for (int i = 0; i < num_hashes; i++) {
        char hash[HASH_SIZE] = {0};
        MPI_Recv(hash, HASH_SIZE-1, MPI_CHAR, TRACKER_RANK, TAG_FILE_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        hashes[i] = string(hash);
    }
    file_hashes[filename] = hashes;
}

bool download_segment(const string& filename, const string& hash, int rank, unordered_map<string, vector<int>>& file_owners, vector<string>& downloaded_hashes) {
    bool got_segment = false;
    int seg_count = 0;

    while (!got_segment) {
        if (seg_count > 0 && seg_count % 10 == 0) {
            send_swarm_update(filename, rank, file_owners);
        }

        int owner = get_next_owner(filename, file_owners[filename], rank);
        if (owner == -1) {
            break;
        }

        MPI_Send(nullptr, 0, MPI_CHAR, owner, TAG_UPLOAD, MPI_COMM_WORLD);
        MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, owner, TAG_HASH_REQUEST, MPI_COMM_WORLD);
        MPI_Send(hash.c_str(), HASH_SIZE-1, MPI_CHAR, owner, TAG_HASH_REQUEST, MPI_COMM_WORLD);

        char status_code;
        MPI_Recv(&status_code, 1, MPI_CHAR, owner, TAG_HASH_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (status_code == STATUS_SUCCESS) {
            char received_hash[HASH_SIZE] = {0};
            MPI_Recv(received_hash, HASH_SIZE-1, MPI_CHAR, owner, TAG_HASH_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            downloaded_hashes.push_back(string(received_hash));
            got_segment = true;
            seg_count++;
        }
    }
    return got_segment;
}

void download_file(const string& filename, const vector<string>& hashes, int rank, unordered_map<string, vector<int>>& file_owners) {
    vector<string> downloaded_hashes;
    downloaded_hashes.reserve(hashes.size());

    for (const auto& hash : hashes) {
        download_segment(filename, hash, rank, file_owners, downloaded_hashes);
    }
    write_output_file(rank, downloaded_hashes, filename);
}

void* download_thread_func(void* arg) {
    int rank = *((int*)arg);
    unordered_map<string, vector<int>> file_owners;
    unordered_map<string, vector<string>> file_hashes;

    for (const auto& filename : wanted_files) {
        request_file_info(filename, file_owners, file_hashes);
    }

    for (const auto& file_entry : file_hashes) {
        download_file(file_entry.first, file_entry.second, rank, file_owners);
    }

    MPI_Send(nullptr, 0, MPI_CHAR, TRACKER_RANK, TAG_FINISH, MPI_COMM_WORLD);
    return NULL;
}

void handle_upload_request(int source) {
    char filename[MAX_FILENAME];
    memset(filename, 0, MAX_FILENAME);

    char requested_hash[HASH_SIZE];
    memset(requested_hash, 0, HASH_SIZE);
    
    MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, TAG_HASH_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Recv(requested_hash, HASH_SIZE-1, MPI_CHAR, source, TAG_HASH_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    bool has_hash = false;
    auto it = achived_files.find(string(filename));
    if (it != achived_files.end()) {
        const auto& hashes = it->second;
        has_hash = (find(hashes.begin(), hashes.end(), string(requested_hash)) != hashes.end());
    }

    char status_code = has_hash ? STATUS_SUCCESS : STATUS_FAILURE;
    MPI_Send(&status_code, 1, MPI_CHAR, source, TAG_HASH_RESPONSE, MPI_COMM_WORLD);
    
    if (has_hash) {
        MPI_Send(requested_hash, HASH_SIZE-1, MPI_CHAR, source, TAG_HASH_RESPONSE, MPI_COMM_WORLD);
    }
}

void* upload_thread_func(void* arg) {
    int rank = *((int*)arg);
    while (true) {
        MPI_Status status;
        int tag;
        
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        tag = status.MPI_TAG;

        if (tag == TAG_UPLOAD) {
            char message[MSG_SIZE];
            memset(message, 0, MSG_SIZE);
            MPI_Recv(message, MSG_SIZE, MPI_CHAR, status.MPI_SOURCE, TAG_UPLOAD, MPI_COMM_WORLD, &status);
            
            if (strcmp(message, MSG_TRACKER_FINISH) == 0) {
                break;
            }

            handle_upload_request(status.MPI_SOURCE);
        }
    }
    return NULL;
}

void handle_swarm_update(int source) {
    char filename[MAX_FILENAME];
    memset(filename, 0, MAX_FILENAME);
    MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, TAG_SWARM_UPDATE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    SwarmClient new_client = {source, CLIENT_PEER, ""};
    memcpy(new_client.filename, filename, MAX_FILENAME);
    swarm.insert(new_client);

    vector<int> clients;
    for (const auto& client : swarm) {
        if (strcmp(client.filename, filename) == 0) {
            clients.push_back(client.rank);
        }
    }

    int num_clients = clients.size();
    MPI_Send(&num_clients, 1, MPI_INT, source, TAG_SWARM_UPDATE, MPI_COMM_WORLD);
    for (int client : clients) {
        MPI_Send(&client, 1, MPI_INT, source, TAG_SWARM_UPDATE, MPI_COMM_WORLD);
    }
}

void receive_file_info(int client) {
    int num_files;
    MPI_Recv(&num_files, 1, MPI_INT, client, TAG_INIT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for (int i = 0; i < num_files; i++) {
        char filename[MAX_FILENAME];
        memset(filename, 0, MAX_FILENAME);
        MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, client, TAG_INIT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        SwarmClient new_client = {client, CLIENT_SEED, ""};
        memcpy(new_client.filename, filename, MAX_FILENAME);
        swarm.insert(new_client);

        int num_hashes;
        MPI_Recv(&num_hashes, 1, MPI_INT, client, TAG_INIT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        vector<string>& file_hashes = files_available[string(filename)];
        file_hashes.clear();
        file_hashes.reserve(num_hashes);

        for (int j = 0; j < num_hashes; j++) {
            char hash[HASH_SIZE] = {0};
            MPI_Recv(hash, HASH_SIZE-1, MPI_CHAR, client, TAG_INIT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            file_hashes.push_back(string(hash));
        }
    }
}

void send_ready_to_start(int num_tasks) {
    for (int client = 1; client < num_tasks; client++) {
        MPI_Send(MSG_READY_TO_START, strlen(MSG_READY_TO_START) + 1, MPI_BYTE, client, TAG_INIT, MPI_COMM_WORLD);
    }
}

void handle_file_request(int source) {
    char filename[MAX_FILENAME];
    memset(filename, 0, MAX_FILENAME);
    MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, TAG_FILE_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    vector<int> owners;
    for (const auto& client : swarm) {
        if (strcmp(client.filename, filename) == 0) {
            owners.push_back(client.rank);
        }
    }

    int num_owners = owners.size();
    MPI_Send(&num_owners, 1, MPI_INT, source, TAG_FILE_REQUEST, MPI_COMM_WORLD);

    for (int owner : owners) {
        MPI_Send(&owner, 1, MPI_INT, source, TAG_FILE_REQUEST, MPI_COMM_WORLD);
    }

    const auto& file_hashes = files_available[string(filename)];
    int num_hashes = file_hashes.size();
    MPI_Send(&num_hashes, 1, MPI_INT, source, TAG_FILE_REQUEST, MPI_COMM_WORLD);
    for (const auto& h: file_hashes) {
        MPI_Send(h.c_str(), HASH_SIZE-1, MPI_CHAR, source, TAG_FILE_REQUEST, MPI_COMM_WORLD);
    }
}

void handle_finish(int source, int& finished_peers) {
    char finish_msg[MSG_SIZE];
    memset(finish_msg, 0, MSG_SIZE);
    MPI_Recv(finish_msg, MSG_SIZE, MPI_CHAR, source, TAG_FINISH, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    finished_peers++;
}

void handle_download_complete(int source) {
    char filename[MAX_FILENAME];
    memset(filename, 0, MAX_FILENAME);
    MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, TAG_DOWNLOAD_COMPLETE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    SwarmClient new_seed = {source, CLIENT_SEED, ""};
    memcpy(new_seed.filename, filename, MAX_FILENAME);

    SwarmClient old_peer = {source, CLIENT_PEER, ""};
    memcpy(old_peer.filename, filename, MAX_FILENAME);

    swarm.erase(old_peer);
    swarm.insert(new_seed);
}

void send_finish_message(int num_tasks) {
    char finish_msg[MSG_SIZE];
    memset(finish_msg, 0, MSG_SIZE);
    memcpy(finish_msg, MSG_TRACKER_FINISH, strlen(MSG_TRACKER_FINISH));
    for (int i = 1; i < num_tasks; i++) {
        MPI_Send(finish_msg, MSG_SIZE, MPI_CHAR, i, TAG_UPLOAD, MPI_COMM_WORLD);
    }
}

void tracker(int num_tasks) {
    for (int client = 1; client < num_tasks; client++) {
        receive_file_info(client);
    }

    send_ready_to_start(num_tasks);

    int peers = 0;
    while (peers < num_tasks - 1) {
        MPI_Status status;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;

        if (tag == TAG_FILE_REQUEST) {
            handle_file_request(source);
        }
        else if (tag == TAG_SWARM_UPDATE) {
            handle_swarm_update(source);
        }
        else if (tag == TAG_FINISH) {
            handle_finish(source, peers);
        }
        else if (tag == TAG_DOWNLOAD_COMPLETE) {
            handle_download_complete(source);
        }
    }

    send_finish_message(num_tasks);
}

void send_file_info(int rank) {
    int no_fields_held = achived_files.size();
    MPI_Send(&no_fields_held, 1, MPI_INT, TRACKER_RANK, TAG_INIT, MPI_COMM_WORLD);

    for (const auto& file : achived_files) {
        MPI_Send(file.first.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TAG_INIT, MPI_COMM_WORLD);
        int no_segments_held_file = file.second.size();
        MPI_Send(&no_segments_held_file, 1, MPI_INT, TRACKER_RANK, TAG_INIT, MPI_COMM_WORLD);
        for (const auto& hash : file.second) {
            MPI_Send(hash.c_str(), HASH_SIZE-1, MPI_CHAR, TRACKER_RANK, TAG_INIT, MPI_COMM_WORLD);
        }
    }
}

void peer(int rank, int num_tasks) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    read_init_file(rank);
    send_file_info(rank);

    char ready_msg[MSG_SIZE];
    MPI_Recv(ready_msg, MSG_SIZE, MPI_BYTE, TRACKER_RANK, TAG_INIT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}


int main(int argc, char *argv[]) {
    int num_tasks, rank, provided;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI doesn't support multi-threading\n");
        exit(1);
    }

    MPI_Comm_size(MPI_COMM_WORLD, &num_tasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(num_tasks);
    } else {
        peer(rank, num_tasks);
    }

    MPI_Finalize();
    return 0;
}
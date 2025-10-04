#include "src/utils/ExternalWatcher.h"
#include "src/core/Solver.h"

#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <json/json.h> // Use JsonCpp

namespace Minisat {

ExternalWatcher::ExternalWatcher(const std::string &socket_path) : 
    socket_path(socket_path),
    socket_id(socket(AF_UNIX, SOCK_STREAM, 0)),
    stopping(false)
{
    if (socket_id < 0) {
        perror("socket");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    unlink(socket_path.c_str()); // Remove any old socket file

    if (bind(socket_id, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(socket_id);
        return;
    }

    if (listen(socket_id, 5) < 0) { // 5 is the backlog queue size
        perror("listen");
        close(socket_id);
        return;
    }

    watcher_thread = std::thread(&ExternalWatcher::watcherThread, this);
    std::cout << "ExternalWatcher: listening on " << socket_path << std::endl;
}

ExternalWatcher::~ExternalWatcher() {
    stopping = true;
    int client_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    int conn = connect(client_socket, (struct sockaddr*)&addr, sizeof(addr));
    watcher_thread.join();
    std::cout << "ExternalWatcher stopped: " << socket_path << std::endl;
}

void ExternalWatcher::watcherThread() {
    for (;;) {
        int client_socket = accept(socket_id, NULL, NULL);
        if (stopping) break;
        if (client_socket < 0) {
            perror("accept");
            continue;
        }

        // Wait for a query (any data) from the client
        char buffer[10];
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            std::unique_lock<std::mutex> lock(query_mutex);
            client_sockets.push_back(client_socket);
        } else {
            perror("recv");
            close(client_socket);
        }
    }
    for (int client_socket : client_sockets)
        close(client_socket);
    client_sockets.clear();
    close(socket_id);
    unlink(socket_path.c_str());
}

/*
  Function called to potentialy send a checkpoint of the SAT solver,
  in case any request has been made.
*/
void ExternalWatcher::notifyConflict(const Clause& confl_clause, const vec<Lit>& trail, const vec<int>& trail_lim, const vec<Lit>& learnt_clause) {
    std::unique_lock<std::mutex> lock(query_mutex);
    if (client_sockets.empty()) return;
    std::string message = prepareData(confl_clause, trail, trail_lim, learnt_clause);
    for (int client_socket : client_sockets) {
        send(client_socket, message.c_str(), message.length(), 0);
        close(client_socket);
    }
    client_sockets.clear();
}

/*
  Take the current state of a SAT solver, and encode into JSON data
*/
std::string ExternalWatcher::prepareData(const Clause& confl_clause, const vec<Lit>& trail, const vec<int>& trail_lim, const vec<Lit>& learnt_clause) {
    Json::Value root;

    Json::Value json_confl_clause(Json::arrayValue);
    for (int i = 0; i < confl_clause.size(); i++) {
        json_confl_clause.append(toInt(confl_clause[i]));
    }
    root["confl_clause"] = json_confl_clause;

    Json::Value json_trail(Json::arrayValue);
    for (int i = 0; i < trail.size(); i++) {
        json_trail.append(toInt(trail[i]));
    }
    root["trail"] = json_trail;

    Json::Value json_trail_lim(Json::arrayValue);
    for (int i = 0; i < trail_lim.size(); i++) {
        json_trail_lim.append(trail_lim[i]);
    }
    root["trail_lim"] = json_trail_lim;

    Json::Value json_learnt_clause(Json::arrayValue);
    for (int i = 0; i < learnt_clause.size(); i++) {
        json_learnt_clause.append(toInt(learnt_clause[i]));
    }
    root["learned_clause"] = json_learnt_clause;

    Json::FastWriter fastWriter;
    std::string message = fastWriter.write(root);
    return message;
}

} // namespace Minisat

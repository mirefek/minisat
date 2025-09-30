#include "src/utils/ExternalWatcher.h"
#include "src/core/Solver.h"

#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <json/json.h> // Use JsonCpp

namespace Minisat {

ExternalWatcher::ExternalWatcher(Solver* solver_ptr) : 
    solver(solver_ptr),
    listen_fd(-1),
    running(false),
    pending_query(false),
    last_confl(CRef_Undef)
{
    // Initialize vectors to empty
    last_trail.clear();
    last_trail_lim.clear();
    last_learnt_clause.clear();
}

ExternalWatcher::~ExternalWatcher() {
    stop();
}

void ExternalWatcher::start(const std::string& path) {
    if (running) {
        std::cerr << "ExternalWatcher is already running." << std::endl;
        return;
    }
    socket_path = path;

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    unlink(socket_path.c_str()); // Remove any old socket file

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return;
    }

    if (listen(listen_fd, 5) < 0) { // 5 is the backlog queue size
        perror("listen");
        close(listen_fd);
        return;
    }

    running = true;
    watcher_thread = std::thread(&ExternalWatcher::watcherThread, this);
    std::cout << "ExternalWatcher listening on: " << socket_path << std::endl;
}

void ExternalWatcher::stop() {
    if (!running) {
        return;
    }
    running = false;
    if (listen_fd != -1) {
        close(listen_fd);
        listen_fd = -1;
        unlink(socket_path.c_str());
    }
    if (watcher_thread.joinable()) {
        watcher_thread.join();
    }
    std::cout << "ExternalWatcher stopped." << std::endl;
}

void ExternalWatcher::notifyConflict(CRef confl, const Minisat::vec<Minisat::Lit>& trail, const Minisat::vec<int>& trail_lim, const Minisat::vec<Minisat::Lit>& learnt_clause) {
    std::unique_lock<std::mutex> lock(query_mutex);
    if (pending_query) {
        last_confl = confl;
        // Deep copy of vectors
        trail.copyTo(last_trail);
        trail_lim.copyTo(last_trail_lim);
        learnt_clause.copyTo(last_learnt_clause);
        query_cond.notify_one();
    }
}

void ExternalWatcher::watcherThread() {
    while (running) {
        int client_socket = accept(listen_fd, NULL, NULL);
        if (client_socket < 0) {
            if (running) {
                perror("accept");
            }
            continue;
        }

        std::cout << "Watcher: Client connected." << std::endl;

        // Wait for a query (any data) from the client
        char buffer[1];
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            std::unique_lock<std::mutex> lock(query_mutex);
            pending_query = true;
            query_cond.wait(lock, [this]{ return !pending_query || !running; }); // Wait until notified or stopped

            if (!running) {
                close(client_socket);
                break;
            }
            
            std::cout << "Watcher: Sending data..." << std::endl;
            sendData(client_socket, last_confl, last_trail, last_trail_lim, last_learnt_clause);
            std::cout << "Watcher: Data sent." << std::endl;
            pending_query = false;
        } else if (bytes_read == 0) {
            std::cout << "Watcher: Client disconnected normally." << std::endl;
        } else {
            perror("recv");
        }

        close(client_socket);
    }
}

void ExternalWatcher::sendData(int client_socket, CRef confl, const Minisat::vec<Minisat::Lit>& trail, const Minisat::vec<int>& trail_lim, const Minisat::vec<Minisat::Lit>& learnt_clause) {
    Json::Value root;

    root["confl"] = (Json::Int64)confl;

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
    root["learnt_clause"] = json_learnt_clause;

    Json::FastWriter fastWriter;
    std::string message = fastWriter.write(root);
    send(client_socket, message.c_str(), message.length(), 0);
}

} // namespace Minisat

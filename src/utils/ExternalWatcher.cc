#include "src/utils/ExternalWatcher.h"
#include "src/core/Solver.h"
#include "src/core/SolverTypes.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <json/value.h>
#include <minisat/core/SolverTypes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <json/json.h>

namespace Minisat {

ExternalWatcher::ExternalWatcher(Solver &S, const std::string &socket_path) : 
    S(S),
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
        
        // Wait for a query (any data) from the client
        std::string received_data;
        char buffer[1000];
        ssize_t bytes_read;
        while ((bytes_read = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
            ssize_t bytes_used = bytes_read;
            if (buffer[bytes_read-1] == 0) bytes_used--;
            received_data.append(buffer, bytes_used);
            if (bytes_used < bytes_read) break;
        }

        // Try to decode as JSON (using JsonCpp)
        Json::CharReaderBuilder builder;
        Json::Value instr;
        std::string errs;
        std::istringstream iss(received_data);
        bool json_ok = Json::parseFromStream(builder, iss, &instr, &errs);
        if (!json_ok) {
            std::cerr << "ExternalWatcher: Failed to parse JSON from client: " << errs << std::endl;
            std::cerr << received_data.size() << std::endl;
        }

        std::unique_lock<std::mutex> lock(query_mutex);
        client_sockets_received.push_back(client_socket);
        if (json_ok) {
            instructions.push_back(std::move(instr));
        }
    }
    for (int client_socket : client_sockets_received) close(client_socket);
    for (int client_socket : client_sockets_ready) close(client_socket);
    client_sockets_received.clear();
    client_sockets_ready.clear();
    close(socket_id);
    unlink(socket_path.c_str());
}

/*
  Function called to potentialy send a checkpoint of the SAT solver,
  in case any request has been made.
*/
void ExternalWatcher::notifyConflict(CRef confl, const vec<Lit>& learnt_clause) {
    if (!client_sockets_ready.empty()) {
        std::string message = exportData(confl, learnt_clause);
        for (int client_socket : client_sockets_ready) {
            send(client_socket, message.c_str(), message.length(), 0);
            close(client_socket);
        }
        client_sockets_ready.clear();
    }
}

/*
  Run received instructions, prepare connections for sending responses.
*/
bool ExternalWatcher::notifyDecision() {
    bool should_restart = false;
    std::unique_lock<std::mutex> lock(query_mutex);
    if (!instructions.empty()) {
        for (const Json::Value &instr : instructions)
        should_restart |= processInstruction(instr);
        instructions.clear();
    }
    if (!client_sockets_received.empty()) {
        for (int client_socket : client_sockets_received)
            client_sockets_ready.push_back(client_socket);
        client_sockets_received.clear();
    }
    return should_restart;
}

/*
  Take the current state of a SAT solver, and encode into JSON data
*/
std::string ExternalWatcher::exportData(CRef confl, const vec<Lit>& learnt_clause) {
    const Clause &confl_clause = S.ca[confl];
    Json::Value root;

    Json::Value json_confl_clause(Json::arrayValue);
    for (int i = 0; i < confl_clause.size(); i++)
        json_confl_clause.append(toInt(confl_clause[i]));
    root["confl_clause"] = json_confl_clause;

    Json::Value json_trail(Json::arrayValue);
    for (int i = 0; i < S.trail.size(); i++)
        json_trail.append(toInt(S.trail[i]));
    root["trail"] = json_trail;

    Json::Value json_trail_lim(Json::arrayValue);
    for (int i = 0; i < S.trail_lim.size(); i++)
        json_trail_lim.append(S.trail_lim[i]);
    root["trail_lim"] = json_trail_lim;

    Json::Value json_learnt_clause(Json::arrayValue);
    for (int i = 0; i < learnt_clause.size(); i++)
        json_learnt_clause.append(toInt(learnt_clause[i]));
    root["learned_clause"] = json_learnt_clause;

    Json::Value json_activity(Json::arrayValue);
    for (int i = 0; i < S.nVars(); i++)
        json_activity.append(S.activity[i]);
    root["activity"] = json_activity;

    if (S.best_clause != CRef_Undef) {
        const Clause &best_clause = S.ca[S.best_clause];
        Json::Value json_best_clause(Json::arrayValue);
        for (int i = 0; i < best_clause.size(); i++)
            json_best_clause.append(toInt(best_clause[i]));
        root["best_clause"] = json_best_clause;
    }

    Json::FastWriter fastWriter;
    std::string message = fastWriter.write(root);
    return message;
}

bool ExternalWatcher::processInstruction(const Json::Value &instr) {
    bool should_restart = false;
    if (!instr.isObject()) {
        std::cerr << "Instruction is not a Json object: " << instr << std::endl;
        return false;
    }

    for (Json::ValueConstIterator it = instr.begin(); it != instr.end(); ++it) {
        const std::string &key = it.name();
        const Json::Value &value = *it;
        if (key == "polarity") {
            if (!value.isArray()) {
                std::cerr << "Instruction: Polarity is not an array" << key << std::endl;
                continue;
            }
            for (int i = 0; i < value.size() && i < S.nVars(); i++) {
                const Json::Value &val = value[i];
                if (!val.isConvertibleTo(Json::ValueType::booleanValue))
                    S.setPolarity(i, l_Undef);
                else {
                    S.setPolarity(i, lbool(!val.asBool()));
                }
            }
        }
        else if (key == "precedence") {
            if (!value.isArray()) {
                std::cerr << "Instruction: Precedence is not an array" << key << std::endl;
                continue;
            }
            for (int i = 0; i < value.size() && i < S.nVars(); i++) {
                const Json::Value &val = value[i];
                if (val.isConvertibleTo(Json::ValueType::intValue)) S.user_prec[i] = val.asInt();
                else S.user_prec[i] = 0;
            }
            S.rebuildOrderHeap();
        }
        else if (key == "restart") {
            if (value.isConvertibleTo(Json::ValueType::booleanValue))
                should_restart |= value.asBool();
            else
                std::cerr << "Instruction: Restart is not a boolean" << key << std::endl;
        }
        else if (key == "definitions") {
            if (!value.isArray()) {
                std::cerr << "Instruction: Definitions do not form an array" << key << std::endl;
                continue;
            }
            if (value.size() > 0) {
                // TODO:
                // temporarily remove values (S.assign) for almost all variables from the stack
                int start = 0;
                if (S.trail_lim.size() > 0) start = S.trail_lim[0];
                // hide values from trail
                for (int i = start; i < S.trail.size(); i++)
                    S.assigns[var(S.trail[i])] = l_Undef;

                // Main code: add definitions
                // TODO: also addClause_ calls propagate, couldn't it lead to some issues?
                for (const Json::Value &definition : value)
                    if (!addDefinition(definition)) break;

                // revert values from trail
                for (int i = start; i < S.trail.size(); i++) {
                    Lit p = S.trail[i];
                    S.assigns[var(p)] = lbool(!sign(p));
                }
                S.propagate();
                // update variable order heap
                S.rebuildOrderHeap();
            }
        }
        else std::cerr << "Instruction: Unexpected Json key: " << key << std::endl;
    }
    return should_restart;
}

/*
A definition is encoded of nested Json arrays & integers, where an array
represents a NAND gate !(x1 & x2 & ... & xn), and an integer represents a literal.
Let `v` decode the new variable, then the calculated value will be equal to
the value of the literal `mkLit(v, false) = mkLit(v)`.
*/
bool ExternalWatcher::checkDefJson(const Json::Value &def_json) {
    if (def_json.isArray()) {
        for (const Json::Value &item : def_json)
            if (!checkDefJson(item)) return false;
        return true;
    }
    else if (def_json.isConvertibleTo(Json::ValueType::intValue)) {
        Lit l = toLit(def_json.asInt());
        if (l.x < 0) return false;
        if (var(l) >= S.nVars()) return false;
        return true;
    }
    else return false;
}

lbool ExternalWatcher::calcDefPolarity(const Json::Value &def_json) {
    if (def_json.isArray()) {
        lbool res = l_False;
        for (const Json::Value &item : def_json) {
            lbool val = calcDefPolarity(item);
            if (val == l_False) return l_True;
            else if (val == l_Undef) res = l_Undef;
        }
        return res;
    }
    else {
        Lit l = toLit(def_json.asInt());
        return S.user_pol[var(l)] ^ sign(l);
    }
}

/*
Let `v` be the newly defined variable.
`getDefClauses` returns a list of clauses equivalent to mkLit(v, sign).
*/
std::vector<vec<Lit>> ExternalWatcher::getDefClauses(const Json::Value &def_json, bool sign) {
    std::vector<vec<Lit>> res;
    if (def_json.isArray()) {
        size_t n = def_json.size();
        std::vector<std::vector<vec<Lit>>> subresults(n);
        for (int i = 0; i < n; i++)
            subresults[i] = getDefClauses(def_json[i], !sign);
        if (sign) { // negated NAND is AND -> we just concatenate the results
            size_t total_clauses = 0;
            for (const std::vector<vec<Lit>> &sub : subresults) total_clauses += sub.size();
            res.resize(total_clauses);
            int clause_i = 0;
            for (std::vector<vec<Lit>> &sub : subresults)
                for (vec<Lit> &clause : sub)
                    clause.moveTo(res[clause_i++]);
        }
        else { // NAND is OR on negated values -> we have to apply distributivity
            size_t total_clauses = 1;
            for (const std::vector<vec<Lit>> &sub : subresults) total_clauses *= sub.size();
            res.resize(total_clauses);
            size_t index;
            std::vector<int> multi_index(n, 0);

            for (;;) {
                // Concatenate the clauses indexed by multi_index into res[index]
                size_t cur_size = 0;
                for (int i = 0; i < n; i++)
                    cur_size += subresults[i][multi_index[i]].size();
                vec<Lit> &cur_res = res[index];
                cur_res.growTo(cur_size);
                int lit_i = 0;
                for (int i = 0; i < n; i++) {
                    vec<Lit> &clause = subresults[i][multi_index[i]];
                    int clause_size = clause.size();
                    for (int lit_j = 0; lit_j < clause_size; lit_j++)
                        cur_res[lit_i++] = clause[lit_j];
                }
                // Increase index & multi_index by one
                index++;
                int i;
                for (i = 0; i < n; i++) {
                    if (++multi_index[i] < subresults[i].size()) break;
                    else multi_index[i] = 0;
                }
                if (i >= n) break;
            }
        }
    }
    else {
        res.emplace_back();
        res[0].push(toLit(def_json.asInt()) ^ sign);
    }
    return res;
}

bool ExternalWatcher::addDefinition(const Json::Value &def_json) {
    // printf("Add definition 1\n");
    if (S.free_vars.size() > 0) {
        std::cerr << "Adding definitions not supported with `free_vars` in use." << std::endl;
        return false;
    }
    // printf("Add definition 2\n");
    if (!checkDefJson(def_json)) {
        std::cerr << "Invalid definition: " << def_json << std::endl;
        return false;
    }
    // printf("Add definition 3\n");
    // Add the new variable
    Var v = S.newVar(calcDefPolarity(def_json));
    // printf("Add definition 4\n");
    // Add positive & negative defining clauses
    for (uint8_t sign = 0; sign < 2; sign++) {
        std::vector<vec<Lit>> clauses = getDefClauses(def_json, sign);
        for (vec <Lit> &clause : clauses) {
            clause.push(mkLit(v, !sign));
            // printf("Adding clause (%d):", clause.size());
            // for (int i = 0; i < clause.size(); i++)
            //     printf(" %d", toInt(clause[i]));
            // printf("\n");
            S.addClause_(clause);
        }
    }
    return true;
}

void ExternalWatcher::loadFileInstruction(const std::string &fname) {
    std::ifstream instr_file(fname);
    if (instr_file.is_open()) {
        Json::Value def_json;
        Json::CharReaderBuilder builder;
        std::string errs;
        if (Json::parseFromStream(builder, instr_file, &def_json, &errs)) {
            processInstruction(def_json);
        } else {
            std::cerr << "Failed to parse JSON from " << fname << ": " << errs << std::endl;
        }
        instr_file.close();
    } else {
        std::cerr << "Failed to open definition file: " << fname << std::endl;
    }
}

} // namespace Minisat

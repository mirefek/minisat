#ifndef Minisat_ExternalWatcher_h
#define Minisat_ExternalWatcher_h

#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <sys/un.h>

#include "src/mtl/Vec.h"
#include "src/core/SolverTypes.h"
#include <json/json.h> // Use JsonCpp

namespace Minisat {

// Forward declaration of Solver to avoid circular dependency
class Solver;

class ExternalWatcher {
public:
    ExternalWatcher(Solver &S, const std::string& socket_path);
    ~ExternalWatcher();

    void notifyConflict(CRef confl, const vec<Lit>& learnt_clause);
    bool notifyDecision();  // returns whether the solver should restart
    void loadFileInstruction(const std::string &fname);

private:
    void         watcherThread();
    std::string  exportData(CRef confl, const vec<Lit>& learnt_clause);

    // Definitions
    bool checkDefJson(const Json::Value &);
    lbool calcDefPolarity(const Json::Value &);
    std::vector<vec<Lit>> getDefClauses(const Json::Value &, bool sign);
    bool addDefinition(const Json::Value &);
    bool processInstruction(const Json::Value &instr);  // returns whether the solver should restart

    bool                       stopping;

    Solver                     &S;
    std::string                socket_path;
    struct sockaddr_un         addr;
    int                        socket_id;
    std::thread                watcher_thread;
    std::mutex                 query_mutex;
    // sockets waiting for their instructions to be applied
    std::vector<int>           client_sockets_received;
    // sockets having their instructions applied, waiting for a conflict
    std::vector<int>           client_sockets_ready;
    std::vector<Json::Value>   instructions;
};

} // namespace Minisat

#endif // Minisat_ExternalWatcher_h

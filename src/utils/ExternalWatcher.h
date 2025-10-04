#ifndef Minisat_ExternalWatcher_h
#define Minisat_ExternalWatcher_h

#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <sys/un.h>

#include "src/mtl/Vec.h"
#include "src/core/SolverTypes.h"

namespace Minisat {

// Forward declaration of Solver to avoid circular dependency
class Solver;

class ExternalWatcher {
public:
    ExternalWatcher(const std::string& socket_path);
    ~ExternalWatcher();

    void notifyConflict(const Clause& confl_clause, const vec<Lit>& trail, const vec<int>& trail_lim, const vec<Lit>& learnt_clause);
private:
    void                       watcherThread();
    std::string                prepareData(const Clause& confl_clause, const vec<Lit>& trail, const vec<int>& trail_lim, const vec<Lit>& learnt_clause);
    bool                       stopping;

    std::string                socket_path;
    struct sockaddr_un addr;
    int                        socket_id;
    std::thread                watcher_thread;
    std::mutex                 query_mutex;
    std::vector<int>           client_sockets;
};

} // namespace Minisat

#endif // Minisat_ExternalWatcher_h

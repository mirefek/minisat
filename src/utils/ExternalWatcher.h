#ifndef Minisat_ExternalWatcher_h
#define Minisat_ExternalWatcher_h

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "minisat/mtl/Vec.h"
#include "minisat/core/SolverTypes.h"

namespace Minisat {

// Forward declaration of Solver to avoid circular dependency
class Solver;

class ExternalWatcher {
public:
    ExternalWatcher(Solver* solver_ptr);
    ~ExternalWatcher();

    void start(const std::string& socket_path);
    void stop();
    void notifyConflict(CRef confl, const Minisat::vec<Minisat::Lit>& trail, const Minisat::vec<int>& trail_lim, const Minisat::vec<Minisat::Lit>& learnt_clause);

private:
    void                       watcherThread();
    void                       sendData(int client_socket, CRef confl, const Minisat::vec<Minisat::Lit>& trail, const Minisat::vec<int>& trail_lim, const Minisat::vec<Minisat::Lit>& learnt_clause);

    Solver*                    solver;
    std::string                socket_path;
    int                        listen_fd;
    bool                       running;
    std::thread                watcher_thread;
    std::mutex                 query_mutex;
    std::condition_variable    query_cond;
    bool                       pending_query;

    CRef                        last_confl;
    Minisat::vec<Minisat::Lit>  last_trail;
    Minisat::vec<int>           last_trail_lim;
    Minisat::vec<Minisat::Lit>  last_learnt_clause;
};

} // namespace Minisat

#endif // Minisat_ExternalWatcher_h

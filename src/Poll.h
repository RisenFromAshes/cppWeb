#ifndef __CW_POLL_H_
#define __CW_POLL_H_

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>
#include "Socket.h"

namespace cW {

class Server;

class Poll {

    int                 fd;
    std::atomic<size_t> nSockets = 0;
    bool                onePoll;

    static const unsigned int bufferSize;

    // static std::mutex mtx;

    void loop();

    const Server* server;

  public:
    Poll(const Server* server, bool onePoll = true);

    void add(Socket* socket);
    void update(Socket* socket, uint32_t events) const;
    void remove(Socket* socket);

    void runLoop(int nThreads = 1);
};
}; // namespace cW

#endif
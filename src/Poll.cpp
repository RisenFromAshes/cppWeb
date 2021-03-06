#include "Poll.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include "ClientSocket.h"
#include "ListenSocket.h"
#include "Server.h"

namespace cW {

const unsigned int Poll::bufferSize = 512 * 1024;

Poll::Poll(const Server* server, bool onePoll) : server(server), onePoll(onePoll)
{
    fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd == -1) {
        perror("Failed to create epoll");
        std::terminate();
    }
}

void Poll::add(Socket* socket)
{
    epoll_ctl(fd, EPOLL_CTL_ADD, socket->fd, (epoll_event*)(socket->event));
    nSockets++;
}

void Poll::update(Socket* socket, uint32_t events) const
{
    ((epoll_event*)socket->event)->events = events | onePoll * EPOLLONESHOT;
    epoll_ctl(fd, EPOLL_CTL_MOD, socket->fd, (epoll_event*)(socket->event));
}

void Poll::remove(Socket* socket)
{
    epoll_ctl(fd, EPOLL_CTL_DEL, socket->fd, nullptr);
    nSockets--;
}

void Poll::loop()
{
    static int  n = 1;
    epoll_event events[1024];
    char        buffer[bufferSize];
    while (nSockets > 0) {
        int nEvents = epoll_wait(fd, events, 1024, -1);
        if (nEvents < 0)
            perror("Epoll wait error");
        else {
            for (int i = 0; i < nEvents; i++) {
                switch (((Socket*)events[i].data.ptr)->type) {
                    case Socket::Type::LISTEN: {
                        // printf("Listen event\n");
                        Clock::start();
                        ListenSocket* socket =
                            static_cast<ListenSocket*>((Socket*)events[i].data.ptr);
                        if (!socket->connected)
                            goto disconnect_listen;
                        else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                            perror("Listen socket error occured");
                            goto disconnect_listen;
                        }
                        else {
                            if (events[i].events & EPOLLIN) {
                                ClientSocket* acceptSocket;
                                while (acceptSocket = ClientSocket::from(socket, server, onePoll)) {
                                    add(acceptSocket);
                                }
                            }
                            update(socket, EPOLLIN);
                            break;
                        }
                    disconnect_listen:
                        remove(socket);
                        close(socket->fd);
                        delete socket;
                        break;
                    }
                    case Socket::Type::ACCEPT: {
                        ClientSocket* socket =
                            static_cast<ClientSocket*>((Socket*)events[i].data.ptr);
                        socket->loopPreCb();
                        if (!socket->connected) goto disconnect;
                        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                            socket->connected = false;
                            socket->onAborted();
                            goto disconnect;
                        }
                        else {
                            if (events[i].events & EPOLLIN) {
                                int bytesReceived = recv(socket->fd, buffer, bufferSize, 0);
                                if (bytesReceived < 0) {
                                    if (errno != EAGAIN) {
                                        perror("Receive error");
                                        goto disconnect;
                                    }
                                }
                                else if (bytesReceived == 0) {
                                    goto disconnect;
                                }
                                else
                                    socket->onData(std::string_view(buffer, bytesReceived));
                            }
                            if (events[i].events & EPOLLOUT) socket->onWritable();
                        }
                        socket->loopPostCb();
                        if (socket->connected) {
                            update(socket,
                                   EPOLLIN * socket->wantRead | EPOLLOUT * socket->wantWrite);
                            break;
                        }
                    disconnect:
                        printf("Closing socket %d\n", n++);
                        remove(socket);
                        shutdown(socket->fd, SHUT_WR);
                        close(socket->fd);
                        delete socket;
                        break;
                    }
                }
            }
        }
    }
}
void Poll::runLoop(int nThreads)
{
    if (nThreads < 0) nThreads = std::thread::hardware_concurrency();
    if (nThreads == 1)
        loop();
    else {
        std::vector<std::unique_ptr<std::thread>> threads;
        for (unsigned int i = 0; i < nThreads; i++)
            threads.push_back(std::make_unique<std::thread>([this] { loop(); }));
        for (unsigned int i = 0; i < nThreads; i++)
            if (threads[i]->joinable()) threads[i]->join();
    }
}
}; // namespace cW
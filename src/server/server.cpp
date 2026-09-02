#include "server/server.h"

#include <atomic>
#include <cerrno>
#include <iostream>
#include <thread>

#include "core/logger.h"
#include "database/database.h"
#include "server/http.h"
#include "server/viewer.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace offcat {
namespace {

void close_conn(int sock);

void handle_client(int sock, Viewer& viewer) {
    // Receive timeout: a client that connects and then stalls must not
    // pin a detached thread forever.  recv() then fails and read_request
    // returns false, closing the socket.
#ifdef _WIN32
    DWORD recv_timeout = 10000;
#else
    timeval recv_timeout{10, 0};
#endif
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout),
               sizeof(recv_timeout));

    HttpRequest req;
    if (!read_request(sock, req)) {
        close_conn(sock);
        return;
    }
    // Loopback-only Host check: blocks DNS rebinding (an attacker page
    // would send its own domain as Host).  Also rejects requests with no
    // Host header, which HTTP/1.1 requires.
    if (!host_allowed(req.host)) {
        send_response(sock, 400, "text/plain",
                      "400 Bad Request: invalid Host header");
        close_conn(sock);
        return;
    }

    std::string body;
    try {
        body = viewer.handle(req);
    } catch (const std::exception& e) {
        // Last line of defense: any exception escaping a detached thread
        // would call std::terminate and kill the whole server.
        LOG_WARN("Request failed: " + std::string(e.what()));
        send_response(sock, 500, "text/plain", "500 Internal Server Error");
        close_conn(sock);
        return;
    } catch (...) {
        LOG_WARN("Request failed");
        send_response(sock, 500, "text/plain", "500 Internal Server Error");
        close_conn(sock);
        return;
    }

    if (body.empty()) {
        send_404(sock, req.path);
    } else {
        std::string ct = viewer.is_api(req.path)
            ? "application/json"
            : viewer.content_type_for(req.path);
        send_response(sock, 200, ct, body);
    }
    close_conn(sock);
}

void close_conn(int sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

}  // namespace

int run_server(const std::string& db_path, int port,
               const std::string& web_root) {
    // db and viewer live on the heap: detached handler threads capture
    // the viewer by shared_ptr, so an accept() failure that exits the
    // accept loop cannot leave running threads with dangling references.
    auto db = std::make_shared<Database>();
    auto open_result = db->open_readonly(db_path);
    if (is_err(open_result)) {
        LOG_ERROR(get_err(open_result).message);
        return 1;
    }

    auto viewer = std::make_shared<Viewer>(*db, web_root);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup failed");
        return 1;
    }
#endif

    int listener = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (listener < 0) {
        LOG_ERROR("socket() failed");
        return 1;
    }

    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("bind() failed: port " + std::to_string(port) + " in use?");
#ifdef _WIN32
        closesocket(listener);
        WSACleanup();
#else
        close(listener);
#endif
        return 1;
    }
    listen(listener, 16);

    std::cout << "Serving catalog (read-only): " << db_path << "\n"
              << "  http://127.0.0.1:" << port << "\n"
              << (web_root.empty()
                      ? "  frontend: embedded\n"
                      : "  frontend: " + web_root + "\n")
              << "  Press Ctrl+C to stop.\n";

    for (;;) {
        sockaddr_in client{};
#ifdef _WIN32
        int len = sizeof(client);
#else
        socklen_t len = sizeof(client);
#endif
        int c = static_cast<int>(
            accept(listener, reinterpret_cast<sockaddr*>(&client), &len));
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // Capture db alongside viewer: Viewer holds a Database&, so the
        // database must outlive every detached handler thread.
        std::thread([c, db, viewer]() { handle_client(c, *viewer); }).detach();
    }

#ifdef _WIN32
    closesocket(listener);
    WSACleanup();
#else
    close(listener);
#endif
    return 0;
}

}  // namespace offcat

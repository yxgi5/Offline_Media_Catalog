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

void handle_client(int sock, Viewer& viewer) {
    HttpRequest req;
    if (!read_request(sock, req)) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return;
    }
    std::string body = viewer.handle(req);
    if (body.empty()) {
        send_404(sock, req.path);
    } else {
        std::string ct = viewer.is_api(req.path)
            ? "application/json"
            : viewer.content_type_for(req.path);
        send_response(sock, ct, body);
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

}  // namespace

int run_server(const std::string& db_path, int port,
               const std::string& web_root) {
    Database db;
    auto open_result = db.open_readonly(db_path);
    if (is_err(open_result)) {
        LOG_ERROR(get_err(open_result).message);
        return 1;
    }

    Viewer viewer(db, web_root);

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
        std::thread([c, &viewer]() { handle_client(c, viewer); }).detach();
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

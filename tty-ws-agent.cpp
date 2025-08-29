// agent.cpp

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <iostream>
#include <thread>
#include <array>
#include <pty.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string>
#include <cstring>
#include <mutex>

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;

using tcp        = asio::ip::tcp;
using ssl_stream = asio::ssl::stream<tcp::socket>;
using ws_stream  = websocket::stream<ssl_stream>;

// Replay buffer globals
std::mutex replayMutex;
std::string replayBuffer;
const size_t MAX_REPLAY = 200000; // 200 KB cap

// --------------------------------------------------
// very small "parser" for resize messages
bool parse_resize_message(const std::string &msg, int &cols, int &rows) {
    if (msg.find("\"type\"") == std::string::npos ||
        msg.find("resize") == std::string::npos) return false;
    const char *cstr = msg.c_str();
    int c, r;
    if (sscanf(cstr, "{\"type\":\"resize\",\"cols\":%d,\"rows\":%d}", &c, &r) == 2) {
        cols = c; rows = r; return true;
    }
    if (sscanf(cstr, "{\"type\":\"resize\",\"rows\":%d,\"cols\":%d}", &r, &c) == 2) {
        cols = c; rows = r; return true;
    }
    return false;
}

bool is_dump_message(const std::string &msg) {
    return msg.find("\"type\"") != std::string::npos &&
           msg.find("dump")     != std::string::npos;
}
// --------------------------------------------------

// Forward data from PTY -> websocket
void pump_pty_to_ws(asio::posix::stream_descriptor &pty, ws_stream &ws) {
    try {
        std::array<char, 4096> buf;
        for (;;) {
            std::size_t n = pty.read_some(asio::buffer(buf));
            if (n == 0) break;

            // append to replay buffer
            {
                std::lock_guard<std::mutex> lk(replayMutex);
                replayBuffer.append(buf.data(), n);
                if (replayBuffer.size() > MAX_REPLAY) {
                    replayBuffer.erase(0, replayBuffer.size() - MAX_REPLAY);
                }
            }

            // forward live to websocket
            ws.write(asio::buffer(buf.data(), n));
        }
    } catch (std::exception &) {
        try { ws.close(websocket::close_code::normal); } catch (...) {}
    }
}

// Forward data from websocket -> PTY
void pump_ws_to_pty(ws_stream &ws, asio::posix::stream_descriptor &pty, pid_t childPid) {
    try {
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);  // read one complete WS frame

            auto data = buffer.data();
            std::string msg(boost::asio::buffer_cast<const char*>(data), data.size());

            int cols, rows;
            if (parse_resize_message(msg, cols, rows)) {
                struct winsize wsz;
                wsz.ws_col = cols;
                wsz.ws_row = rows;
                wsz.ws_xpixel = 0;
                wsz.ws_ypixel = 0;
                ioctl(pty.native_handle(), TIOCSWINSZ, &wsz);
                kill(childPid, SIGWINCH);
                continue; // don't forward resize JSON to shell
            }

            if (is_dump_message(msg)) {
                std::lock_guard<std::mutex> lk(replayMutex);
                if (!replayBuffer.empty()) {
                    ws.write(asio::buffer(replayBuffer));
                }
                continue; // don't forward to shell
            }

            // Otherwise normal stdin -> PTY
            if (data.size() > 0) {
                asio::write(pty, data);
            }
        }
    } catch (std::exception &) {
        try { pty.close(); } catch (...) {}
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <server-host> <server-port> <remote-path> [--no-verify]\n"
                  << "Example: ./agent existingsite.com 443 /agent --no-verify\n";
        return 1;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *path = argv[3];
    bool insecure = (argc >= 5 && std::string(argv[4]) == "--no-verify");

    try {
        asio::io_context ioc;
        asio::ssl::context ctx(asio::ssl::context::tls_client);

        ctx.set_default_verify_paths();
        if (insecure) {
            std::cout << "WARNING: TLS certificate verification is disabled!" << std::endl;
            ctx.set_verify_mode(asio::ssl::verify_none);
        } else {
            ctx.set_verify_mode(asio::ssl::verify_peer);
        }

        // TCP + TLS connect
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host, port);
        ssl_stream sslSock(ioc, ctx);
        if(!SSL_set_tlsext_host_name(sslSock.native_handle(), host)) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()};
            throw beast::system_error{ec};
        }
        asio::connect(sslSock.next_layer(), results);
        sslSock.handshake(asio::ssl::stream_base::client);

        // Upgrade to WebSocket
        ws_stream ws(std::move(sslSock));
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws.binary(true);
        ws.handshake(host, path);
        std::cout << "Connected to wss://" << host << path << std::endl;

        // Fork PTY with child bash
        int master_fd;
        pid_t pid = forkpty(&master_fd, nullptr, nullptr, nullptr);
        if (pid == -1) {
            perror("forkpty");
            return 1;
        }
        if (pid == 0) {
            // set TERM
            setenv("TERM", "xterm-256color", 1);
            execl("/bin/bash", "bash", (char*)nullptr);
            execl("/bin/sh", "sh", (char*)nullptr);
            _exit(127);
        }

        asio::posix::stream_descriptor pty(ioc, master_fd);
        std::cout << "Spawned shell (pid " << pid << ") with pty" << std::endl;

        std::thread t1([&] { pump_pty_to_ws(pty, ws); });
        std::thread t2([&] { pump_ws_to_pty(ws, pty, pid); });

        t1.join();
        t2.join();

        int status;
        waitpid(pid, &status, 0);

    } catch (std::exception &e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 2;
    }
}
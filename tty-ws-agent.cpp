// tty-ws-agent.cpp

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <iostream>
#include <thread>
#include <array>
#include <unistd.h>
#include <pty.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <mutex>
#include <fcntl.h>
#include <chrono>

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;

using tcp        = asio::ip::tcp;
using ssl_stream = asio::ssl::stream<tcp::socket>;
using ws_stream  = websocket::stream<ssl_stream>;

// Replay buffer globals
std::mutex replayMutex;
std::string replayBuffer;
constexpr size_t MAX_REPLAY = 200000;

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

// Async pumps ---------------------------------------------------
void start_pty_to_ws(std::shared_ptr<asio::posix::stream_descriptor> pty,
                     std::shared_ptr<ws_stream> ws,
                     std::shared_ptr<std::array<char,4096>> buf,
                     pid_t childPid,
                     asio::io_context& ioc)
{
    pty->async_read_some(asio::buffer(*buf),
        [pty, ws, buf, childPid, &ioc](beast::error_code ec, std::size_t n) {
            if (!ec && n > 0) {
                {
                    std::lock_guard<std::mutex> lk(replayMutex);
                    replayBuffer.append(buf->data(), n);
                    if (replayBuffer.size() > MAX_REPLAY)
                        replayBuffer.erase(0, replayBuffer.size()-MAX_REPLAY);
                }
                ws->async_write(asio::buffer(buf->data(), n),
                    [pty, ws, buf, childPid, &ioc](beast::error_code ec2, std::size_t) {
                        if (!ec2) start_pty_to_ws(pty, ws, buf, childPid, ioc);
                        else {
                            std::cerr << "[pty->ws] write error: " << ec2.message() << "\n";
                            beast::error_code ignore;
                            pty->close(ignore);
                            ws->next_layer().next_layer().close(ignore);
                            if (childPid > 0) kill(childPid, SIGTERM);
                            ioc.stop();
                        }
                    });
            } else {
                std::cerr << "[pty->ws] pipe closed (ec=" << ec.message() << ")\n";
                beast::error_code ignore;
                pty->close(ignore);
                ws->next_layer().next_layer().close(ignore);
                if (childPid>0) kill(childPid,SIGTERM);
                ioc.stop();
            }
        });
}

void start_ws_to_pty(std::shared_ptr<ws_stream> ws,
                     std::shared_ptr<asio::posix::stream_descriptor> pty,
                     pid_t childPid,
                     asio::io_context& ioc)
{
    auto buffer = std::make_shared<beast::flat_buffer>();
    ws->async_read(*buffer,
        [ws, pty, buffer, childPid, &ioc](beast::error_code ec, std::size_t) {
            if (!ec) {
                auto data = buffer->data();
                std::string msg(boost::asio::buffer_cast<const char*>(data), data.size());

                int cols, rows;
                if (parse_resize_message(msg, cols, rows)) {
                    struct winsize wsz;
                    wsz.ws_row    = static_cast<unsigned short>(rows);
                    wsz.ws_col    = static_cast<unsigned short>(cols);
                    wsz.ws_xpixel = 0;
                    wsz.ws_ypixel = 0;
                    ioctl(pty->native_handle(), TIOCSWINSZ, &wsz);
                    kill(childPid, SIGWINCH);
                    start_ws_to_pty(ws, pty, childPid, ioc);
                    return;
                }
                if (is_dump_message(msg)) {
                    std::lock_guard<std::mutex> lk(replayMutex);
                    if (!replayBuffer.empty()) {
                        // ANSI clear + restore
                        static const char clearSeq[] = "\033[2J\033[H";
                        std::string payload(clearSeq);
                        payload += replayBuffer;

                        ws->async_write(asio::buffer(payload),
                            [ws, pty, childPid, &ioc](beast::error_code, std::size_t) {
                                start_ws_to_pty(ws, pty, childPid, ioc);
                            });
                        return;
                    }
                    start_ws_to_pty(ws, pty, childPid, ioc);
                    return;
                }

                asio::async_write(*pty, data,
                    [ws, pty, childPid, &ioc](beast::error_code ec2, std::size_t) {
                        if (!ec2) start_ws_to_pty(ws, pty, childPid, ioc);
                        else {
                            std::cerr << "[ws->pty write error] " << ec2.message() << "\n";
                            beast::error_code ignore;
                            pty->close(ignore);
                            ws->next_layer().next_layer().close(ignore);
                            if (childPid>0) kill(childPid,SIGTERM);
                            ioc.stop();
                        }
                    });
            } else {
                if (ec == websocket::error::closed) {
                    std::cerr << "[ws] Remote closed connection\n";
                } else {
                    std::cerr << "[ws] Error: " << ec.message() << "\n";
                }
                beast::error_code ignore;
                pty->close(ignore);
                ws->next_layer().next_layer().close(ignore);
                if (childPid>0) kill(childPid,SIGTERM);
                ioc.stop();
            }
        });
}

// Keepalive ---------------------------------------------------
void start_ping(std::shared_ptr<ws_stream> ws,
                asio::steady_timer& timer,
                asio::io_context& ioc,
                std::shared_ptr<std::chrono::steady_clock::time_point> last_pong)
{
    timer.expires_after(std::chrono::seconds(10)); // interval
    timer.async_wait([ws, &timer, &ioc, last_pong](beast::error_code ec){
        if (!ec) {
            ws->async_ping({}, [ws, &timer, &ioc, last_pong](beast::error_code ec2){
                if (ec2) {
                    std::cerr << "[ping] write error: " << ec2.message() << "\n";
                    beast::error_code ignore;
                    ws->next_layer().next_layer().close(ignore);
                    ioc.stop();
                    return;
                }
                auto now = std::chrono::steady_clock::now();
                if (now - *last_pong > std::chrono::seconds(20)) { // timeout
                    std::cerr << "[ping] no pong received within 20s, closing.\n";
                    beast::error_code ignore;
                    ws->next_layer().next_layer().close(ignore);
                    ioc.stop();
                    return;
                }
                start_ping(ws, timer, ioc, last_pong);
            });
        }
    });
}

// main ---------------------------------------------------------
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

        auto ws = std::make_shared<ws_stream>(std::move(sslSock));
        ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws->binary(true);
        ws->handshake(host, path);
        std::cout << "Connected to wss://" << host << path << std::endl;

        // FD_CLOEXEC -------------------------------------------------
        {
            int fd = ws->next_layer().next_layer().native_handle();
            int flags = fcntl(fd, F_GETFD);
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }

        // Last pong tracker
        auto last_pong = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        ws->control_callback([last_pong](websocket::frame_type kind, boost::beast::string_view){
            if (kind == websocket::frame_type::pong) {
                *last_pong = std::chrono::steady_clock::now();
                std::cerr << "[ws] Got pong\n";
            }
        });

        // Fork PTY with child bash ----------------------------------
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
        auto pty = std::make_shared<asio::posix::stream_descriptor>(ioc, master_fd);
        std::cout << "Spawned shell (pid " << pid << ") with PTY\n";

        auto buf = std::make_shared<std::array<char,4096>>();
        start_pty_to_ws(pty, ws, buf, pid, ioc);
        start_ws_to_pty(ws, pty, pid, ioc);

        // Start Ping loop -------------------------------------------
        asio::steady_timer ping_timer(ioc);
        start_ping(ws, ping_timer, ioc, last_pong);

        // SIGCHLD handler -------------------------------------------
        asio::signal_set sigs(ioc, SIGCHLD);
        sigs.async_wait([&](beast::error_code, int signo){
            if (signo == SIGCHLD) {
                int status;
                pid_t dead;
                while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
                    if (dead == pid) {
                        std::cerr << "[main] child " << pid << " exited\n";
                        beast::error_code ignore;
                        pty->close(ignore);
                        ws->next_layer().next_layer().close(ignore);
                        ioc.stop();
                    }
                }
            }
        });

        // Run --------------------------------------------------------
        ioc.run();
        return 0;
    } catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
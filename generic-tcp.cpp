#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <iostream>
#include <thread>
#include <array>

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;

using tcp         = asio::ip::tcp;
using ssl_stream  = asio::ssl::stream<tcp::socket>;
using ws_stream   = websocket::stream<ssl_stream>;
using unix_domain = asio::local::stream_protocol;

// Forward local UNIX → WebSocket
void pump_unix_to_ws(unix_domain::socket &usock, ws_stream &ws) {
    try {
        std::array<char, 4096> buf;
        for (;;) {
            std::size_t n = usock.read_some(asio::buffer(buf));
            if (n == 0) break;
            ws.write(asio::buffer(buf.data(), n));
        }
    } catch (std::exception &e) {
        try { ws.close(websocket::close_code::normal); } catch (...) {}
    }
}

// Forward WebSocket → local UNIX
void pump_ws_to_unix(ws_stream &ws, unix_domain::socket &usock) {
    try {
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);                           // one complete WS frame
            auto data = buffer.data();
            if (data.size() > 0) {
                asio::write(usock, data);
            }
        }
    } catch (std::exception &e) {
        try { usock.close(); } catch (...) {}
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <server-host> <server-port> <remote-path> [unix-sock-path]\n"
                  << "Example: ./generic-tcp existingsite.com 443 /agent /tmp/generic-tcp.sock\n";
        return 1;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *path = argv[3];
    std::string unix_path = (argc > 4) ? argv[4] : "/tmp/generic-tcp.sock";

    try {
        asio::io_context ioc;
        asio::ssl::context ctx(asio::ssl::context::tls_client);

        // simplified: skip CA verification for self-signed server
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(asio::ssl::verify_none);

        // --- Connect TCP + TLS ---
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host, port);
        ssl_stream sslSock(ioc, ctx);

        asio::connect(sslSock.next_layer(), results);
        sslSock.handshake(asio::ssl::stream_base::client);

        // --- WebSocket upgrade ---
        ws_stream ws(std::move(sslSock));
        ws.set_option(
            websocket::stream_base::timeout::suggested(beast::role_type::client)
        );
        ws.binary(true); // ensure we send/recv binary frames, not text
        ws.handshake(host, path);
        std::cout << "Connected to wss://" << host << path << std::endl;

        // --- Prepare local Unix socket ---
        ::unlink(unix_path.c_str());
        unix_domain::acceptor acceptor(ioc, unix_domain::endpoint(unix_path));
        std::cout << "Listening on local unix socket: " << unix_path << std::endl;

        unix_domain::socket usock(ioc);
        acceptor.accept(usock);
        std::cout << "Local client connected on " << unix_path << std::endl;

        // --- Pump both directions in threads ---
        std::thread t1([&] { pump_unix_to_ws(usock, ws); });
        std::thread t2([&] { pump_ws_to_unix(ws, usock); });

        t1.join();
        t2.join();

    } catch (std::exception &e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 2;
    }
}
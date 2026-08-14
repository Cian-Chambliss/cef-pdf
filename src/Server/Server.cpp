#include "Server.h"

#include "include/wrapper/cef_helpers.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

#include <iostream>
#include <utility>
#include <functional>
#include <chrono>

namespace cefpdf {
namespace server {

Server::Server(
    CefRefPtr<cefpdf::Client> client,
    const std::string& address,
    const std::string& port
) :
    m_client(client),
    m_thread(),
    m_ioService(),
    m_signals(m_ioService),
    m_acceptor(m_ioService),
    m_socket(m_ioService),
    m_sessionManager(new SessionManager),
    m_counter(0),
    m_idleTimer(m_ioService)
{
    m_signals.add(SIGINT);
    m_signals.add(SIGTERM);
#if defined(SIGQUIT)
    m_signals.add(SIGQUIT);
#endif // defined(SIGQUIT)

    // Open the acceptor with the option to reuse the address (i.e. SO_REUSEADDR).
    asio::ip::tcp::resolver resolver(m_ioService);
    asio::ip::tcp::endpoint endpoint = *resolver.resolve({address, port});
    m_acceptor.open(endpoint.protocol());
    m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    m_acceptor.bind(endpoint);
}

void Server::Start()
{
    m_thread = std::thread(std::bind(&Server::Run, this));

    m_client->SetAllowedSchemes({"http", "https", "ftp", "data", cefpdf::constants::scheme});
    m_client->Run();

    m_thread.join();
}

void Server::Run()
{
    m_acceptor.listen();

    m_signals.async_wait(std::bind(
        &Server::OnSignal,
        this,
        std::placeholders::_1,
        std::placeholders::_2
    ));

    auto endpoint = m_acceptor.local_endpoint();

    // Welcome message
    std::cout << "Starting HTTP server on "
              << endpoint.address().to_string()
              << ":" << endpoint.port()
              << std::endl;

    Listen();
    // start/reset idle timer
    ResetIdleTimer();

    m_ioService.run();

    CefPostTask(TID_UI, base::BindOnce(&cefpdf::Client::Stop, m_client));

    DLOG(INFO) << "HTTP server thread finished";
}

void Server::Listen()
{
    DLOG(INFO) << "Server::Listen";

    m_acceptor.async_accept(m_socket, std::bind(
        &Server::OnConnection,
        this,
        std::placeholders::_1
    ));
}

void Server::OnSignal(std::error_code error, int signno)
{
    DLOG(INFO) << "HTTP server received shutdown signal";

    m_acceptor.close();
    m_sessionManager->CloseAll();
    // Also stop io_service so Run() can exit promptly
    m_ioService.stop();
}

void Server::OnConnection(std::error_code error)
{
    // Check whether the server was stopped by a signal before this
    // completion handler had a chance to run.
    if (!m_acceptor.is_open()) {
        return;
    }

    if (!error) {
        auto clientIp = m_socket.remote_endpoint().address().to_string();
        m_sessionManager->Start(
            new Session(m_client, m_sessionManager, std::move(m_socket))
        );
        // Update activity and reset idle timer
        m_sessionManager->UpdateActivity();
        ResetIdleTimer();

        ++m_counter;
        DLOG(INFO) << "Got HTTP hit no. " << m_counter
                   << " from " << clientIp;
        Listen();
    }
}

void Server::ResetIdleTimer()
{
    asio::error_code ec;
    m_idleTimer.cancel(ec); // ignore cancel errors
    m_idleTimer.expires_from_now(idleTimeout);
    m_idleTimer.async_wait(std::bind(&Server::OnIdleTimeout, this, std::placeholders::_1));
}

void Server::OnIdleTimeout(const std::error_code& ec)
{
    if (ec == asio::error::operation_aborted) {
        // Timer was reset or cancelled; nothing to do
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto last = m_sessionManager->LastActivity();

    if (m_sessionManager->IsEmpty() &&
        std::chrono::duration_cast<std::chrono::seconds>(now - last) >= idleTimeout) {
        std::cout << "Idle timeout reached, shutting down HTTP server" << std::endl;
        m_acceptor.close();
        m_sessionManager->CloseAll();
        m_ioService.stop();
    } else {
        // Not yet idle — restart timer
        ResetIdleTimer();
    }
}

} // namespace server
} // namespace cefpdf

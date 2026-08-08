#include "Session.h"
#include "SessionManager.h"

#include "../Job/Local.h"
#include "../Job/Remote.h"
#include "../Common.h"

#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

#include <iostream>
#include <string>
#include <sstream>
#include <ostream>
#include <functional>
#include <regex>
#include <algorithm>
#include <cctype>

namespace cefpdf {
namespace server {

namespace {
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseInteger(const std::string& value, int minimum, int maximum, int& result)
{
    try {
        std::size_t consumed = 0;
        long parsed = std::stol(value, &consumed);
        if (consumed != value.size() || parsed < minimum || parsed > maximum) return false;
        result = static_cast<int>(parsed);
        return true;
    } catch (...) { return false; }
}

bool ParseBackground(const std::string& input, job::Job::ImageBackground& color)
{
    const std::string value = Lower(input);
    if (value == "white") { color = {255, 255, 255, 255}; return true; }
    if (value == "black") { color = {0, 0, 0, 255}; return true; }
    if (value.size() != 7 || value[0] != '#') return false;
    try {
        std::size_t consumed = 0;
        unsigned long rgb = std::stoul(value.substr(1), &consumed, 16);
        if (consumed != 6) return false;
        color = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
            static_cast<uint8_t>(rgb), 255};
        return true;
    } catch (...) { return false; }
}
}

Session::Session(
    CefRefPtr<cefpdf::Client> client,
    CefRefPtr<SessionManager> sessionManager,
    asio::ip::tcp::socket socket
) :
    m_client(client),
    m_sessionManager(sessionManager),
    m_socket(std::move(socket))
{}

void Session::ReadHeaders()
{
    asio::async_read_until(
        m_socket,
        m_buffer,
        http::crlf + http::crlf,
        std::bind(
            &Session::OnReadHeaders,
            this,
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

void Session::ReadChunk()
{
    asio::async_read_until(
        m_socket,
        m_buffer,
        http::crlf,
        std::bind(
            &Session::OnReadChunk,
            this,
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

void Session::Read(std::size_t bytes)
{
    asio::async_read(
        m_socket,
        m_buffer,
        asio::transfer_exactly(bytes),
        std::bind(
            &Session::OnRead,
            this,
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

void Session::ReadAll()
{
    asio::async_read(
        m_socket,
        m_buffer,
        asio::transfer_all(),
        std::bind(
            &Session::OnRead,
            this,
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

void Session::Write()
{
    std::error_code error;
    asio::streambuf buffer;
    std::ostream os(&buffer);

    m_response.WriteToStream(os);

    asio::write(m_socket, buffer, error);

    if (!error) {
        // Initiate graceful connection closure.
        asio::error_code ignored_ec;
        m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored_ec);
    }

    if (error != asio::error::operation_aborted) {
        m_sessionManager->Stop(this);
    }
}

void Session::Write(const std::string& status)
{
    m_response.SetStatus(status);
    Write();
}

void Session::Write100Continue()
{
    std::error_code error;
    asio::streambuf buffer;
    std::ostream os(&buffer);

    os << http::protocol << " " << http::statuses::cont << http::crlf << http::crlf;

    asio::write(m_socket, buffer, error);
}

void Session::OnReadHeaders(const std::error_code& ec, std::size_t bytes_transferred)
{
    ParseRequestHeaders();

    if (m_request.method == "GET") {
        Handle();
    } else if (m_request.method == "POST") {
        if (!(m_request.encoding.empty() || m_request.chunked)) {
            // No transfer encoding supported yet
            Write(http::statuses::unsupported);
            return;
        }

        if (stringsEqual(m_request.expect, "100-continue")) {
            Write100Continue();
        }

        if (m_request.chunked) {
            ReadChunk();
            return;
        }

        if (m_request.length > 0) {
            std::size_t bytesToRead = m_request.length - m_request.content.size();
            if (bytesToRead > 0) {
                Read(bytesToRead);
            } else {
                Handle();
            }
        } else if (!m_request.location.empty()) {
            Handle();
        } else {
            Write(http::statuses::lengthRequired);
        }
    } else {
        Write(http::statuses::badMethod);
    }
}

void Session::OnReadChunk(const std::error_code& ec, std::size_t bytes_transferred)
{
    if (ParseChunks(FetchBuffer())) {
        Handle();
    }
}

void Session::OnRead(const std::error_code& ec, std::size_t bytes_transferred)
{
    auto content = FetchBuffer();

    if (m_request.chunked) {
        if (content.size() > http::crlf.size()) {
            m_request.content += content.substr(0, content.size() - http::crlf.size());
        }

        ReadChunk();
        return;
    } else {
        m_request.content += content;
    }

    Handle();
}

std::string Session::FetchBuffer()
{
    std::ostringstream ss;
    ss << &m_buffer;

    return ss.str();
}

bool Session::ParseChunks(const std::string& content)
{
    std::size_t chunkSize, pos = 0;
    auto separatorSize = http::crlf.size();
    auto idx = content.find(http::crlf);

    while (idx != std::string::npos) {
        chunkSize = std::stoi(m_chunkStart + content.substr(pos, idx - pos), nullptr, 16);
        m_chunkStart.clear();

        if (0 == chunkSize) {
            // Zero chunk, this is the end
            return true;
        }

        m_request.content += content.substr(idx + separatorSize, chunkSize);
        pos = idx + chunkSize + 2 * separatorSize;

        if (content.size() == pos) {
            break;
        } else if (content.size() < pos) {
            // Read rest of the chunk data
            Read(pos - content.size());
            return false;
        }

        idx = content.find(http::crlf, pos);

        if (idx == std::string::npos) {
            m_chunkStart = content.substr(pos);
        }
    }

    // Read next chunk
    ReadChunk();
    return false;
}

void Session::ParseRequestHeaders()
{
    auto requestData = FetchBuffer();

    // Separate headers section from request content
    std::string headerData;
    auto sepIndex = requestData.find(http::crlf + http::crlf);

    if (sepIndex == std::string::npos) {
        headerData = requestData;
    } else {
        headerData = requestData.substr(0, sepIndex + 2);
        m_request.content = requestData.substr(sepIndex + 4);
    }

    // Parse status line
    std::regex re("^([A-Z]+) (\\S+) HTTP/(.+)" + http::crlf);
    std::smatch match;

    if (std::regex_search(headerData, match, re)) {
        m_request.method = match[1];
        m_request.url = match[2];
        m_request.version = match[3];
    }

    // Collect request headers
    std::regex re2("(.+): (.+)" + http::crlf);
    std::string::const_iterator it, end;
    it = match[0].second;
    end = headerData.end();
    m_request.chunked = false;
    m_request.length = 0;

    while (std::regex_search(it, end, match, re2)) {
        m_request.headers.push_back({match[1], match[2]});

        if (stringsEqual(match[1], http::headers::encoding)) {
            if (!stringsEqual(match[2], "identity")) {
                m_request.encoding = match[2];
                if (stringsEqual(m_request.encoding, "chunked")) {
                    m_request.chunked = true;
                }
            }
        } else if (stringsEqual(match[1], http::headers::length)) {
            m_request.length = std::stoi(match[2]);
        } else if (stringsEqual(match[1], http::headers::expect)) {
            m_request.expect = match[2];
        } else if (stringsEqual(match[1], http::headers::location)) {
            m_request.location = match[2];
        } else if (stringsEqual(match[1], http::headers::contentType)) {
            m_request.contentType = match[2];
        } else if (stringsEqual(match[1], http::headers::pageSize)) {
            m_request.pageSize = match[2];
        } else if (stringsEqual(match[1], http::headers::pageMargin)) {
            m_request.pageMargin = match[2];
        } else if (stringsEqual(match[1], http::headers::pdfOptions)) {
            m_request.pdfOptions = match[2];
        } else if (stringsEqual(match[1], http::headers::headerTitle)) {
            m_request.headerTitle = match[2];
        } else if (stringsEqual(match[1], http::headers::footerURL)) {
            m_request.footerURL = match[2];
        } else if (stringsEqual(match[1], http::headers::imageCapture)) {
            m_request.imageCapture = match[2];
        } else if (stringsEqual(match[1], http::headers::imageViewport)) {
            m_request.imageViewport = match[2];
        } else if (stringsEqual(match[1], http::headers::imageQuality)) {
            m_request.imageQuality = match[2];
        } else if (stringsEqual(match[1], http::headers::imageBackground)) {
            m_request.imageBackground = match[2];
        }
        it = match[0].second;
    }
}

void Session::Handle()
{
    m_response.SetStatus(http::statuses::ok);
    m_response.SetHeader(http::headers::date, formatDate());

    if (m_request.url == "/" || m_request.url == "/about") {
        m_response.SetContent(GetAboutReply(), "application/json");
        Write();
    } else if (m_request.url == "/list-sizes") {
        m_response.SetContent(GetPageSizesReply(), "application/json");
        Write();
    } else {
        // Parse url path
        std::regex re("^/(?:.*/)?([^/?]+\\.(pdf|png|jpg|jpeg|bmp))(?:\\?.*)?$", std::regex_constants::icase);
        std::smatch match;

        if (std::regex_search(m_request.url, match, re)) {
            if (m_request.method == "GET" && m_request.location.empty()) {
                Write(http::statuses::badMethod);
            } else {
                const std::string extension = Lower(match[2]);
                job::Job::OutputFormat format = job::Job::OutputFormat::PDF;
                if (extension == "png") format = job::Job::OutputFormat::PNG;
                else if (extension == "jpg" || extension == "jpeg") format = job::Job::OutputFormat::JPEG;
                else if (extension == "bmp") format = job::Job::OutputFormat::BMP;
                HandleOutput(match[1], format);
            }
        } else {
            Write(http::statuses::notFound);
        }
    }
}

std::string Session::GetAboutReply()
{
    std::string content;

    content += "{";
    content += "\"status\": \"ok\", ";
    content += "\"version\": \"" + cefpdf::constants::version + "\", ";
    content += "\"headers\": [";
    content += "\"" + http::headers::location + "\", ";
    content += "\"" + http::headers::pageSize + "\", ";
    content += "\"" + http::headers::pageMargin + "\", ";
    content += "\"" + http::headers::pdfOptions + "(landscape|backgrounds|headerfooter)\"";
    content += ", \"" + http::headers::imageCapture + "\"";
    content += ", \"" + http::headers::imageViewport + "\"";
    content += ", \"" + http::headers::imageQuality + "\"";
    content += ", \"" + http::headers::imageBackground + "\"";
    content += "]}";

    return content;
}

std::string Session::GetPageSizesReply()
{
    std::string content;

    content += "[";
    cefpdf::PageSizesMap::const_iterator it;

    for (it = cefpdf::pageSizesMap.begin(); it != cefpdf::pageSizesMap.end(); ++it) {
        if (content.size() > 1) {
            content += ", ";
        }

        content += "\"" + it->name + "\"";
    }

    content += "]";

    return content;
}

void Session::HandleOutput(const std::string& fileName, job::Job::OutputFormat format)
{
    std::string safeName = fileName;
    safeName.erase(std::remove_if(safeName.begin(), safeName.end(),
        [](char c) { return c == '\r' || c == '\n' || c == '\"'; }), safeName.end());
    m_response.SetHeader(http::headers::disposition, "inline; filename=\"" + safeName + "\"");

    CefRefPtr<cefpdf::job::Job> job;

    if (m_request.location.empty()) {
        std::string mediaType = Lower(m_request.contentType);
        const std::size_t separator = mediaType.find(';');
        if (separator != std::string::npos) mediaType.erase(separator);
        while (!mediaType.empty() && std::isspace(static_cast<unsigned char>(mediaType.back()))) mediaType.pop_back();
        if (mediaType.empty()) mediaType = "text/html";
        if (mediaType != "text/html" && mediaType != "image/svg+xml") {
            Write(http::statuses::unsupported);
            return;
        }
        job = new cefpdf::job::Local(m_request.content);
        job->SetInputMediaType(mediaType);
    } else {
        job = new cefpdf::job::Remote(m_request.location);
    }
    job->SetOutputFormat(format);

    const bool imageOutput = format != cefpdf::job::Job::OutputFormat::PDF;
    if (imageOutput && (!m_request.pageSize.empty() || !m_request.pageMargin.empty() ||
        !m_request.pdfOptions.empty() || !m_request.headerTitle.empty() || !m_request.footerURL.empty())) {
        Write(http::statuses::badRequest);
        return;
    }
    if (!imageOutput && (!m_request.imageCapture.empty() || !m_request.imageViewport.empty() ||
        !m_request.imageQuality.empty() || !m_request.imageBackground.empty())) {
        Write(http::statuses::badRequest);
        return;
    }

    if (imageOutput) {
        job->SetViewWidth(1280);
        job->SetViewHeight(720);
        const std::string capture = Lower(m_request.imageCapture);
        if (!capture.empty() && capture != "full" && capture != "viewport") {
            Write(http::statuses::badRequest); return;
        }
        if (capture == "viewport") job->SetCaptureMode(cefpdf::job::Job::CaptureMode::VIEWPORT);

        if (!m_request.imageViewport.empty()) {
            std::smatch dimensions;
            std::regex viewport("^([0-9]+)[xX]([0-9]+)$");
            if (!std::regex_match(m_request.imageViewport, dimensions, viewport)) {
                Write(http::statuses::badRequest); return;
            }
            int width = 0, height = 0;
            if (!ParseInteger(dimensions[1], 1, 32767, width) ||
                !ParseInteger(dimensions[2], 1, 32767, height)) {
                Write(http::statuses::badRequest); return;
            }
            job->SetViewWidth(width); job->SetViewHeight(height);
        }
        if (static_cast<long long>(job->GetViewWidth()) * job->GetViewHeight() > 100000000LL) {
            Write(http::statuses::badRequest); return;
        }
        if (!m_request.imageQuality.empty()) {
            int quality = 0;
            if (format != cefpdf::job::Job::OutputFormat::JPEG ||
                !ParseInteger(m_request.imageQuality, 0, 100, quality)) {
                Write(http::statuses::badRequest); return;
            }
            job->SetImageQuality(quality);
        }
        if (!m_request.imageBackground.empty()) {
            cefpdf::job::Job::ImageBackground background;
            if (format == cefpdf::job::Job::OutputFormat::PNG ||
                !ParseBackground(m_request.imageBackground, background)) {
                Write(http::statuses::badRequest); return;
            }
            job->SetImageBackground(background);
        }
    }

    if (!m_request.pageSize.empty()) {
        try {
            job->SetPageSize(m_request.pageSize);
        } catch (...) {}
    }

    if (!m_request.pageMargin.empty()) {
        try {
            job->SetPageMargin(m_request.pageMargin);
        } catch (...) {}
    }

    if (!m_request.pdfOptions.empty()) {
        if (std::string::npos != m_request.pdfOptions.find("landscape")) {
            job->SetLandscape(true);
        }
        if (std::string::npos != m_request.pdfOptions.find("backgrounds")) {
            job->SetBackgrounds(true);
        }
        if (std::string::npos != m_request.pdfOptions.find("headerfooter")) {
            job->SetHeaderFooterEnabled(true);
        }        
    }
    if (!m_request.headerTitle.empty()) job->SetHeaderFooterTitle(m_request.headerTitle);
    if (!m_request.footerURL.empty()) job->SetHeaderFooterUrl(m_request.footerURL);

    job->SetCallback(std::bind(
        &Session::OnResolve,
        this,
        std::placeholders::_1
    ));

    CefPostTask(TID_UI, base::BindOnce(&cefpdf::Client::AddJob, m_client, job));
}

void Session::OnResolve(CefRefPtr<cefpdf::job::Job> job)
{
    CefRefPtr<Session> self(this);
    m_socket.get_io_service().post([self, job]() {
        self->CompleteResponse(job);
    });
}

void Session::CompleteResponse(CefRefPtr<cefpdf::job::Job> job)
{
    if (!m_socket.is_open()) {
        return;
    }

    if (job->GetStatus() == cefpdf::job::Job::Status::SUCCESS) {
        m_response.SetContent(loadTempFile(job->GetOutputPath()), cefpdf::job::GetOutputMimeType(job->GetOutputFormat()));
    } else {
        m_response.SetStatus(http::statuses::error);
    }

    Write();
}

} // namespace server
} // namespace cefpdf

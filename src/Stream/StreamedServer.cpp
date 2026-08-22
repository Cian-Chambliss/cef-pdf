#include "StreamedServer.h"

#include "../Client.h"
#include "../Common.h"
#include "../Job/Local.h"
#include "../Job/Remote.h"

#include "include/cef_parser.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

#if defined(OS_WIN)
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cefpdf {
namespace stream {
namespace {

const std::size_t kMaxHeaderBytes = 8192;
const std::size_t kMaxPacketBytes = 64 * 1024 * 1024;
const std::size_t kMaxActiveRequests = 100;
const std::size_t kMaxQueuedResponses = 1024;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string Trim(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

CefRefPtr<CefValue> CopyId(CefRefPtr<CefValue> id)
{
    if (id) return id->Copy();
    CefRefPtr<CefValue> value = CefValue::Create();
    value->SetNull();
    return value;
}

CefRefPtr<CefValue> Response(CefRefPtr<CefValue> id, const std::string& status)
{
    CefRefPtr<CefDictionaryValue> dictionary = CefDictionaryValue::Create();
    dictionary->SetValue("id", CopyId(id));
    dictionary->SetString("status", status);
    CefRefPtr<CefValue> value = CefValue::Create();
    value->SetDictionary(dictionary);
    return value;
}

CefRefPtr<CefValue> ErrorResponse(CefRefPtr<CefValue> id,
    const std::string& code, const std::string& message)
{
    CefRefPtr<CefValue> value = Response(id, "error");
    CefRefPtr<CefDictionaryValue> error = CefDictionaryValue::Create();
    error->SetString("code", code);
    error->SetString("message", message);
    value->GetDictionary()->SetDictionary("error", error);
    return value;
}

bool IsId(CefRefPtr<CefValue> value)
{
    if (!value) return false;
    return value->GetType() == VTYPE_STRING || value->GetType() == VTYPE_INT ||
        (value->GetType() == VTYPE_DOUBLE && std::isfinite(value->GetDouble()));
}

std::string IdKey(CefRefPtr<CefValue> value)
{
    if (value->GetType() == VTYPE_STRING)
        return std::string("string:") + value->GetString().ToString();
    return std::string("number:") + CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}

std::string PathKey(const std::string& value)
{
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(std::filesystem::u8path(value), error);
    if (error) path = std::filesystem::u8path(value);
    std::string key = path.lexically_normal().string();
#if defined(OS_WIN)
    key = Lower(key);
#endif
    return key;
}

std::vector<std::string> RequestPaths(CefRefPtr<job::Job> job)
{
    std::vector<std::string> paths;
    paths.push_back(PathKey(job->GetOutputPath().ToString()));
    if (!job->GetSaveHtmlPath().empty()) {
        const std::string saveHtml = PathKey(job->GetSaveHtmlPath());
        if (saveHtml == paths.front())
            throw std::string("output.path and options.saveHtml must be different");
        paths.push_back(saveHtml);
    }
    return paths;
}

std::string RequiredString(CefRefPtr<CefDictionaryValue> dictionary,
    const char* key, const char* description)
{
    if (!dictionary || dictionary->GetType(key) != VTYPE_STRING)
        throw std::string(description) + " must be a string";
    const std::string value = dictionary->GetString(key).ToString();
    if (value.empty()) throw std::string(description) + " is empty";
    return value;
}

bool OptionalBool(CefRefPtr<CefDictionaryValue> dictionary, const char* key, bool& value)
{
    if (!dictionary || !dictionary->HasKey(key)) return false;
    if (dictionary->GetType(key) != VTYPE_BOOL)
        throw std::string("options.") + key + " must be a boolean";
    value = dictionary->GetBool(key);
    return true;
}

bool OptionalString(CefRefPtr<CefDictionaryValue> dictionary, const char* key, std::string& value)
{
    if (!dictionary || !dictionary->HasKey(key)) return false;
    if (dictionary->GetType(key) != VTYPE_STRING)
        throw std::string("options.") + key + " must be a string";
    value = dictionary->GetString(key).ToString();
    return true;
}

bool OptionalInteger(CefRefPtr<CefDictionaryValue> dictionary, const char* key,
    int minimum, int maximum, int& result)
{
    if (!dictionary || !dictionary->HasKey(key)) return false;
    CefRefPtr<CefValue> value = dictionary->GetValue(key);
    double number = 0;
    if (value->GetType() == VTYPE_INT) number = value->GetInt();
    else if (value->GetType() == VTYPE_DOUBLE) number = value->GetDouble();
    else throw std::string("options.") + key + " must be an integer";
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < minimum || number > maximum)
        throw std::string("options.") + key + " is out of range";
    result = static_cast<int>(number);
    return true;
}

job::Job::OutputFormat ParseFormat(const std::string& input)
{
    const std::string value = Lower(input);
    if (value == "pdf") return job::Job::OutputFormat::PDF;
    if (value == "png") return job::Job::OutputFormat::PNG;
    if (value == "jpg" || value == "jpeg") return job::Job::OutputFormat::JPEG;
    if (value == "bmp") return job::Job::OutputFormat::BMP;
    throw std::string("unsupported output format: ") + input;
}

const char* FormatName(job::Job::OutputFormat format)
{
    return format == job::Job::OutputFormat::JPEG ? "jpeg" : job::GetOutputExtension(format);
}

job::Job::ImageBackground ParseBackground(const std::string& input)
{
    const std::string value = Lower(input);
    if (value == "white") return {255, 255, 255, 255};
    if (value == "black") return {0, 0, 0, 255};
    if (value == "red") return {255, 0, 0, 255};
    if (value == "green") return {0, 128, 0, 255};
    if (value == "blue") return {0, 0, 255, 255};
    if (value.size() == 7 && value[0] == '#') {
        char* end = nullptr;
        const unsigned long rgb = std::strtoul(value.c_str() + 1, &end, 16);
        if (end == value.c_str() + 7)
            return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
                static_cast<uint8_t>(rgb), 255};
    }
    throw std::string("invalid image background color");
}

void ValidateKeys(CefRefPtr<CefDictionaryValue> dictionary,
    const std::set<std::string>& allowed, const std::string& location)
{
    if (!dictionary) return;
    CefDictionaryValue::KeyList keys;
    dictionary->GetKeys(keys);
    for (const auto& key : keys) {
        const std::string name = key.ToString();
        if (allowed.find(name) == allowed.end())
            throw std::string("unknown ") + location + " field: " + name;
    }
}

std::string InputValue(CefRefPtr<CefDictionaryValue> input,
    const char* primary, const char* fallback)
{
    if (input->GetType(primary) == VTYPE_STRING)
        return input->GetString(primary).ToString();
    if (fallback && input->GetType(fallback) == VTYPE_STRING)
        return input->GetString(fallback).ToString();
    throw std::string("input.") + primary + " must be a string";
}

CefRefPtr<job::Job> BuildJob(CefRefPtr<CefDictionaryValue> request)
{
    ValidateKeys(request, {"id", "command", "input", "output", "options"}, "request");
    if (request->GetType("input") != VTYPE_DICTIONARY)
        throw std::string("input must be an object");
    if (request->GetType("output") != VTYPE_DICTIONARY)
        throw std::string("output must be an object");
    if (request->HasKey("options") && request->GetType("options") != VTYPE_DICTIONARY)
        throw std::string("options must be an object");

    CefRefPtr<CefDictionaryValue> input = request->GetDictionary("input");
    CefRefPtr<CefDictionaryValue> output = request->GetDictionary("output");
    CefRefPtr<CefDictionaryValue> options = request->GetDictionary("options");
    ValidateKeys(input, {"type", "content", "url", "path"}, "input");
    ValidateKeys(output, {"path", "format"}, "output");
    ValidateKeys(options, {"size", "margin", "landscape", "backgrounds", "scale",
        "delay", "waitSignal", "waitSignalTimeout", "saveHtml", "staticOnly",
        "viewWidth", "viewHeight", "headerFooter", "headerTitle", "footerUrl",
        "capture", "quality", "imageBackground"}, "options");

    const std::string type = Lower(RequiredString(input, "type", "input.type"));
    CefRefPtr<job::Job> job;
    if (type == "html" || type == "svg") {
        job = new job::Local(InputValue(input, "content", nullptr));
        job->SetInputMediaType(type == "svg" ? "image/svg+xml" : "text/html");
    } else if (type == "url") {
        const std::string url = InputValue(input, "url", "content");
        if (url.empty()) throw std::string("input.url is empty");
        job = new job::Remote(url);
    } else if (type == "file") {
        const std::string path = InputValue(input, "path", "content");
        if (path.empty() || !fileExists(path)) throw std::string("input file does not exist");
        job = new job::Remote(pathToUri(path));
    } else {
        throw std::string("input.type must be html, svg, url, or file");
    }

    const std::string outputPath = RequiredString(output, "path", "output.path");
    const std::string formatName = RequiredString(output, "format", "output.format");
    const job::Job::OutputFormat format = ParseFormat(formatName);
    const std::size_t dot = outputPath.find_last_of('.');
    if (dot != std::string::npos) {
        const std::string extension = Lower(outputPath.substr(dot + 1));
        if (extension == "pdf" || extension == "png" || extension == "jpg" ||
            extension == "jpeg" || extension == "bmp") {
            if (ParseFormat(extension) != format)
                throw std::string("output.format conflicts with output.path extension");
        }
    }
    job->SetOutputPath(outputPath);
    job->SetOutputFormat(format);

    const bool image = format != job::Job::OutputFormat::PDF;
    if (image) {
        job->SetViewWidth(1280);
        job->SetViewHeight(720);
    }

    std::string text;
    bool flag = false;
    int number = 0;
    const bool hasPdfOption = options && (options->HasKey("size") || options->HasKey("margin") ||
        options->HasKey("landscape") || options->HasKey("backgrounds") || options->HasKey("scale") ||
        options->HasKey("headerFooter") || options->HasKey("headerTitle") || options->HasKey("footerUrl"));
    const bool hasImageOption = options && (options->HasKey("capture") || options->HasKey("quality") ||
        options->HasKey("imageBackground"));
    if (image && hasPdfOption) throw std::string("PDF options require PDF output");
    if (!image && hasImageOption) throw std::string("image options require PNG, JPEG, or BMP output");

    if (OptionalString(options, "size", text)) {
        if (text.empty()) throw std::string("options.size is empty");
        job->SetPageSize(text);
    }
    if (OptionalString(options, "margin", text)) {
        if (text.empty()) throw std::string("options.margin is empty");
        job->SetPageMargin(text);
    }
    if (OptionalBool(options, "landscape", flag)) job->SetLandscape(flag);
    if (OptionalBool(options, "backgrounds", flag)) job->SetBackgrounds(flag);
    if (OptionalInteger(options, "scale", 1, 200, number)) job->SetScale(number);
    if (OptionalInteger(options, "delay", 0, INT_MAX, number)) job->SetDelay(number);
    if (OptionalBool(options, "waitSignal", flag)) job->SetWaitForSignal(flag);
    if (OptionalInteger(options, "waitSignalTimeout", 0, INT_MAX, number)) job->SetWaitSignalTimeout(number);
    if (OptionalString(options, "saveHtml", text)) {
        if (text.empty()) throw std::string("options.saveHtml is empty");
        job->SetSaveHtmlPath(text);
    }
    if (OptionalBool(options, "staticOnly", flag)) {
        if (flag && (!options || !options->HasKey("saveHtml")))
            throw std::string("options.staticOnly requires options.saveHtml");
        job->SetSaveHtmlStaticOnly(flag);
    }
    if (OptionalInteger(options, "viewWidth", 1, 32767, number)) job->SetViewWidth(number);
    if (OptionalInteger(options, "viewHeight", 1, 32767, number)) job->SetViewHeight(number);
    if (OptionalBool(options, "headerFooter", flag)) job->SetHeaderFooterEnabled(flag);
    if (OptionalString(options, "headerTitle", text)) job->SetHeaderFooterTitle(text);
    if (OptionalString(options, "footerUrl", text)) job->SetHeaderFooterUrl(text);
    if (OptionalString(options, "capture", text)) {
        text = Lower(text);
        if (text == "full") job->SetCaptureMode(job::Job::CaptureMode::FULL);
        else if (text == "viewport") job->SetCaptureMode(job::Job::CaptureMode::VIEWPORT);
        else throw std::string("options.capture must be full or viewport");
    }
    if (OptionalInteger(options, "quality", 0, 100, number)) {
        if (format != job::Job::OutputFormat::JPEG)
            throw std::string("options.quality is only valid for JPEG output");
        job->SetImageQuality(number);
    }
    if (OptionalString(options, "imageBackground", text)) {
        if (format == job::Job::OutputFormat::PNG)
            throw std::string("options.imageBackground is only valid for JPEG or BMP output");
        job->SetImageBackground(ParseBackground(text));
    }
    if (image && static_cast<long long>(job->GetViewWidth()) * job->GetViewHeight() > 100000000LL)
        throw std::string("image viewport exceeds the 100 million pixel limit");
    return job;
}

bool WriteAll(ProtocolHandle output, const char* data, std::size_t size)
{
    while (size > 0) {
#if defined(OS_WIN)
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
        if (!WriteFile(static_cast<HANDLE>(output), data, chunk, &written, nullptr) || written == 0)
            return false;
#else
        const ssize_t written = write(output, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
#endif
        data += written;
        size -= written;
    }
    return true;
}

} // namespace

bool PrepareProtocolStdout(ProtocolHandle& handle, std::string& error)
{
    std::fflush(stdout);
#if defined(OS_WIN)
    HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE duplicate = nullptr;
    if (!original || original == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate,
            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = "could not duplicate stdout";
        return false;
    }
    HANDLE standardError = GetStdHandle(STD_ERROR_HANDLE);
    if (!standardError || standardError == INVALID_HANDLE_VALUE ||
        !SetStdHandle(STD_OUTPUT_HANDLE, standardError) ||
        _dup2(_fileno(stderr), _fileno(stdout)) != 0) {
        CloseHandle(duplicate);
        error = "could not redirect stdout to stderr";
        return false;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    handle = duplicate;
#else
    const int duplicate = dup(STDOUT_FILENO);
    if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) < 0 ||
        dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        if (duplicate >= 0) close(duplicate);
        error = "could not reserve and redirect stdout";
        return false;
    }
    handle = duplicate;
#endif
    return true;
}

void CloseProtocolHandle(ProtocolHandle handle)
{
#if defined(OS_WIN)
    if (handle) CloseHandle(static_cast<HANDLE>(handle));
#else
    if (handle >= 0) close(handle);
#endif
}

StreamedServer::StreamedServer(CefRefPtr<Client> client, ProtocolHandle output) :
    m_client(client), m_output(output), m_pending(0), m_accepting(true),
    m_finishing(false), m_finalQueued(false), m_exitCode(0)
{}

StreamedServer::~StreamedServer()
{
    Join();
}

void StreamedServer::Start()
{
    m_writer = std::thread(&StreamedServer::WriteLoop, this);
    m_reader = std::thread(&StreamedServer::ReadLoop, this);
}

void StreamedServer::Join()
{
    if (m_reader.joinable()) m_reader.join();
    if (m_writer.joinable()) m_writer.join();
}

int StreamedServer::GetExitCode() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_exitCode;
}

void StreamedServer::ReadLoop()
{
    for (;;) {
        std::string header;
        int matched = 0;
        bool sawByte = false;
        while (matched < 4) {
            const int c = std::fgetc(stdin);
            if (c == EOF) {
                if (!sawByte && std::feof(stdin)) {
                    FinishInput(nullptr);
                    return;
                }
                FramingError(sawByte ? "unexpected EOF in frame header" : "failed to read frame header");
                return;
            }
            sawByte = true;
            header.push_back(static_cast<char>(c));
            if (header.size() > kMaxHeaderBytes) {
                FramingError("frame header is too large");
                return;
            }
            const char delimiter[] = "\r\n\r\n";
            matched = (c == delimiter[matched]) ? matched + 1 : (c == '\r' ? 1 : 0);
        }

        std::size_t contentLength = 0;
        bool foundLength = false;
        std::size_t offset = 0;
        while (offset + 2 <= header.size() - 2) {
            const std::size_t end = header.find("\r\n", offset);
            if (end == std::string::npos || end == offset) break;
            const std::string line = header.substr(offset, end - offset);
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                FramingError("malformed frame header");
                return;
            }
            const std::string name = Lower(Trim(line.substr(0, colon)));
            const std::string value = Trim(line.substr(colon + 1));
            if (name == "content-length") {
                if (foundLength || value.empty() ||
                    value.find_first_not_of("0123456789") != std::string::npos) {
                    FramingError("invalid Content-Length header");
                    return;
                }
                errno = 0;
                char* parseEnd = nullptr;
                const unsigned long long parsed = std::strtoull(value.c_str(), &parseEnd, 10);
                if (errno == ERANGE || parseEnd != value.c_str() + value.size() || parsed > kMaxPacketBytes) {
                    FramingError("Content-Length is out of range");
                    return;
                }
                contentLength = static_cast<std::size_t>(parsed);
                foundLength = true;
            } else if (name != "content-type") {
                FramingError("unsupported frame header: " + name);
                return;
            }
            offset = end + 2;
        }
        if (!foundLength) {
            FramingError("missing Content-Length header");
            return;
        }

        std::string packet(contentLength, '\0');
        std::size_t read = 0;
        while (read < contentLength) {
            const std::size_t count = std::fread(&packet[read], 1, contentLength - read, stdin);
            if (count == 0) {
                FramingError("unexpected EOF in frame body");
                return;
            }
            read += count;
        }
        HandlePacket(packet);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting) return;
    }
}

void StreamedServer::HandlePacket(const std::string& packet)
{
    CefRefPtr<CefValue> parsed = CefParseJSON(packet.data(), packet.size(), JSON_PARSER_RFC);
    CefRefPtr<CefDictionaryValue> request = parsed ? parsed->GetDictionary() : nullptr;
    CefRefPtr<CefValue> id = request ? request->GetValue("id") : nullptr;
    if (!request) {
        Enqueue(ErrorResponse(nullptr, "invalid_request", "request body must be a JSON object"));
        return;
    }
    if (!IsId(id)) {
        Enqueue(ErrorResponse(nullptr, "invalid_request", "id must be a string or number"));
        return;
    }
    if (request->GetType("command") != VTYPE_STRING) {
        Enqueue(ErrorResponse(id, "invalid_request", "command must be a string"));
        return;
    }
    const std::string command = request->GetString("command").ToString();
    if (command == "quit") {
        try {
            ValidateKeys(request, {"id", "command"}, "quit request");
        } catch (const std::string& error) {
            Enqueue(ErrorResponse(id, "invalid_request", error));
            return;
        }
        FinishInput(id->Copy());
        return;
    }
    if (command != "render") {
        Enqueue(ErrorResponse(id, "invalid_request", "unsupported command: " + command));
        return;
    }

    try {
        CefRefPtr<job::Job> job = BuildJob(request);
        CefRefPtr<CefValue> savedId = id->Copy();
        const std::string idKey = IdKey(savedId);
        const std::vector<std::string> paths = RequestPaths(job);
        job->SetCallback(std::bind(&StreamedServer::OnJobComplete, this,
            std::placeholders::_1, savedId, idKey, paths));
        std::string rejectionCode;
        std::string rejectionMessage;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_accepting) return;
            if (m_pending >= kMaxActiveRequests) {
                rejectionCode = "server_busy";
                rejectionMessage = "active request limit reached";
            } else if (m_activeIds.find(idKey) != m_activeIds.end()) {
                rejectionCode = "duplicate_id";
                rejectionMessage = "id is already active";
            } else {
                for (const auto& path : paths) {
                    if (m_activePaths.find(path) != m_activePaths.end()) {
                        rejectionCode = "path_conflict";
                        rejectionMessage = "output or saveHtml path is already active";
                        break;
                    }
                }
            }
            if (rejectionCode.empty()) {
                m_activeIds.insert(idKey);
                m_activePaths.insert(paths.begin(), paths.end());
                ++m_pending;
            }
        }
        if (!rejectionCode.empty()) {
            Enqueue(ErrorResponse(id, rejectionCode, rejectionMessage));
            return;
        }
        if (!CefPostTask(TID_UI, base::BindOnce(&Client::AddJob, m_client, job))) {
            Enqueue(ErrorResponse(id, "internal_error", "render could not be submitted"));
            ReleaseRequest(idKey, paths);
        }
    } catch (const std::string& error) {
        Enqueue(ErrorResponse(id, "invalid_request", error));
    } catch (...) {
        Enqueue(ErrorResponse(id, "invalid_request", "invalid render request"));
    }
}

void StreamedServer::OnJobComplete(CefRefPtr<job::Job> job, CefRefPtr<CefValue> id,
    const std::string& idKey, const std::vector<std::string>& paths)
{
    CefRefPtr<CefValue> response;
    if (job->GetStatus() == job::Job::Status::SUCCESS) {
        response = Response(id, "success");
        CefRefPtr<CefDictionaryValue> output = CefDictionaryValue::Create();
        output->SetString("path", job->GetOutputPath());
        output->SetString("format", FormatName(job->GetOutputFormat()));
        output->SetString("mediaType", job::GetOutputMimeType(job->GetOutputFormat()));
        response->GetDictionary()->SetDictionary("output", output);
    } else {
        std::string code = "load_error";
        std::string message = "document failed to load";
        switch (job->GetStatus()) {
            case job::Job::Status::HTTP_ERROR: code = "http_error"; message = "document returned an HTTP error"; break;
            case job::Job::Status::OUTPUT_ERROR: code = "output_error"; message = "output could not be written"; break;
            case job::Job::Status::ABORTED: code = "aborted"; message = "render was aborted"; break;
            default: break;
        }
        response = ErrorResponse(id, code, message);
    }
    Enqueue(response);
    ReleaseRequest(idKey, paths);
}

void StreamedServer::ReleaseRequest(const std::string& idKey,
    const std::vector<std::string>& paths)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeIds.erase(idKey);
        for (const auto& path : paths) m_activePaths.erase(path);
        if (m_pending > 0) --m_pending;
    }
    FinishIfDrained();
}

void StreamedServer::FinishInput(CefRefPtr<CefValue> quitId)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finishing) return;
        m_accepting = false;
        m_finishing = true;
        m_quitId = quitId;
    }
    FinishIfDrained();
}

void StreamedServer::FinishIfDrained()
{
    CefRefPtr<CefValue> finalResponse;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_finishing || m_pending != 0 || m_finalQueued) return;
        m_finalQueued = true;
        if (m_quitId) finalResponse = Response(m_quitId, "success");
    }
    if (finalResponse) Enqueue(finalResponse, true);
    else EnqueueRaw(std::string(), true);
}

void StreamedServer::FramingError(const std::string& message)
{
    std::fprintf(stderr, "ERROR: streamed protocol framing: %s\n", message.c_str());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_exitCode = 1;
        m_accepting = false;
        m_finishing = true;
        if (m_finalQueued) return;
        m_finalQueued = true;
    }
    EnqueueRaw(std::string(), true);
}

void StreamedServer::Enqueue(CefRefPtr<CefValue> value, bool stopAfterWrite)
{
    EnqueueRaw(CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString(), stopAfterWrite);
}

void StreamedServer::EnqueueRaw(const std::string& json, bool stopAfterWrite)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_writeSpace.wait(lock, [this]() { return m_writes.size() < kMaxQueuedResponses; });
        m_writes.push({json, stopAfterWrite});
    }
    m_writeReady.notify_one();
}

void StreamedServer::WriteLoop()
{
    for (;;) {
        WriteItem item;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_writeReady.wait(lock, [this]() { return !m_writes.empty(); });
            item = m_writes.front();
            m_writes.pop();
        }
        m_writeSpace.notify_one();
        if (!item.json.empty()) {
            std::ostringstream header;
            header << "Content-Length: " << item.json.size()
                   << "\r\nContent-Type: application/json\r\n\r\n";
            const std::string framing = header.str();
            if (!WriteAll(m_output, framing.data(), framing.size()) ||
                !WriteAll(m_output, item.json.data(), item.json.size())) {
                std::_Exit(1);
            }
        }
        if (item.stopAfterWrite) {
            if (!CefPostTask(TID_UI, base::BindOnce(&Client::RequestIdleStop, m_client)))
                std::_Exit(1);
            return;
        }
    }
}

} // namespace stream
} // namespace cefpdf

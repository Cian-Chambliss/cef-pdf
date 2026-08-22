#include "Client.h"
#include "Server/Server.h"
#include "Job/Remote.h"
#include "Job/StdInput.h"
#include "Stream/StreamedServer.h"

#include <string>
#include <list>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <climits>

#if defined(OS_WIN)
#include <io.h>
#include <fcntl.h>
#include "WindowsCrashDumps.h"
#endif // OS_WIN

void printSizes()
{
    cefpdf::PageSizesMap::const_iterator it;

    for (it = cefpdf::pageSizesMap.begin(); it != cefpdf::pageSizesMap.end(); ++it) {
        std::cout << it->name <<  " " << it->width << "x" << it->height << std::endl;
    }
}

void printHelp(std::string name)
{
    std::cout << name << " v" << cefpdf::constants::version << std::endl;
    std::cout << "  Creates PDF files and browser screenshots from HTML or SVG" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "  cef-pdf [options] --url=<url>|--file=<path> [output]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --help -h             This help screen." << std::endl;
    std::cout << "  --url=<url>           URL to load, may be http, file, data, anything supported by Chromium." << std::endl;
    std::cout << "  --file=<path>         File path to load using file:// scheme. May be relative to current directory." << std::endl;
    std::cout << "  --stdin               Read content from standard input until EOF (Unix: Ctrl+D, Windows: Ctrl+Z)." << std::endl;
    std::cout << "  --streamed            Read framed JSON render requests from stdin and write responses to stdout." << std::endl;
    std::cout << "  --format=<type>       Output format: pdf, png, jpg, jpeg, or bmp." << std::endl;
    std::cout << "  --capture=<mode>      Image capture mode: full (default) or viewport." << std::endl;
    std::cout << "  --quality=<0-100>     JPEG quality. Default is 90." << std::endl;
    std::cout << "  --image-background=<color>  JPEG/BMP background (#RRGGBB or a named color)." << std::endl;
    std::cout << "  --size=<spec>         Size (format) of the paper: A3, B2.. or custom <width>x<height> in mm." << std::endl;
    std::cout << "                        " << cefpdf::constants::pageSize << " is the default." << std::endl;
    std::cout << "  --list-sizes          Show all defined page sizes." << std::endl;
    std::cout << "  --landscape           Wheather to print with a landscape page orientation." << std::endl;
    std::cout << "                        Default is portrait." << std::endl;
    std::cout << "  --margin=<spec>       Paper margins in mm (much like CSS margin but without units)" << std::endl;
    std::cout << "                        If omitted some default margin is applied." << std::endl;
    std::cout << "  --javascript          Enable JavaScript." << std::endl;
    std::cout << "  --backgrounds         Print with backgrounds. Default is without." << std::endl;
    std::cout << "  --scale=<%>           Scale the output. Default is 100." << std::endl;
    std::cout << "  --delay=<ms>          Wait after page load before creating PDF. Default is 0." << std::endl;
    std::cout << "  --wait-signal         Wait for JavaScript signal before creating PDF." << std::endl;
    std::cout << "  --wait-signal-timeout=<ms>  Timeout for wait-signal before printing. Default is 0 (no timeout)." << std::endl;
    std::cout << "  --savehtml=<path>     Save generated DOM HTML before creating PDF." << std::endl;
    std::cout << "  --staticonly          Remove <script> tags from saved HTML snapshot." << std::endl;
    std::cout << "  --viewwidth=<px>      Width of viewport. Image default is 1280." << std::endl;
    std::cout << "  --viewheight=<px>     Height of viewport. Image default is 720." << std::endl;
    std::cout << "  --headerfooter        PDF rendered with a page header and footer." << std::endl;
    std::cout << "  --headertitle=<title> Title to render in the PDF page header." << std::endl;
    std::cout << "  --footerurl=<title>   URL to render in the PDF page footer." << std::endl;
    std::cout << std::endl;
    std::cout << "Server options:" << std::endl;
    std::cout << "  --server              Start HTTP server" << std::endl;
    std::cout << "  --host=<host>         If starting server, specify ip address to bind to." << std::endl;
    std::cout << "                        Default is " << cefpdf::constants::serverHost << std::endl;
    std::cout << "  --port=<port>         Specify server port number. Default is " << cefpdf::constants::serverPort << std::endl;
    std::cout << "  --profile=<folder>    Specify a custom folder for the chrome profile" << std::endl; 
    std::cout << "  --disable-gpu         Disable GPU acceleration and GPU compositing for headless servers." << std::endl;
#if defined(OS_WIN)
    std::cout << "  --dump-file-prefix=<path_prefix>  Enable unhandled exception dumps (.dmp)." << std::endl;
    std::cout << "                        Prefix includes directory and file name prefix." << std::endl;
    std::cout << "  --max-dump-files=<n>  Max number of dump files to keep (default: 5)." << std::endl;
#endif // OS_WIN
    std::cout << std::endl;
    std::cout << "Output:" << std::endl;
    std::cout << "  Output file name. Format is inferred from its extension; stdout defaults to PDF." << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  cef-pdf --file=invoice.html invoice.pdf" << std::endl;
    std::cout << "  cef-pdf --url=https://example.com page.pdf" << std::endl;
    std::cout << "  cef-pdf --file=chart.html chart.png" << std::endl;
    std::cout << "  cef-pdf --quality=85 --url=https://example.com page.jpg" << std::endl;
    std::cout << "  cef-pdf --capture=viewport --viewwidth=1280 --viewheight=720 --file=page.html page.bmp" << std::endl;
    std::cout << std::endl;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int parseInteger(const std::string& value, int minimum, int maximum, const std::string& name)
{
    if (value.empty()) throw std::string(name + " value is empty");
    std::size_t consumed = 0;
    long parsed = 0;
    try { parsed = std::stol(value, &consumed); }
    catch (...) { throw std::string("invalid " + name); }
    if (consumed != value.size() || parsed < minimum || parsed > maximum)
        throw std::string(name + " is out of range");
    return static_cast<int>(parsed);
}

cefpdf::job::Job::OutputFormat parseFormat(const std::string& value)
{
    const std::string format = lower(value);
    if (format == "pdf") return cefpdf::job::Job::OutputFormat::PDF;
    if (format == "png") return cefpdf::job::Job::OutputFormat::PNG;
    if (format == "jpg" || format == "jpeg") return cefpdf::job::Job::OutputFormat::JPEG;
    if (format == "bmp") return cefpdf::job::Job::OutputFormat::BMP;
    throw std::string("unsupported output format: " + value);
}

bool inferFormat(const std::string& path, cefpdf::job::Job::OutputFormat& format)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string extension = lower(path.substr(dot + 1));
    if (extension != "pdf" && extension != "png" && extension != "jpg" &&
        extension != "jpeg" && extension != "bmp") return false;
    format = parseFormat(extension);
    return true;
}

cefpdf::job::Job::ImageBackground parseBackground(const std::string& input)
{
    const std::string value = lower(input);
    if (value == "white") return {255, 255, 255, 255};
    if (value == "black") return {0, 0, 0, 255};
    if (value == "red") return {255, 0, 0, 255};
    if (value == "green") return {0, 128, 0, 255};
    if (value == "blue") return {0, 0, 255, 255};
    if (value.size() == 7 && value[0] == '#') {
        try {
            std::size_t consumed = 0;
            unsigned long rgb = std::stoul(value.substr(1), &consumed, 16);
            if (consumed != 6) throw std::string("invalid image background color");
            return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
                static_cast<uint8_t>(rgb), 255};
        } catch (...) {}
    }
    throw std::string("invalid image background color");
}

std::string getExecutableName(CefRefPtr<CefCommandLine> commandLine)
{
    std::string program = commandLine->GetProgram().ToString();

    // Remove directory if present.
    // Do this before extension removal in case directory has a period character.
    const std::size_t s = program.find_last_of("\\/");
    if (std::string::npos != s) {
        program.erase(0, s + 1);
    }

    // Remove extension if present.
    const std::size_t e = program.rfind('.');
    if (std::string::npos != e) {
        program.erase(e);
    }

    return program;
}

int runJob(CefRefPtr<cefpdf::Client> app, CefRefPtr<CefCommandLine> commandLine)
{
    CefRefPtr<cefpdf::job::Job> job;
    bool readFromStdIn = false;
    bool writeToStdOut = false;
    CefCommandLine::ArgumentList args;
    commandLine->GetArguments(args);

    try {
        if (commandLine->HasSwitch("stdin")) {
            job = new cefpdf::job::StdInput();
            readFromStdIn = true;
        } else {
            std::string url;

            if (commandLine->HasSwitch("url")) {
                url = commandLine->GetSwitchValue("url").ToString();
                if (url.empty() && !args.empty()) {
                    url = args.front();
                    args.erase(args.begin());
                }
            } else if (commandLine->HasSwitch("file")) {
                auto path = commandLine->GetSwitchValue("file").ToString();
                if (path.empty() && !args.empty()) {
                    path = args.front();
                    args.erase(args.begin());
                }

                if (!cefpdf::fileExists(path)) {
                    throw std::string("input file does not exist");
                }

                url = cefpdf::pathToUri(path);
            }

            if (url.empty()) {
                throw std::string("no input specified");
            }

            job = new cefpdf::job::Remote(url);
        }

        // Set output file
        if (!args.empty()) {
            job->SetOutputPath(args[0]);
        } else {
            writeToStdOut = true;
        }

        cefpdf::job::Job::OutputFormat inferred = cefpdf::job::Job::OutputFormat::PDF;
        const bool hasInferred = !args.empty() && inferFormat(args[0], inferred);
        const bool hasExplicit = commandLine->HasSwitch("format");
        const auto format = hasExplicit ? parseFormat(commandLine->GetSwitchValue("format").ToString())
                                        : (hasInferred ? inferred : cefpdf::job::Job::OutputFormat::PDF);
        if (hasExplicit && hasInferred && format != inferred)
            throw std::string("explicit format conflicts with output extension");
        job->SetOutputFormat(format);
        const bool imageOutput = format != cefpdf::job::Job::OutputFormat::PDF;
        if (imageOutput) {
            job->SetViewWidth(1280);
            job->SetViewHeight(720);
            const char* pdfSwitches[] = {"size", "margin", "landscape", "backgrounds", "scale",
                "pageheaderfooter", "headerfooter", "headertitle", "footerurl"};
            for (const char* option : pdfSwitches) {
                if (commandLine->HasSwitch(option))
                    throw std::string(std::string("--") + option + " is only valid for PDF output");
            }
        } else if (commandLine->HasSwitch("capture") || commandLine->HasSwitch("quality") ||
                   commandLine->HasSwitch("image-background")) {
            throw std::string("image options require PNG, JPEG, or BMP output");
        }

        if (commandLine->HasSwitch("capture")) {
            const std::string capture = lower(commandLine->GetSwitchValue("capture").ToString());
            if (capture == "full") job->SetCaptureMode(cefpdf::job::Job::CaptureMode::FULL);
            else if (capture == "viewport") job->SetCaptureMode(cefpdf::job::Job::CaptureMode::VIEWPORT);
            else throw std::string("capture must be full or viewport");
        }
        if (commandLine->HasSwitch("quality")) {
            if (format != cefpdf::job::Job::OutputFormat::JPEG)
                throw std::string("quality is only valid for JPEG output");
            job->SetImageQuality(parseInteger(commandLine->GetSwitchValue("quality").ToString(), 0, 100, "quality"));
        }
        if (commandLine->HasSwitch("image-background")) {
            if (format == cefpdf::job::Job::OutputFormat::PNG)
                throw std::string("image-background is only valid for JPEG or BMP output");
            job->SetImageBackground(parseBackground(commandLine->GetSwitchValue("image-background").ToString()));
        }

        if (commandLine->HasSwitch("size")) {
            job->SetPageSize(commandLine->GetSwitchValue("size"));
        }

        if (commandLine->HasSwitch("margin")) {
            job->SetPageMargin(commandLine->GetSwitchValue("margin"));
        }

        if (commandLine->HasSwitch("landscape")) {
            job->SetLandscape();
        }

        if (commandLine->HasSwitch("pageheaderfooter") || commandLine->HasSwitch("headerfooter"))
        {
            job->SetHeaderFooterEnabled();
        }

        if (commandLine->HasSwitch("headertitle")) 
        {
            job->SetHeaderFooterTitle(commandLine->GetSwitchValue("headertitle").ToString());
        }
        if (commandLine->HasSwitch("footerurl")) 
        {
            job->SetHeaderFooterUrl(commandLine->GetSwitchValue("footerurl").ToString());
        }


        if (commandLine->HasSwitch("backgrounds")) {
            job->SetBackgrounds();
        }

        if (commandLine->HasSwitch("scale")) {
            job->SetScale(std::atoi(commandLine->GetSwitchValue("scale").ToString().c_str()));
        }

        if (commandLine->HasSwitch("delay")) {
            job->SetDelay(parseInteger(commandLine->GetSwitchValue("delay").ToString(), 0, INT_MAX, "delay"));
        }

        if (commandLine->HasSwitch("wait-signal")) {
            job->SetWaitForSignal(true);
        }

        if (commandLine->HasSwitch("wait-signal-timeout")) {
            job->SetWaitSignalTimeout(parseInteger(commandLine->GetSwitchValue("wait-signal-timeout").ToString(), 0, INT_MAX, "wait-signal-timeout"));
        }

        if (commandLine->HasSwitch("savehtml")) {
            std::string saveHtmlPath = commandLine->GetSwitchValue("savehtml").ToString();
            if (saveHtmlPath.empty()) {
                throw std::string("savehtml output path is empty");
            }

            job->SetSaveHtmlPath(saveHtmlPath);
        }

        if (commandLine->HasSwitch("staticonly")) {
            if (!commandLine->HasSwitch("savehtml")) {
                throw std::string("staticonly requires savehtml");
            }

            job->SetSaveHtmlStaticOnly(true);
        }

        if (commandLine->HasSwitch("viewwidth")) {
            job->SetViewWidth(parseInteger(commandLine->GetSwitchValue("viewwidth").ToString(), 1, 32767, "viewwidth"));
        }

        if (commandLine->HasSwitch("viewheight")) {
            job->SetViewHeight(parseInteger(commandLine->GetSwitchValue("viewheight").ToString(), 1, 32767, "viewheight"));
        }
        if (imageOutput && static_cast<long long>(job->GetViewWidth()) * job->GetViewHeight() > 100000000LL) {
            throw std::string("image viewport exceeds the 100 million pixel limit");
        }

    } catch (std::string error) {
        std::cerr << "ERROR: " << error << std::endl;
        app->Shutdown();
        return 1;
    }

    if (readFromStdIn && !writeToStdOut) {
        std::cout << "Waiting for input until EOF (Unix: Ctrl+D, Windows: Ctrl+Z)" << std::endl;
    }

    app->SetStopAfterLastJob(true);
    app->AddJob(job);
    app->Run();

    if (writeToStdOut && job->GetStatus() == cefpdf::job::Job::Status::SUCCESS) {
#if defined(OS_WIN)
        // On Windows, in text mode, new line characters are translated to CRLF
        // So we need to switch to binary mode
        _setmode(_fileno(stdout), _O_BINARY);
#endif // OS_WIN
        std::cout << cefpdf::loadTempFile(job->GetOutputPath());
    }

    switch (job->GetStatus()) {
        case cefpdf::job::Job::Status::SUCCESS:
            std::clog << "Rendering document finished successfully" << std::endl;
            break;
        case cefpdf::job::Job::Status::OUTPUT_ERROR:
            std::clog << "Rendering document failed!!" << std::endl;
            return 1;
        default:
            std::clog << "Loading document failed!!" << std::endl;
            return 1;
    }

    return 0;
}

int runServer(CefRefPtr<cefpdf::Client> app, CefRefPtr<CefCommandLine> commandLine)
{
    std::string port = cefpdf::constants::serverPort;
    if (commandLine->HasSwitch("port")) {
        port = commandLine->GetSwitchValue("port").ToString();
    }

    std::string host = cefpdf::constants::serverHost;
    if (commandLine->HasSwitch("host")) {
        host = commandLine->GetSwitchValue("host").ToString();
    }

    CefRefPtr<cefpdf::server::Server> server = new cefpdf::server::Server(app, host, port);

    server->Start();

    return 0;
}

int runStreamed(CefRefPtr<cefpdf::Client> app, cefpdf::stream::ProtocolHandle output)
{
    cefpdf::stream::StreamedServer server(app, output);
    server.Start();
    app->Run();
    server.Join();
    return server.GetExitCode();
}

int main(int argc, char* argv[])
{
    CefRefPtr<cefpdf::Client> app = new cefpdf::Client();

#if defined(OS_WIN)
    CefMainArgs mainArgs(::GetModuleHandle(NULL));
#else
    CefMainArgs mainArgs(argc, argv);
#endif // OS_WIN

    CefRefPtr<CefCommandLine> commandLine = CefCommandLine::CreateCommandLine();

#if defined(OS_WIN)
    commandLine->InitFromString(::GetCommandLine());
#else
    commandLine->InitFromArgv(argc, argv);
#endif // OS_WIN

    const bool mainProcess = !commandLine->HasSwitch("type");
    const bool streamed = mainProcess && commandLine->HasSwitch("streamed") &&
        !commandLine->HasSwitch("help") && !commandLine->HasSwitch("h") &&
        !commandLine->HasSwitch("list-sizes");
    cefpdf::stream::ProtocolHandle protocolOutput =
#if defined(OS_WIN)
        nullptr;
#else
        -1;
#endif

    if (streamed) {
        CefCommandLine::ArgumentList arguments;
        commandLine->GetArguments(arguments);
        const char* jobSwitches[] = {
            "server", "stdin", "url", "file", "format", "capture", "quality",
            "image-background", "size", "margin", "landscape", "backgrounds",
            "scale", "delay", "wait-signal", "wait-signal-timeout", "savehtml",
            "staticonly", "viewwidth", "viewheight", "pageheaderfooter",
            "headerfooter", "headertitle", "footerurl"
        };
        bool hasJobSwitch = false;
        for (const char* name : jobSwitches) {
            if (commandLine->HasSwitch(name)) {
                hasJobSwitch = true;
                break;
            }
        }
        if (hasJobSwitch || !arguments.empty()) {
            std::cerr << "ERROR: --streamed cannot be combined with one-shot or HTTP server options" << std::endl;
            return 1;
        }
        std::string error;
        if (!cefpdf::stream::PrepareProtocolStdout(protocolOutput, error)) {
            std::cerr << "ERROR: " << error << std::endl;
            return 1;
        }
    }

#if !defined(OS_MACOSX)
    // Execute the sub-process logic, if any. This will either return immediately for the browser
    // process or block until the sub-process should exit.
    int exitCode = app->ExecuteSubProcess(mainArgs);
    if (exitCode >= 0) {
        // The sub-process terminated, exit now.
        if (streamed) cefpdf::stream::CloseProtocolHandle(protocolOutput);
        return exitCode;
    }
#endif // !OS_MACOSX

#if defined(OS_WIN)
    int maxDumpFiles = 5;
    if (commandLine->HasSwitch("max-dump-files")) {
        maxDumpFiles = std::atoi(commandLine->GetSwitchValue("max-dump-files").ToString().c_str());
        if (maxDumpFiles <= 0) {
            std::cerr << "WARNING: max-dump-files must be greater than zero. Using default value 5." << std::endl;
            maxDumpFiles = 5;
        }
    }

    if (commandLine->HasSwitch("dump-file-prefix")) {
        const std::string dumpFilePrefix = commandLine->GetSwitchValue("dump-file-prefix").ToString();
        if (dumpFilePrefix.empty()) {
            std::cerr << "ERROR: dump-file-prefix value is empty" << std::endl;
            return 1;
        }

        cefpdf::ConfigureWindowsCrashDumps(dumpFilePrefix, maxDumpFiles);
    }
#endif // OS_WIN

    if (commandLine->HasSwitch("help") || commandLine->HasSwitch("h")) {
        printHelp(getExecutableName(commandLine));
        return 0;
    }

    if (commandLine->HasSwitch("list-sizes")) {
        printSizes();
        return 0;
    }
    app->Initialize(mainArgs,commandLine);
    app->SetDisableJavaScript(!commandLine->HasSwitch("javascript"));

    int result = 0;
    if (streamed) result = runStreamed(app, protocolOutput);
    else result = commandLine->HasSwitch("server") ? runServer(app, commandLine) : runJob(app, commandLine);
    if (streamed) cefpdf::stream::CloseProtocolHandle(protocolOutput);
    return result;
}

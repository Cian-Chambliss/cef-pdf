#include "Screenshotter.h"
#include "Manager.h"
#include "../Common.h"

#include "include/cef_image.h"
#include "include/cef_parser.h"
#include "include/cef_values.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace cefpdf {
namespace job {

namespace {
const double kMaxDimension = 32767.0;
const double kMaxPixels = 100000000.0;

void AppendLe16(std::vector<uint8_t>& output, uint16_t value)
{
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendLe32(std::vector<uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 24));
}

double GetNumber(CefRefPtr<CefDictionaryValue> dictionary, const CefString& key)
{
    CefRefPtr<CefValue> value = dictionary->GetValue(key);
    if (!value) return 0;
    if (value->GetType() == VTYPE_INT) return value->GetInt();
    if (value->GetType() == VTYPE_DOUBLE) return value->GetDouble();
    return 0;
}
}

Screenshotter::Screenshotter(CefRefPtr<Manager> manager,
    CefRefPtr<CefBrowser> browser, CefRefPtr<Job> job) :
    m_manager(manager), m_browser(browser), m_job(job), m_layoutRequest(0),
    m_backgroundRequest(0), m_captureRequest(0), m_captureWidth(0),
    m_captureHeight(0), m_completed(false)
{}

void Screenshotter::Start()
{
    CEF_REQUIRE_UI_THREAD();
    m_registration = m_browser->GetHost()->AddDevToolsMessageObserver(this);
    if (!m_registration) {
        Complete(false);
        return;
    }

    CefPostDelayedTask(TID_UI, base::BindOnce(&Screenshotter::OnTimeout,
        CefRefPtr<Screenshotter>(this)), 30000);
    if (m_job->GetCaptureMode() == Job::CaptureMode::FULL) {
        RequestLayoutMetrics();
    } else {
        RequestBackground(m_job->GetViewWidth(), m_job->GetViewHeight());
    }
}

void Screenshotter::RequestLayoutMetrics()
{
    m_layoutRequest = m_browser->GetHost()->ExecuteDevToolsMethod(
        0, "Page.getLayoutMetrics", nullptr);
    if (!m_layoutRequest) Complete(false);
}

void Screenshotter::RequestBackground(double width, double height)
{
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > kMaxDimension || height > kMaxDimension || width * height > kMaxPixels) {
        Complete(false);
        return;
    }
    m_captureWidth = width;
    m_captureHeight = height;

    Job::ImageBackground color = m_job->GetImageBackground();
    if (m_job->GetOutputFormat() == Job::OutputFormat::PNG) {
        color = {0, 0, 0, 0};
    }
    CefRefPtr<CefDictionaryValue> rgba = CefDictionaryValue::Create();
    rgba->SetInt("r", color.red);
    rgba->SetInt("g", color.green);
    rgba->SetInt("b", color.blue);
    rgba->SetDouble("a", static_cast<double>(color.alpha) / 255.0);
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetDictionary("color", rgba);
    m_backgroundRequest = m_browser->GetHost()->ExecuteDevToolsMethod(
        0, "Emulation.setDefaultBackgroundColorOverride", params);
    if (!m_backgroundRequest) Complete(false);
}

void Screenshotter::RequestCapture(double width, double height)
{
    CefRefPtr<CefDictionaryValue> clip = CefDictionaryValue::Create();
    clip->SetDouble("x", 0);
    clip->SetDouble("y", 0);
    clip->SetDouble("width", width);
    clip->SetDouble("height", height);
    clip->SetDouble("scale", 1);

    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("format", m_job->GetOutputFormat() == Job::OutputFormat::JPEG ? "jpeg" : "png");
    if (m_job->GetOutputFormat() == Job::OutputFormat::JPEG) {
        params->SetInt("quality", m_job->GetImageQuality());
    }
    params->SetBool("fromSurface", true);
    params->SetBool("captureBeyondViewport", m_job->GetCaptureMode() == Job::CaptureMode::FULL);
    params->SetDictionary("clip", clip);
    m_captureRequest = m_browser->GetHost()->ExecuteDevToolsMethod(
        0, "Page.captureScreenshot", params);
    if (!m_captureRequest) Complete(false);
}

void Screenshotter::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
    int message_id, bool success, const void* result, size_t result_size)
{
    CEF_REQUIRE_UI_THREAD();
    if (m_completed || !browser->IsSame(m_browser) || !success) {
        if (!m_completed && browser->IsSame(m_browser)) {
            std::cerr << "Image capture DevTools request failed (id=" << message_id << "): "
                << std::string(static_cast<const char*>(result), result_size) << std::endl;
            Complete(false);
        }
        return;
    }

    CefRefPtr<CefValue> value = CefParseJSON(result, result_size, JSON_PARSER_RFC);
    CefRefPtr<CefDictionaryValue> dictionary = value ? value->GetDictionary() : nullptr;
    if (!dictionary) {
        std::cerr << "Image capture returned malformed JSON" << std::endl;
        Complete(false);
        return;
    }

    if (message_id == m_layoutRequest) {
        CefRefPtr<CefDictionaryValue> size = dictionary->GetDictionary("cssContentSize");
        if (!size) {
            std::cerr << "Image capture layout metrics omitted cssContentSize" << std::endl;
            Complete(false);
            return;
        }
        const double width = std::ceil(GetNumber(size, "width"));
        const double height = std::ceil(GetNumber(size, "height"));
        RequestBackground(width, height);
    } else if (message_id == m_backgroundRequest) {
        RequestCapture(m_captureWidth, m_captureHeight);
    } else if (message_id == m_captureRequest) {
        const bool saved = SaveResult(dictionary->GetString("data"));
        if (!saved) std::cerr << "Image capture result could not be encoded or written" << std::endl;
        Complete(saved);
    }
}

bool Screenshotter::SaveResult(const CefString& encoded)
{
    if (encoded.empty()) return false;
    CefRefPtr<CefBinaryValue> bytes = CefBase64Decode(encoded);
    if (!bytes || bytes->GetSize() == 0) return false;
    if (m_job->GetOutputFormat() == Job::OutputFormat::BMP) {
        return SaveBmp(bytes->GetRawData(), bytes->GetSize());
    }
    return writeBinaryFile(m_job->GetOutputPath().ToString(), bytes->GetRawData(), bytes->GetSize());
}

bool Screenshotter::SaveBmp(const void* pngData, size_t pngSize)
{
    CefRefPtr<CefImage> image = CefImage::CreateImage();
    if (!image || !image->AddPNG(1.0f, pngData, pngSize)) return false;
    int width = 0, height = 0;
    CefRefPtr<CefBinaryValue> bitmap = image->GetAsBitmap(1.0f,
        CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, width, height);
    if (!bitmap || width <= 0 || height <= 0) return false;

    const uint8_t* source = static_cast<const uint8_t*>(bitmap->GetRawData());
    const uint32_t stride = (static_cast<uint32_t>(width) * 3u + 3u) & ~3u;
    const uint32_t pixelBytes = stride * static_cast<uint32_t>(height);
    const uint32_t fileSize = 54u + pixelBytes;
    std::vector<uint8_t> output;
    output.reserve(fileSize);
    AppendLe16(output, 0x4D42);
    AppendLe32(output, fileSize);
    AppendLe16(output, 0); AppendLe16(output, 0);
    AppendLe32(output, 54);
    AppendLe32(output, 40);
    AppendLe32(output, static_cast<uint32_t>(width));
    AppendLe32(output, static_cast<uint32_t>(height));
    AppendLe16(output, 1); AppendLe16(output, 24);
    AppendLe32(output, 0); AppendLe32(output, pixelBytes);
    AppendLe32(output, 2835); AppendLe32(output, 2835);
    AppendLe32(output, 0); AppendLe32(output, 0);

    const Job::ImageBackground background = m_job->GetImageBackground();
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* row = source + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const uint8_t alpha = row[x * 4 + 3];
            const unsigned inverse = 255u - alpha;
            output.push_back(static_cast<uint8_t>(std::min(255u, row[x * 4] + (background.blue * inverse + 127u) / 255u)));
            output.push_back(static_cast<uint8_t>(std::min(255u, row[x * 4 + 1] + (background.green * inverse + 127u) / 255u)));
            output.push_back(static_cast<uint8_t>(std::min(255u, row[x * 4 + 2] + (background.red * inverse + 127u) / 255u)));
        }
        while ((output.size() - 54u) % stride != 0) output.push_back(0);
    }
    return writeBinaryFile(m_job->GetOutputPath().ToString(), output.data(), output.size());
}

void Screenshotter::OnDevToolsAgentDetached(CefRefPtr<CefBrowser> browser)
{
    if (!m_completed && browser->IsSame(m_browser)) Complete(false);
}

void Screenshotter::OnTimeout()
{
    if (!m_completed) Complete(false);
}

void Screenshotter::Complete(bool ok)
{
    if (m_completed) return;
    m_completed = true;
    if (!CefPostTask(TID_UI, base::BindOnce(&Screenshotter::Finalize,
        CefRefPtr<Screenshotter>(this), ok))) {
        Finalize(ok);
    }
}

void Screenshotter::Finalize(bool ok)
{
    CefRefPtr<Screenshotter> keepAlive(this);
    m_registration = nullptr;
    CefRefPtr<Manager> manager = m_manager;
    CefRefPtr<CefBrowser> browser = m_browser;
    m_job = nullptr;
    m_browser = nullptr;
    m_manager = nullptr;
    manager->Finish(browser, "", ok);
}

} // namespace job
} // namespace cefpdf

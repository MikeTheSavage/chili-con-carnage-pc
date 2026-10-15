#pragma once
#include "pad_sample.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#pragma comment(lib, "hid.lib")
namespace chili {
// PlayStation pads without a helper program. XInput does not list them; Windows shows them as HID game
// controllers (usage page 1, usage 5 or 4, vendor 054C). The game window registers for their Raw Input
// reports (PcInput::attach), and each report is read here into the same PadSample an Xbox-style pad gives.
//
// NOTHING IN THIS FILE HAS SEEN A REAL PAD: no controller exists on the PC it was written on. The report
// layouts, the product ids, the Bluetooth check sum and the vibration reports are from general knowledge
// of these devices. CHILI_HID_DUMP=1 logs what a real pad sends and how it was read, so it can be checked.
//
// Two readers:
// - the system's HID parser (hid.dll: HidP_GetUsages, HidP_GetUsageValue) over the device's own
//   description of its report. It needs the device (its "preparsed data"), so it cannot be tested on bytes
//   alone. It reads the USB report and the plain Bluetooth report of both pads.
// - the known byte layouts below, which can. The game uses them for the long Bluetooth reports, which the
//   device describes only as vendor data (accepted only when their CRC-32 is right), and compares them with
//   the system's reading of the other reports: a difference is counted and logged once.

// What a report says, by HID usage. Buttons are on the Button page, the rest on Generic Desktop.
struct HidPadReading {
    std::uint32_t buttons{};      // bit n-1: button n is down
    std::uint8_t hat{8};          // 0 up, then clockwise to 7 up-left; 8 released
    float x{}, y{}, z{}, rz{};    // X, Y: left stick; Z, Rz: right stick; -1 to 1, right and down positive
    float rx{}, ry{};             // Rx, Ry: left and right trigger, 0 to 1
    bool triggers{};              // the report carries the two trigger axes
};
inline bool hid_pad_readings_agree(const HidPadReading &a, const HidPadReading &b) noexcept {
    const auto close = [](float p, float q) { return p - q < 0.02f && q - p < 0.02f; };
    return a.buttons == b.buttons && a.hat == b.hat && close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z) && close(a.rz, b.rz) &&
           a.triggers == b.triggers && close(a.rx, b.rx) && close(a.ry, b.ry);
}
// A stick axis with the logical range low..high as -1 to 1 (128 of 0..255 is the middle), a trigger as 0 to 1.
inline float hid_stick_axis(long value, long low, long high) noexcept {
    if (high <= low) return 0.0f;
    const long middle = (low + high + 1) / 2, half = (high - low) / 2;
    return std::clamp(static_cast<float>(value - middle) / static_cast<float>(half > 0 ? half : 1), -1.0f, 1.0f);
}
inline float hid_trigger_axis(long value, long low, long high) noexcept {
    if (high <= low) return 0.0f;
    return std::clamp(static_cast<float>(value - low) / static_cast<float>(high - low), 0.0f, 1.0f);
}

// The position map onto the sixteen logical buttons. Button usages of a PlayStation pad: 1 Square, 2 Cross,
// 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2, 9 Share or Create, 10 Options, 11 L3, 12 R3 (13 PS, 14 the
// touch pad's click and 15 mute are not mapped). The hat is the D-pad. L2 and R2 go by their axes, so
// [Input.Gamepad] TriggerThreshold applies as it does to an Xbox-style pad; a pad without those axes gives
// its two trigger buttons as full travel.
inline PadSample hid_pad_sample(const HidPadReading &reading, std::uint16_t vid, std::uint16_t pid, std::uint32_t packet = 0u) noexcept {
    static constexpr std::array<int, 12> by_usage{PadSample::X, PadSample::A, PadSample::B, PadSample::Y, PadSample::LB, PadSample::RB,
        -1, -1, PadSample::Back, PadSample::Start, PadSample::LS, PadSample::RS};
    constexpr std::uint16_t up = PadSample::bit(PadSample::DpadUp), right = PadSample::bit(PadSample::DpadRight),
        down = PadSample::bit(PadSample::DpadDown), left = PadSample::bit(PadSample::DpadLeft);
    static constexpr std::array<std::uint16_t, 8> by_hat{up, static_cast<std::uint16_t>(up | right), right, static_cast<std::uint16_t>(right | down),
        down, static_cast<std::uint16_t>(down | left), left, static_cast<std::uint16_t>(left | up)};
    PadSample sample;
    sample.kind = PadKind::PlayStation; sample.vid = vid; sample.pid = pid; sample.packet = packet;
    for (std::size_t i = 0; i < by_usage.size(); ++i)
        if (by_usage[i] >= 0 && (reading.buttons >> i & 1u)) sample.buttons = static_cast<std::uint16_t>(sample.buttons | PadSample::bit(static_cast<unsigned>(by_usage[i])));
    if (reading.hat < by_hat.size()) sample.buttons = static_cast<std::uint16_t>(sample.buttons | by_hat[reading.hat]);
    sample.lx = reading.x; sample.ly = reading.y; sample.rx = reading.z; sample.ry = reading.rz;
    if (reading.triggers) { sample.lt = reading.rx; sample.rt = reading.ry; }
    else { sample.lt = (reading.buttons >> 6 & 1u) ? 1.0f : 0.0f; sample.rt = (reading.buttons >> 7 & 1u) ? 1.0f : 0.0f; }
    return sample;
}

enum class HidPadModel : std::uint8_t { Unknown, DualShock4, DualSense };
inline constexpr std::uint16_t hid_sony_vendor = 0x054Cu, hid_microsoft_vendor = 0x045Eu;
// DualShock 4: 05C4 (first model), 09CC (second), 0BA0 (its wireless adapter). DualSense: 0CE6, Edge 0DF2.
inline HidPadModel hid_pad_model(std::uint16_t vid, std::uint16_t pid) noexcept {
    if (vid != hid_sony_vendor) return HidPadModel::Unknown;
    switch (pid) {
    case 0x05C4u: case 0x09CCu: case 0x0BA0u: return HidPadModel::DualShock4;
    case 0x0CE6u: case 0x0DF2u: return HidPadModel::DualSense;
    default: return HidPadModel::Unknown;
    }
}
inline const char *hid_pad_model_name(HidPadModel model) noexcept {
    return model == HidPadModel::DualShock4 ? "DualShock 4" : model == HidPadModel::DualSense ? "DualSense" : "PlayStation";
}

// CRC-32 (the zip one: reflected, polynomial EDB88320). Pass the result back in to go on with more bytes.
inline std::uint32_t hid_crc32(const std::uint8_t *data, std::size_t size, std::uint32_t crc = 0u) noexcept {
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
// A long Bluetooth report ends in the CRC-32 of the byte A1 (the Bluetooth header of an input report)
// followed by everything before the sum, low byte first.
inline bool hid_bluetooth_sum_ok(const std::uint8_t *report, std::size_t size) noexcept {
    if (size < 5u) return false;
    const std::uint8_t header = 0xA1u;
    const auto sum = hid_crc32(report, size - 4u, hid_crc32(&header, 1u));
    return report[size - 4u] == (sum & 0xFFu) && report[size - 3u] == (sum >> 8 & 0xFFu) && report[size - 2u] == (sum >> 16 & 0xFFu) &&
           report[size - 1u] == (sum >> 24 & 0xFFu);
}

// The byte layouts (general knowledge, see the head of this file). `report` starts with the report id, as
// Raw Input hands it over; `size` may be longer than the report (Raw Input pads to the device's longest).
//   DualShock 4, report 01 (USB 64 bytes, plain Bluetooth 10): 1-4 the sticks LX LY RX RY, 5 low nibble the
//     hat and bits 4-7 Square Cross Circle Triangle, 6 L1 R1 L2 R2 Share Options L3 R3, 7 bit 0 PS, bit 1 the
//     touch pad, 8-9 the triggers L2 R2.
//   DualShock 4, Bluetooth report 11 (78 bytes): the same fields from byte 3 on, CRC-32 in the last four.
//   DualSense, USB report 01 (64 bytes): 1-4 the sticks, 5-6 the triggers, 7 a counter, 8 the hat and the
//     four face buttons, 9 L1 R1 L2 R2 Create Options L3 R3, 10 bit 0 PS, bit 1 the touch pad, bit 2 mute.
//   DualSense, plain Bluetooth report 01 (10 bytes): laid out like the DualShock 4's report 01.
//   DualSense, Bluetooth report 31 (78 bytes): byte 1 a tag, then the USB fields from byte 2 on, CRC-32 last.
enum class HidPadLayout : std::uint8_t { None, DualShock4, DualShock4Bluetooth, DualSenseUsb, DualSenseBluetoothPlain, DualSenseBluetooth };
inline const char *hid_pad_layout_name(HidPadLayout layout) noexcept {
    switch (layout) {
    case HidPadLayout::DualShock4: return "dualshock4 report 01";
    case HidPadLayout::DualShock4Bluetooth: return "dualshock4 bluetooth report 11";
    case HidPadLayout::DualSenseUsb: return "dualsense usb report 01";
    case HidPadLayout::DualSenseBluetoothPlain: return "dualsense bluetooth report 01";
    case HidPadLayout::DualSenseBluetooth: return "dualsense bluetooth report 31";
    default: return "none";
    }
}
inline constexpr std::size_t hid_bluetooth_report_bytes = 78u;
// Reads `report` by the layout that fits the model, the connection and the report id. None: not a report
// this code knows, or a long Bluetooth report whose check sum is wrong; `out` is then left alone.
inline HidPadLayout hid_pad_decode(HidPadModel model, bool bluetooth, const std::uint8_t *report, std::size_t size, HidPadReading &out) noexcept {
    if (!report || size < 1u) return HidPadLayout::None;
    // sticks at `sticks`, the three button bytes at `buttons`, the triggers at `triggers`.
    const auto fields = [&](std::size_t sticks, std::size_t buttons, std::size_t triggers, std::uint8_t last_mask) {
        HidPadReading reading;
        reading.x = hid_stick_axis(report[sticks], 0, 255); reading.y = hid_stick_axis(report[sticks + 1u], 0, 255);
        reading.z = hid_stick_axis(report[sticks + 2u], 0, 255); reading.rz = hid_stick_axis(report[sticks + 3u], 0, 255);
        const std::uint8_t hat = report[buttons] & 0x0Fu;
        reading.hat = hat > 7u ? std::uint8_t{8u} : hat;
        reading.buttons = static_cast<std::uint32_t>(report[buttons] >> 4) | static_cast<std::uint32_t>(report[buttons + 1u]) << 4 |
                          static_cast<std::uint32_t>(report[buttons + 2u] & last_mask) << 12;
        reading.rx = hid_trigger_axis(report[triggers], 0, 255); reading.ry = hid_trigger_axis(report[triggers + 1u], 0, 255);
        reading.triggers = true;
        out = reading;
    };
    const std::uint8_t id = report[0];
    if (model == HidPadModel::DualShock4) {
        if (id == 0x01u && size >= 10u) { fields(1u, 5u, 8u, 0x03u); return HidPadLayout::DualShock4; }
        if (id == 0x11u && size >= hid_bluetooth_report_bytes && hid_bluetooth_sum_ok(report, hid_bluetooth_report_bytes)) {
            fields(3u, 7u, 10u, 0x03u); return HidPadLayout::DualShock4Bluetooth;
        }
    } else if (model == HidPadModel::DualSense) {
        if (id == 0x01u && !bluetooth && size >= 11u) { fields(1u, 8u, 5u, 0x07u); return HidPadLayout::DualSenseUsb; }
        if (id == 0x01u && bluetooth && size >= 10u) { fields(1u, 5u, 8u, 0x03u); return HidPadLayout::DualSenseBluetoothPlain; }
        if (id == 0x31u && size >= hid_bluetooth_report_bytes && hid_bluetooth_sum_ok(report, hid_bluetooth_report_bytes)) {
            fields(2u, 9u, 6u, 0x07u); return HidPadLayout::DualSenseBluetooth;
        }
    }
    return HidPadLayout::None;
}

// The report that sets the two motors of a pad connected by USB (general knowledge, never sent to a pad
// from this PC). `left` is the large, slow motor, `right` the small, fast one, 0 to 255. Only the motors are
// marked as valid, so the light bar and the DualSense's triggers keep what they have.
//   DualShock 4, report 05, 32 bytes: 1 = 01 (motors valid), 4 the small motor, 5 the large one.
//   DualSense, report 02, 48 bytes: 1 = 03 (vibration valid, vibration instead of haptics), 3 the small
//     motor, 4 the large one.
// Returns the report's length, 0 for a model or a connection with no known report (Bluetooth needs a longer
// report with a CRC-32: not written here).
inline std::size_t hid_pad_rumble_report(HidPadModel model, bool bluetooth, std::uint8_t left, std::uint8_t right, std::array<std::uint8_t, 64> &report) noexcept {
    report.fill(0u);
    if (bluetooth) return 0u;
    if (model == HidPadModel::DualShock4) { report[0] = 0x05u; report[1] = 0x01u; report[4] = right; report[5] = left; return 32u; }
    if (model == HidPadModel::DualSense) { report[0] = 0x02u; report[1] = 0x03u; report[3] = right; report[4] = left; return 48u; }
    return 0u;
}

// What is known of a HID pad once it has been looked at.
struct HidPadInfo {
    std::uint16_t vid{}, pid{};
    HidPadModel model{HidPadModel::Unknown};
    std::string name;             // the device's product string
    bool bluetooth{};
    unsigned buttons{}, axes{};   // as the device describes itself
    bool hat{};
    // "PlayStation pad 054C:0CE6 'Wireless Controller' usb, 15 buttons 6 axes hat"
    std::string describe() const {
        char text[200];
        std::snprintf(text, sizeof(text), "PlayStation pad %04X:%04X '%s' %s, %u buttons %u axes%s", unsigned(vid), unsigned(pid), name.c_str(),
            bluetooth ? "bluetooth" : "usb", buttons, axes, hat ? " hat" : "");
        return text;
    }
};
inline std::string hid_hex(const std::uint8_t *data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 3u);
    for (std::size_t i = 0; i < size; ++i) { if (i) text.push_back(' '); text.push_back(digits[data[i] >> 4]); text.push_back(digits[data[i] & 15u]); }
    return text;
}
inline std::string hid_reading_text(const HidPadReading &reading) {
    char text[160];
    std::snprintf(text, sizeof(text), "buttons %04X hat %u x %+.2f y %+.2f z %+.2f rz %+.2f rx %.2f ry %.2f%s", unsigned(reading.buttons), unsigned(reading.hat),
        reading.x, reading.y, reading.z, reading.rz, reading.rx, reading.ry, reading.triggers ? "" : " (no trigger axes)");
    return text;
}
// CHILI_HID_DUMP=1: the first reports of each HID pad, and after them every report whose buttons, hat or
// axes differ from the last one logged, up to hid_dump_reports per pad. A line has the bytes in hex, what
// the system's parser made of them, what the known layout made of them and the logical buttons: copy the
// hex of a line to make a test fixture of it (tools/pc_input_test.cpp).
inline constexpr unsigned hid_dump_reports = 200u, hid_dump_first = 4u;
inline bool hid_dump_enabled() {
    static const bool enabled = [] { const char *value = std::getenv("CHILI_HID_DUMP"); return value && *value && *value != '0'; }();
    return enabled;
}

// Writes the vibration report to a pad on a thread of its own: a write to a HID device waits for the
// device, and the game's thread must not. Only the newest values are kept.
class HidPadOutput {
public:
    HidPadOutput(std::wstring path, HidPadModel model, bool bluetooth, unsigned report_bytes)
        : path_(std::move(path)), model_(model), bluetooth_(bluetooth), report_bytes_(report_bytes) {}
    ~HidPadOutput() {
        if (!worker_.joinable()) return;
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        worker_.join();
    }
    HidPadOutput(const HidPadOutput &) = delete;
    HidPadOutput &operator=(const HidPadOutput &) = delete;
    // Whether this pad has a vibration report this code knows and the device takes a report of that length.
    bool supported() const noexcept {
        std::array<std::uint8_t, 64> report;
        const auto size = hid_pad_rumble_report(model_, bluetooth_, 0u, 0u, report);
        return size != 0u && size == report_bytes_;
    }
    void set(std::uint8_t left, std::uint8_t right) {
        if (!supported()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            left_ = left; right_ = right; dirty_ = true;
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        }
        wake_.notify_all();
    }
    std::uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }
    std::uint64_t failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    // The Windows error of the first failure (opening the device or writing to it), 0 when none.
    unsigned long error() const noexcept { return error_.load(std::memory_order_relaxed); }

private:
    void fail(unsigned long code) noexcept {
        failed_.fetch_add(1u, std::memory_order_relaxed);
        unsigned long none = 0u;
        error_.compare_exchange_strong(none, code ? code : 1u, std::memory_order_relaxed);
    }
    void run() {
        HANDLE device = INVALID_HANDLE_VALUE;
        const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Whether the last report written left a motor running.
        bool running = false;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stop_ || dirty_; });
            // On the way out the motors are stopped if they were left running.
            const bool last = stop_;
            if (last && !running) break;
            const std::uint8_t left = last ? std::uint8_t{0u} : left_, right = last ? std::uint8_t{0u} : right_;
            dirty_ = false;
            lock.unlock();
            if (device == INVALID_HANDLE_VALUE) {
                device = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (device == INVALID_HANDLE_VALUE) fail(GetLastError());
            }
            if (device != INVALID_HANDLE_VALUE && done) {
                std::array<std::uint8_t, 64> report;
                const auto size = hid_pad_rumble_report(model_, bluetooth_, left, right, report);
                OVERLAPPED overlapped{};
                overlapped.hEvent = done;
                ResetEvent(done);
                DWORD count = 0;
                bool ok = WriteFile(device, report.data(), static_cast<DWORD>(size), nullptr, &overlapped) != FALSE;
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    // A pad that does not take the report within a fifth of a second is given up on for this one.
                    if (WaitForSingleObject(done, 200u) != WAIT_OBJECT_0) CancelIo(device);
                    ok = GetOverlappedResult(device, &overlapped, &count, TRUE) != FALSE;
                } else if (ok) ok = GetOverlappedResult(device, &overlapped, &count, TRUE) != FALSE;
                if (ok) { written_.fetch_add(1u, std::memory_order_relaxed); running = left || right; }
                else fail(GetLastError());
            }
            lock.lock();
            if (last) break;
        }
        lock.unlock();
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
        if (done) CloseHandle(done);
    }
    std::wstring path_;
    HidPadModel model_;
    bool bluetooth_;
    unsigned report_bytes_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic<std::uint64_t> written_{0u}, failed_{0u};
    std::atomic<unsigned long> error_{0u};
    std::uint8_t left_{}, right_{};
    bool dirty_{}, stop_{};
};

// One HID game controller as Raw Input presents it: what it is, and its reports read into HidPadReading.
class HidPadDevice {
public:
    using Log = std::function<void(const std::string &)>;
    enum class Refused : std::uint8_t { No, NotAPad, XInput, NotSony, Unreadable };
    // Looks at a Raw Input device. Null when it is not a pad this code reads: `refused` says why, and for a
    // pad of another make, or a Microsoft pad without an XInput path, `note` is the line for the log. `any_device` is for diagnostics and tests: any HID
    // device at all is taken (a mouse, a keyboard, a pad of another make), so the Windows side of this
    // class can be run on a PC that has no PlayStation pad.
    static std::unique_ptr<HidPadDevice> open(HANDLE raw, Refused &refused, std::string &note, bool any_device = false) {
        refused = Refused::NotAPad;
        RID_DEVICE_INFO device{};
        device.cbSize = sizeof(device);
        UINT size = sizeof(device);
        if (GetRawInputDeviceInfoW(raw, RIDI_DEVICEINFO, &device, &size) == static_cast<UINT>(-1) || device.dwType != RIM_TYPEHID) return nullptr;
        if (!any_device && (device.hid.usUsagePage != 1u || (device.hid.usUsage != 4u && device.hid.usUsage != 5u))) return nullptr;
        UINT characters = 0u;
        (void)GetRawInputDeviceInfoW(raw, RIDI_DEVICENAME, nullptr, &characters);
        std::wstring path(characters, L'\0');
        if (!characters || GetRawInputDeviceInfoW(raw, RIDI_DEVICENAME, path.data(), &characters) == static_cast<UINT>(-1)) { refused = Refused::Unreadable; return nullptr; }
        path.resize(path.find(L'\0') == std::wstring::npos ? path.size() : path.find(L'\0'));
        std::wstring upper = path;
        for (auto &c : upper) if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        const auto vid = static_cast<std::uint16_t>(device.hid.dwVendorId), pid = static_cast<std::uint16_t>(device.hid.dwProductId);
        // An Xbox-style pad shows here too; it is read through XInput.
        const bool xinput_path = upper.find(L"IG_") != std::wstring::npos;
        if (!any_device && xinput_path) { refused = Refused::XInput; return nullptr; }
        auto pad = std::unique_ptr<HidPadDevice>(new HidPadDevice);
        pad->path_ = path;
        pad->info_.vid = vid; pad->info_.pid = pid;
        pad->info_.model = hid_pad_model(pad->info_.vid, pad->info_.pid);
        // The Bluetooth HID service's id is in the path of a pad connected that way.
        pad->info_.bluetooth = upper.find(L"00001124-0000-1000-8000-00805F9B34FB") != std::wstring::npos || upper.find(L"BTHENUM") != std::wstring::npos;
        pad->info_.name = product_name(path);
        if (!any_device) {
            refused = refusal(vid, pid, xinput_path, pad->info_.name, note);
            if (refused != Refused::No) return nullptr;
        }
        refused = Refused::Unreadable;
        UINT bytes = 0u;
        if (GetRawInputDeviceInfoW(raw, RIDI_PREPARSEDDATA, nullptr, &bytes) != 0u || !bytes) return nullptr;
        pad->preparsed_.resize((bytes + 7u) / 8u);
        if (GetRawInputDeviceInfoW(raw, RIDI_PREPARSEDDATA, pad->preparsed_.data(), &bytes) == static_cast<UINT>(-1)) return nullptr;
        if (HidP_GetCaps(pad->preparsed(), &pad->caps_) != HIDP_STATUS_SUCCESS) return nullptr;
        pad->read_caps();
        refused = Refused::No;
        return pad;
    }
    // Whether a HID game controller is read here, by its maker and its device path: No for a PlayStation
    // pad; otherwise why not, and in `note` the line for the log when there is something to say. An Xbox pad
    // is XInput's: Windows marks its device path with IG_ (`xinput_path`), and a controller of Microsoft's
    // own is one whatever its path says (a pad of theirs that XInput does not list is not read at all).
    static Refused refusal(std::uint16_t vid, std::uint16_t pid, bool xinput_path, const std::string &name, std::string &note) {
        note.clear();
        if (xinput_path) return Refused::XInput;
        if (vid == hid_sony_vendor) return Refused::No;
        char text[200];
        if (vid == hid_microsoft_vendor) {
            std::snprintf(text, sizeof(text), "gamepad: HID %04X:%04X '%s' is a Microsoft pad: it is read through XInput", unsigned(vid), unsigned(pid), name.c_str());
            note = text;
            return Refused::XInput;
        }
        std::snprintf(text, sizeof(text), "gamepad: HID %04X:%04X '%s' ignored (not a PlayStation pad; use an XInput-compatible mode)",
            unsigned(vid), unsigned(pid), name.c_str());
        note = text;
        return Refused::NotSony;
    }
    const HidPadInfo &info() const noexcept { return info_; }
    // The length of the device's input reports as Raw Input hands them over, and the report id of its first
    // value or button (0 for a device without report ids).
    unsigned input_report_bytes() const noexcept { return caps_.InputReportByteLength; }
    std::uint8_t first_report_id() const noexcept {
        return !value_caps_.empty() ? value_caps_.front().ReportID : !button_caps_.empty() ? button_caps_.front().ReportID : std::uint8_t{0u};
    }
    void set_log(Log log) { log_ = std::move(log); }
    // What the device says about itself, for the dump.
    std::string caps_text() const {
        char text[160];
        std::snprintf(text, sizeof(text), "usage %X:%X, input report %u bytes, output %u, feature %u", unsigned(caps_.UsagePage), unsigned(caps_.Usage),
            unsigned(caps_.InputReportByteLength), unsigned(caps_.OutputReportByteLength), unsigned(caps_.FeatureReportByteLength));
        std::string out = text;
        for (const auto &cap : button_caps_) {
            std::snprintf(text, sizeof(text), "; buttons page %X usages %u-%u report %u", unsigned(cap.UsagePage),
                unsigned(cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage), unsigned(cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage), unsigned(cap.ReportID));
            out += text;
        }
        for (const auto &cap : value_caps_) {
            std::snprintf(text, sizeof(text), "; value page %X usage %X report %u, %u bits x%u, %ld..%ld", unsigned(cap.UsagePage),
                unsigned(cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage), unsigned(cap.ReportID), unsigned(cap.BitSize), unsigned(cap.ReportCount),
                static_cast<long>(cap.LogicalMin), static_cast<long>(cap.LogicalMax));
            out += text;
        }
        return out;
    }
    // One report, as Raw Input hands it over (the report id first). False when neither reader knows it.
    bool read(const std::uint8_t *report, std::size_t size, HidPadReading &out) {
        ++reports_;
        HidPadReading system, known;
        const bool by_system = read_system(report, size, system);
        const auto layout = hid_pad_decode(info_.model, info_.bluetooth, report, size, known);
        const bool by_layout = layout != HidPadLayout::None;
        if (by_system) { out = system; ++read_by_system_; }
        else if (by_layout) { out = known; ++read_by_layout_; }
        else ++unread_;
        if (by_system && by_layout && !hid_pad_readings_agree(system, known)) {
            if (!disagreements_++ && log_) log_("gamepad: " + ids() + " the system's reading of a report differs from the known layout (" +
                hid_pad_layout_name(layout) + "); the system's is used. CHILI_HID_DUMP=1 logs both");
        }
        if (hid_dump_enabled() && log_ && dumped_ < hid_dump_reports) {
            const HidPadReading *shown = by_system ? &system : by_layout ? &known : nullptr;
            const bool changed = shown && !same_for_dump(*shown, dump_last_);
            if (dumped_ < hid_dump_first || changed || (!shown && unread_ <= hid_dump_first)) {
                ++dumped_;
                if (shown) dump_last_ = *shown;
                std::string line = "hid " + ids() + " report " + std::to_string(reports_) + " (" + std::to_string(size) + " bytes): " +
                    hid_hex(report, std::min<std::size_t>(size, 80u)) + (size > 80u ? " ..." : "");
                line += by_system ? " | system: " + hid_reading_text(system) : std::string(" | system: not read");
                line += by_layout ? std::string(" | ") + hid_pad_layout_name(layout) + ": " + hid_reading_text(known) : std::string(" | no known layout");
                if (shown) {
                    char text[32];
                    const auto sample = hid_pad_sample(*shown, info_.vid, info_.pid);
                    std::snprintf(text, sizeof(text), " | pad buttons %04X", unsigned(sample.with_triggers(0.12f)));
                    line += text;
                }
                log_(line);
            }
        }
        return by_system || by_layout;
    }
    std::string ids() const {
        char text[16];
        std::snprintf(text, sizeof(text), "%04X:%04X", unsigned(info_.vid), unsigned(info_.pid));
        return text;
    }
    // "reports=.. read by the system's parser=.. by the known layout=.. not read=.. disagreements=.."
    std::string counts_text() const {
        char text[200];
        std::snprintf(text, sizeof(text), "reports=%llu, read by the system's parser=%llu, by the known layout=%llu, not read=%llu, readings that differed=%llu",
            static_cast<unsigned long long>(reports_), static_cast<unsigned long long>(read_by_system_), static_cast<unsigned long long>(read_by_layout_),
            static_cast<unsigned long long>(unread_), static_cast<unsigned long long>(disagreements_));
        return text;
    }
    // The motors: 0 to 255 each. Nothing happens for a pad or a connection without a known report.
    bool can_rumble() { return output().supported(); }
    void rumble(std::uint8_t left, std::uint8_t right) { output().set(left, right); }
    std::string rumble_text() {
        if (!output_) return "no vibration report was sent";
        char text[120];
        std::snprintf(text, sizeof(text), "vibration reports written=%llu failed=%llu (first error %lu)", static_cast<unsigned long long>(output_->written()),
            static_cast<unsigned long long>(output_->failed()), output_->error());
        return text;
    }

private:
    HidPadDevice() = default;
    struct Axis { USAGE usage{}; long low{}, high{}; unsigned bits{}; bool present{}; };
    PHIDP_PREPARSED_DATA preparsed() const noexcept { return reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<std::uint64_t *>(preparsed_.data())); }
    HidPadOutput &output() {
        if (!output_) output_ = std::make_unique<HidPadOutput>(path_, info_.model, info_.bluetooth, unsigned(caps_.OutputReportByteLength));
        return *output_;
    }
    static bool same_for_dump(const HidPadReading &a, const HidPadReading &b) noexcept {
        const auto close = [](float p, float q) { return p - q < 0.125f && q - p < 0.125f; };
        return a.buttons == b.buttons && a.hat == b.hat && close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z) && close(a.rz, b.rz) && close(a.rx, b.rx) && close(a.ry, b.ry);
    }
    // The product string, read through a handle that asks for no access to the device's reports.
    static std::string product_name(const std::wstring &path) {
        std::string name;
        const HANDLE device = CreateFileW(path.c_str(), 0u, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0u, nullptr);
        if (device == INVALID_HANDLE_VALUE) return name;
        wchar_t text[128]{};
        if (HidD_GetProductString(device, text, sizeof(text) - sizeof(wchar_t)))
            for (const wchar_t *c = text; *c; ++c) name.push_back(*c >= 32 && *c < 127 && *c != L'\'' ? static_cast<char>(*c) : '?');
        CloseHandle(device);
        return name;
    }
    void read_caps() {
        USHORT count = caps_.NumberInputButtonCaps;
        button_caps_.resize(count);
        if (!count || HidP_GetButtonCaps(HidP_Input, button_caps_.data(), &count, preparsed()) != HIDP_STATUS_SUCCESS) count = 0;
        button_caps_.resize(count);
        for (const auto &cap : button_caps_)
            if (cap.UsagePage == 9u) info_.buttons += cap.IsRange ? unsigned(cap.Range.UsageMax - cap.Range.UsageMin + 1) : 1u;
        count = caps_.NumberInputValueCaps;
        value_caps_.resize(count);
        if (!count || HidP_GetValueCaps(HidP_Input, value_caps_.data(), &count, preparsed()) != HIDP_STATUS_SUCCESS) count = 0;
        value_caps_.resize(count);
        // Generic Desktop: X 30, Y 31, Z 32, Rx 33, Ry 34, Rz 35, hat switch 39.
        static constexpr std::array<USAGE, 7> wanted{0x30u, 0x31u, 0x32u, 0x35u, 0x33u, 0x34u, 0x39u};
        for (const auto &cap : value_caps_) {
            if (cap.UsagePage != 1u) continue;
            const USAGE first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage, last = cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage;
            for (std::size_t i = 0; i < wanted.size(); ++i) {
                if (wanted[i] < first || wanted[i] > last || axes_[i].present) continue;
                auto &axis = axes_[i];
                axis.usage = wanted[i]; axis.bits = cap.BitSize; axis.low = cap.LogicalMin; axis.high = cap.LogicalMax; axis.present = true;
                // A description that gives the top of an unsigned range in one byte reads as -1.
                if (axis.high <= axis.low && axis.bits && axis.bits < 31u) { axis.low = 0; axis.high = (1l << axis.bits) - 1; }
                if (i < 6u) ++info_.axes; else info_.hat = true;
            }
        }
    }
    bool read_system(const std::uint8_t *report, std::size_t size, HidPadReading &out) const {
        if (size > 0xFFFFu) return false;
        auto *bytes = reinterpret_cast<PCHAR>(const_cast<std::uint8_t *>(report));
        const auto length = static_cast<ULONG>(size);
        std::array<USAGE, 64> down{};
        ULONG count = static_cast<ULONG>(down.size());
        // Fails for a report the device describes as vendor data only (the long Bluetooth reports).
        if (HidP_GetUsages(HidP_Input, 9u, 0u, down.data(), &count, preparsed(), bytes, length) != HIDP_STATUS_SUCCESS) return false;
        for (ULONG i = 0; i < count && i < down.size(); ++i)
            if (down[i] >= 1u && down[i] <= 32u) out.buttons |= 1u << (down[i] - 1u);
        const auto value = [&](const Axis &axis, long &result) {
            ULONG raw = 0u;
            if (!axis.present || HidP_GetUsageValue(HidP_Input, 1u, 0u, axis.usage, &raw, preparsed(), bytes, length) != HIDP_STATUS_SUCCESS) return false;
            result = static_cast<long>(raw);
            // A signed range: the field's top bit is the sign.
            if (axis.low < 0 && axis.bits && axis.bits < 31u && (raw >> (axis.bits - 1u) & 1u)) result = static_cast<long>(raw) - (1l << axis.bits);
            return true;
        };
        long read = 0;
        unsigned sticks = 0u;
        if (value(axes_[0], read)) { out.x = hid_stick_axis(read, axes_[0].low, axes_[0].high); ++sticks; }
        if (value(axes_[1], read)) { out.y = hid_stick_axis(read, axes_[1].low, axes_[1].high); ++sticks; }
        if (value(axes_[2], read)) out.z = hid_stick_axis(read, axes_[2].low, axes_[2].high);
        if (value(axes_[3], read)) out.rz = hid_stick_axis(read, axes_[3].low, axes_[3].high);
        bool left = false, right = false;
        if (value(axes_[4], read)) { out.rx = hid_trigger_axis(read, axes_[4].low, axes_[4].high); left = true; }
        if (value(axes_[5], read)) { out.ry = hid_trigger_axis(read, axes_[5].low, axes_[5].high); right = true; }
        out.triggers = left && right;
        // Outside its range the hat is released (a PlayStation pad sends 8 in a range of 0 to 7).
        if (value(axes_[6], read) && read >= axes_[6].low && read <= axes_[6].high && read - axes_[6].low < 8) out.hat = static_cast<std::uint8_t>(read - axes_[6].low);
        // A report with buttons but neither stick axis is not the pad's state report.
        return sticks == 2u;
    }
    HidPadInfo info_;
    std::wstring path_;
    std::vector<std::uint64_t> preparsed_;
    HIDP_CAPS caps_{};
    std::vector<HIDP_BUTTON_CAPS> button_caps_;
    std::vector<HIDP_VALUE_CAPS> value_caps_;
    std::array<Axis, 7> axes_{};
    Log log_;
    HidPadReading dump_last_{};
    std::unique_ptr<HidPadOutput> output_;
    std::uint64_t reports_{}, read_by_system_{}, read_by_layout_{}, unread_{}, disagreements_{};
    unsigned dumped_{};
};
}

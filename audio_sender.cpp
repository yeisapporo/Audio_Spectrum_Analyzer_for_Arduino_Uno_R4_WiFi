#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio_dsp.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uuid.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr const char *default_ip = "192.168.1.XXX";
constexpr int default_port = 5005;

volatile LONG stopping = 0;

BOOL WINAPI on_console(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        InterlockedExchange(&stopping, 1);
        return TRUE;
    }
    return FALSE;
}

bool stopped() {
    return InterlockedCompareExchange(&stopping, 0, 0) != 0;
}

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char text[256];
        std::snprintf(
            text, sizeof(text),
            "%s failed (HRESULT 0x%08lX)",
            operation,
            static_cast<unsigned long>(result)
        );
        throw std::runtime_error(text);
    }
}

struct ComRuntime {
    ComRuntime() {
        check(
            CoInitializeEx(nullptr, COINIT_MULTITHREADED),
            "CoInitializeEx"
        );
    }
    ~ComRuntime() {
        CoUninitialize();
    }
};

struct WinsockRuntime {
    WinsockRuntime() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            throw std::runtime_error(
                "WSAStartup failed: " + std::to_string(error)
            );
        }
    }
    ~WinsockRuntime() {
        WSACleanup();
    }
};

struct UdpSocket {
    SOCKET value = INVALID_SOCKET;

    UdpSocket() {
        value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (value == INVALID_SOCKET) {
            throw std::runtime_error(
                "socket failed: " + std::to_string(WSAGetLastError())
            );
        }
    }
    ~UdpSocket() {
        if (value != INVALID_SOCKET) closesocket(value);
    }
};

struct RunningAudio {
    IAudioClient* client;

    explicit RunningAudio(IAudioClient* c) : client(c) {
        check(client->Start(), "Start");
    }
    ~RunningAudio() {
        client->Stop();
    }
};

struct ConsoleHandler {
    ConsoleHandler() {
        if (!SetConsoleCtrlHandler(on_console, TRUE)) {
            throw std::runtime_error("SetConsoleCtrlHandler failed");
        }
    }
    ~ConsoleHandler() {
        SetConsoleCtrlHandler(on_console, FALSE);
    }
};

std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], size, nullptr, nullptr);
    result.pop_back();
    return result;
}
}

int main(int argc, char *argv[]) {
    try {
        if (argc > 3 ||
            (argc > 1 &&
             (std::strcmp(argv[1], "--help") == 0
             || std::strcmp(argv[1], "-h") == 0))) {
            std::cout
                << "Usage: audio_sender.exe [IPv4-address [port]]\n"
                << "Default: 192.168.1.19 5005\n";
            return argc > 3 ? 1 : 0;
        }

        const char *ip = argc > 1 ? argv[1] : default_ip;
        int port = default_port;

        if (argc > 2) {
            size_t used = 0;
            const std::string argument = argv[2];
            port = std::stoi(argument, &used);

            if (used != argument.size() || port < 1 || port > 65535) {
                throw std::runtime_error(
                    "Port must be an integer from 1 to 65535"
                );
            }
        }

        ComRuntime com;
        WinsockRuntime winsock;
        UdpSocket sock;

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<u_short>(port));

        if (InetPtonA(AF_INET, ip, &target.sin_addr) != 1) {
            throw std::runtime_error("Invalid IPv4 address");
        }

        // Windowsの既定再生デバイス
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(
            CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(enumerator.GetAddressOf())
            ),
            "Create device enumerator"
        );

        ComPtr<IMMDevice> device;
        check(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf()),
            "Get default playback device"
        );

        ComPtr<IPropertyStore> properties;
        check(
            device->OpenPropertyStore(
                STGM_READ, properties.GetAddressOf()
            ),
            "OpenPropertyStore"
        );

        PROPVARIANT name;
        PropVariantInit(&name);

        const HRESULT name_result =
            properties->GetValue(PKEY_Device_FriendlyName, &name);

        std::string source = "Default playback device";
        if (SUCCEEDED(name_result) &&
            name.vt == VT_LPWSTR && name.pwszVal) {
            source = utf8(name.pwszVal);
        }
        PropVariantClear(&name);

        SetConsoleOutputCP(CP_UTF8);
        std::cout
            << "Audio source: " << source << '\n'
            << "UDP target: " << ip << ':' << port << '\n';

        ComPtr<IAudioClient> client;
        check(
            device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())
            ),
            "Activate WASAPI"
        );

        // 48kHz、ステレオ、float32。
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = audio::input_rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 8;
        format.nAvgBytesPerSec =
            format.nSamplesPerSec * format.nBlockAlign;

        // デバイスのネイティブ形式との変換はWindows任せ
        const DWORD flags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        check(
            client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                flags,
                1000000,  // 100ms、単位100ns
                0,
                &format,
                nullptr
            ),
            "Initialize WASAPI loopback (48 kHz stereo float)"
        );

        ComPtr<IAudioCaptureClient> capture;
        check(
            client->GetService(
                IID_PPV_ARGS(capture.GetAddressOf())
            ),
            "Get capture service"
        );

        audio::Converter converter;
        std::vector<float> pending;
        pending.reserve(audio::input_frames * 4);

        ConsoleHandler handler;
        RunningAudio running(client.Get());

        std::uint64_t count = 0;
        auto last_report = std::chrono::steady_clock::now();

        std::cout
            << "Streaming started "
            << "(8000 Hz, mono, unsigned 8-bit, 512 bytes/packet)\n"
            << "Press Ctrl+C to stop.\n";

        while (!stopped()) {
            UINT32 available = 0;
            check(
                capture->GetNextPacketSize(&available),
                "GetNextPacketSize"
            );

            if (available == 0) {
                Sleep(2);
                continue;
            }

            while (available != 0 && !stopped()) {
                // GetBuffer中に再確保しないように先に確保
                pending.reserve(pending.size() + available);

                BYTE* bytes = nullptr;
                UINT32 frames = 0;
                DWORD buffer_flags = 0;

                check(
                    capture->GetBuffer(&bytes, &frames, &buffer_flags, nullptr, nullptr),
                    "GetBuffer"
                );

                const bool silent = ((buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);

                for (UINT32 f = 0; f < frames; ++f) {
                    float mono = 0;

                    if (!silent) {
                        float stereo[2];
                        std::memcpy(stereo, bytes + static_cast<size_t>(f) * 8, sizeof(stereo));
                        mono = (stereo[0] + stereo[1]) * 0.5f;
                        if (!std::isfinite(mono)) mono = 0;
                    }

                    pending.push_back(mono);
                }

                check(capture->ReleaseBuffer(frames), "ReleaseBuffer");

                if ((buffer_flags &
                     AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    std::cerr
                        << "Warning: audio capture discontinuity\n";
                }

                size_t consumed = 0;

                while (pending.size() - consumed >=
                       audio::input_frames && !stopped()) {
                    const float* mono = pending.data() + consumed;
                    const auto pcm = converter.convert(mono);

                    const int sent = sendto(
                        sock.value,
                        reinterpret_cast<const char*>(pcm.data()),
                        static_cast<int>(pcm.size()),
                        0,
                        reinterpret_cast<const sockaddr*>(&target),
                        sizeof(target)
                    );

                    if (sent == SOCKET_ERROR) {
                        throw std::runtime_error(
                            "sendto failed: " +
                            std::to_string(WSAGetLastError())
                        );
                    }

                    if (sent != static_cast<int>(pcm.size())) {
                        throw std::runtime_error(
                            "Incomplete UDP datagram"
                        );
                    }

                    ++count;

                    const auto now =
                        std::chrono::steady_clock::now();

                    if (now - last_report >= std::chrono::seconds(1)) {
                        float peak = 0;

                        for (int i = 0; i < audio::input_frames; ++i) {
                            peak = std::max(peak, std::abs(mono[i]));
                        }

                        std::printf(
                            "Packets: %llu, Peak: %.3f\n",
                            static_cast<unsigned long long>(count),
                            static_cast<double>(peak)
                        );

                        last_report = now;
                    }

                    consumed += audio::input_frames;
                }

                pending.erase(
                    pending.begin(),
                    pending.begin() + consumed
                );

                check(
                    capture->GetNextPacketSize(&available),
                    "GetNextPacketSize"
                );
            }
        }

        std::cout << "Streaming stopped\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

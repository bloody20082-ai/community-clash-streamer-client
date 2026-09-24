#include "loopback_audio.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

static float read_sample(const BYTE* base, UINT frame, UINT ch,
                         UINT channels, const WAVEFORMATEX* fmt) {
  const UINT index = frame * channels + ch;

  bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  bool isPcm = fmt->wFormatTag == WAVE_FORMAT_PCM;
  WORD bits = fmt->wBitsPerSample;

  if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    isFloat = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    isPcm = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    bits = ex->Format.wBitsPerSample;
  }

  if (isFloat && bits == 32) {
    return reinterpret_cast<const float*>(base)[index];
  }
  if (isPcm && bits == 16) {
    return reinterpret_cast<const std::int16_t*>(base)[index] / 32768.0f;
  }
  if (isPcm && bits == 32) {
    return reinterpret_cast<const std::int32_t*>(base)[index] / 2147483648.0f;
  }
  if (isPcm && bits == 24) {
    const BYTE* p = base + index * 3;
    std::int32_t v = (p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000) v |= ~0xFFFFFF;
    return v / 8388608.0f;
  }
  return 0.0f;
}

static std::int16_t to_i16(float x) {
  x = std::clamp(x, -1.0f, 1.0f);
  return static_cast<std::int16_t>(std::lrintf(x * 32767.0f));
}

LoopbackAudioCapture::~LoopbackAudioCapture() { stop(); }

void LoopbackAudioCapture::start(AudioCallback cb) {
  stop();
  running_ = true;
  blocks_ = 0;

  thread_ = std::thread([this, cb = std::move(cb)]() mutable {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* fmt = nullptr;
    HANDLE eventHandle = nullptr;

    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)))) goto done;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) goto done;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())))) goto done;
    if (FAILED(client->GetMixFormat(&fmt))) goto done;

    sample_rate_ = static_cast<int>(fmt->nSamplesPerSec);

    eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) goto done;

    if (FAILED(client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0, 0, fmt, nullptr))) goto done;

    if (FAILED(client->SetEventHandle(eventHandle))) goto done;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) goto done;
    if (FAILED(client->Start())) goto done;

    {
      const int rate = static_cast<int>(fmt->nSamplesPerSec);
      const UINT inChannels = fmt->nChannels;
      const int outChannels = 2;
      const size_t blockFrames = std::max<size_t>(1, rate / 100);
      std::vector<std::int16_t> pending;
      pending.reserve(blockFrames * outChannels * 4);

      while (running_) {
        DWORD wait = WaitForSingleObject(eventHandle, 100);
        if (wait != WAIT_OBJECT_0) continue;

        UINT packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;

        while (packet > 0) {
          BYTE* data = nullptr;
          UINT frames = 0;
          DWORD flags = 0;
          if (FAILED(capture->GetBuffer(
                  &data, &frames, &flags, nullptr, nullptr))) break;

          const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
          for (UINT f = 0; f < frames; ++f) {
            float l = 0.0f, r = 0.0f;
            if (!silent && data) {
              l = read_sample(data, f, 0, inChannels, fmt);
              r = inChannels > 1 ? read_sample(data, f, 1, inChannels, fmt) : l;
            }
            pending.push_back(to_i16(l));
            pending.push_back(to_i16(r));
          }

          capture->ReleaseBuffer(frames);

          const size_t blockSamples = blockFrames * outChannels;
          while (pending.size() >= blockSamples) {
            std::vector<std::int16_t> block(
                pending.begin(), pending.begin() + blockSamples);
            pending.erase(pending.begin(), pending.begin() + blockSamples);
            cb(rate, outChannels, std::move(block));
            ++blocks_;
          }

          if (FAILED(capture->GetNextPacketSize(&packet))) {
            packet = 0;
          }
        }
      }
      client->Stop();
    }

done:
    if (fmt) CoTaskMemFree(fmt);
    if (eventHandle) CloseHandle(eventHandle);
    CoUninitialize();
  });
}

void LoopbackAudioCapture::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

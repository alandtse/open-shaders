#include "GazeProvider.h"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

namespace
{
	constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t(0);

	// IVRSystem vtable slots: GetProjectionRaw has been slot 2 since IVRSystem_002,
	// and GetEyeTrackedFoveationCenter is slot 35 in IVRSystem_026 (OpenVR 2.15).
	constexpr int kSlotGetProjectionRaw = 2;
	constexpr int kSlotGetEyeTrackedFoveationCenter = 35;

	// Winsock constants and layouts, used through ws2_32.dll entry points so no
	// winsock header has to be ordered against the plugin PCH.
	constexpr std::uint16_t kWinsockVersion = 0x0202;
	constexpr int kAddressFamilyInet = 2;
	constexpr int kSocketDatagram = 2;
	constexpr int kProtocolUdp = 17;
	constexpr int kSocketLevel = 0xffff;     // SOL_SOCKET
	constexpr int kReuseAddress = 0x0004;    // SO_REUSEADDR
	constexpr int kReceiveTimeout = 0x1006;  // SO_RCVTIMEO
	constexpr int kReceiveTimeoutMs = 500;
	constexpr int kNoSocketFlags = 0;

	struct HmdVector2
	{
		float v[2];
	};

	struct SockAddrIn
	{
		std::int16_t family;
		std::uint16_t port;
		std::uint32_t address;
		char zero[8];
	};

	struct WinsockApi
	{
		using WsaStartupFn = int(__stdcall*)(std::uint16_t, void*);
		using SocketFn = std::uintptr_t(__stdcall*)(int, int, int);
		using BindFn = int(__stdcall*)(std::uintptr_t, const void*, int);
		using RecvFromFn = int(__stdcall*)(std::uintptr_t, char*, int, int, void*, int*);
		using CloseSocketFn = int(__stdcall*)(std::uintptr_t);
		using SetSockOptFn = int(__stdcall*)(std::uintptr_t, int, int, const char*, int);

		bool ready = false;
		WsaStartupFn WsaStartup = nullptr;
		SocketFn Socket = nullptr;
		BindFn Bind = nullptr;
		RecvFromFn RecvFrom = nullptr;
		CloseSocketFn CloseSocket = nullptr;
		SetSockOptFn SetSockOpt = nullptr;

		bool Load()
		{
			const HMODULE module = LoadLibraryW(L"ws2_32.dll");
			if (!module)
				return false;
			WsaStartup = reinterpret_cast<WsaStartupFn>(reinterpret_cast<void*>(GetProcAddress(module, "WSAStartup")));
			Socket = reinterpret_cast<SocketFn>(reinterpret_cast<void*>(GetProcAddress(module, "socket")));
			Bind = reinterpret_cast<BindFn>(reinterpret_cast<void*>(GetProcAddress(module, "bind")));
			RecvFrom = reinterpret_cast<RecvFromFn>(reinterpret_cast<void*>(GetProcAddress(module, "recvfrom")));
			CloseSocket = reinterpret_cast<CloseSocketFn>(reinterpret_cast<void*>(GetProcAddress(module, "closesocket")));
			SetSockOpt = reinterpret_cast<SetSockOptFn>(reinterpret_cast<void*>(GetProcAddress(module, "setsockopt")));
			if (!WsaStartup || !Socket || !Bind || !RecvFrom || !CloseSocket || !SetSockOpt)
				return false;
			alignas(8) char startupData[1024]{};
			ready = WsaStartup(kWinsockVersion, startupData) == 0;
			return ready;
		}

		bool IsLoaded() const { return ready; }
	};

	WinsockApi& Winsock()
	{
		static WinsockApi api;
		static std::once_flag once;
		std::call_once(once, [] { api.Load(); });
		return api;
	}

	std::int64_t NowMilliseconds()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	std::uint16_t NetworkByteOrderPort(int a_port)
	{
		return static_cast<std::uint16_t>(((a_port & 0xff) << 8) | ((a_port >> 8) & 0xff));
	}

	using GetGenericInterfaceFn = void*(__cdecl*)(const char*, int*);
	using GetProjectionRawFn = void(__cdecl*)(void*, int, float*, float*, float*, float*);
	using GetEyeTrackedFoveationCenterFn = bool(__cdecl*)(void*, HmdVector2*, HmdVector2*);
}

namespace Util::Gaze
{
	Provider::~Provider()
	{
		Shutdown();
	}

	void Provider::Configure(bool a_allowSteamVr, bool a_allowOsc, int a_port, float a_offsetX, float a_offsetY)
	{
		allowSteamVr = a_allowSteamVr;
		allowOsc = a_allowOsc;
		offsetX = std::clamp(a_offsetX, -1.0f, 1.0f);
		offsetY = std::clamp(a_offsetY, -1.0f, 1.0f);

		const int port = std::clamp(a_port, 1, 65535);
		if (port != oscPort) {
			StopOsc();
			oscPort = port;
			oscBindFailed = false;
		}
		if (!allowOsc)
			StopOsc();
	}

	Sample Provider::Poll()
	{
		Sample sample;
		EnsureOpenVr();
		if (allowSteamVr)
			sample = PollSteamVr();
		if (!sample.valid && allowOsc) {
			StartOsc();
			sample = PollOsc();
		}

		std::string lastAddress;
		{
			std::lock_guard lock(oscMutex);
			lastAddress = lastOscAddress;
		}
		const std::uint64_t frames = sample.valid ? validFrames.fetch_add(1) + 1 : validFrames.load();

		{
			std::lock_guard lock(statusMutex);
			status.source = sample.source;
			status.valid = sample.valid;
			status.centre = sample.centre;
			status.validFrames = frames;
			status.oscPackets = oscPackets.load();
			status.oscListening = oscListening.load();
			status.oscBindFailed = oscBindFailed.load();
			status.oscPort = oscPort;
			status.lastOscAddress = std::move(lastAddress);
		}
		return sample;
	}

	Status Provider::GetStatus() const
	{
		std::lock_guard lock(statusMutex);
		return status;
	}

	void Provider::Shutdown()
	{
		StopOsc();
	}

	void Provider::EnsureOpenVr()
	{
		if (steamVrSystem)
			return;
		if (steamVrRetryFrames > 0) {
			--steamVrRetryFrames;
			return;
		}
		if (!TryAcquireOpenVr())
			steamVrRetryFrames = kSteamVrRetryFrames;
	}

	bool Provider::TryAcquireOpenVr()
	{
		const HMODULE module = GetModuleHandleW(L"openvr_api.dll");
		if (!module)
			return false;
		const auto getInterface = reinterpret_cast<GetGenericInterfaceFn>(reinterpret_cast<void*>(GetProcAddress(module, "VR_GetGenericInterface")));
		if (!getInterface)
			return false;

		// Only IVRSystem_026 has the foveation centre; every version has GetProjectionRaw at slot 2,
		// which the OSC projection needs on the older runtimes Skyrim VR ships.
		for (const char* const version : { "IVRSystem_026", "IVRSystem_022", "IVRSystem_021", "IVRSystem_020", "IVRSystem_019" }) {
			int error = 0;
			void* system = getInterface(version, &error);
			if (!system || error != 0)
				continue;
			steamVrSystem = system;
			steamVrHasFoveation = std::string_view(version) == "IVRSystem_026";
			LoadProjection();
			logger::info("[Gaze] {} acquired; eye-tracked foveation centre {}", version, steamVrHasFoveation ? "available" : "unavailable");
			return true;
		}
		return false;
	}

	void Provider::LoadProjection()
	{
		const auto vtable = *reinterpret_cast<void***>(steamVrSystem);
		if (!vtable)
			return;
		const auto getProjectionRaw = reinterpret_cast<GetProjectionRawFn>(vtable[kSlotGetProjectionRaw]);
		if (!getProjectionRaw)
			return;
		for (int eye = 0; eye < 2; ++eye) {
			ProjectionRaw raw;
			getProjectionRaw(steamVrSystem, eye, &raw.left, &raw.right, &raw.top, &raw.bottom);
			projection[eye] = raw;
		}
	}

	Sample Provider::PollSteamVr()
	{
		Sample sample;
		if (!steamVrSystem || !steamVrHasFoveation)
			return sample;

		const auto vtable = *reinterpret_cast<void***>(steamVrSystem);
		if (!vtable)
			return sample;
		const auto getCentre = reinterpret_cast<GetEyeTrackedFoveationCenterFn>(vtable[kSlotGetEyeTrackedFoveationCenter]);
		if (!getCentre)
			return sample;

		HmdVector2 leftNdc{};
		HmdVector2 rightNdc{};
		if (!getCentre(steamVrSystem, &leftNdc, &rightNdc))
			return sample;
		if (!std::isfinite(leftNdc.v[0]) || !std::isfinite(leftNdc.v[1]) ||
			!std::isfinite(rightNdc.v[0]) || !std::isfinite(rightNdc.v[1])) {
			return sample;
		}

		sample.centre[0] = NdcToEyeUV({ leftNdc.v[0], leftNdc.v[1] });
		sample.centre[1] = NdcToEyeUV({ rightNdc.v[0], rightNdc.v[1] });
		sample.source = Source::kSteamVr;
		sample.valid = true;
		return sample;
	}

	Sample Provider::PollOsc()
	{
		Sample sample;
		std::array<Direction, 2> directions{};
		bool fresh = false;
		{
			std::lock_guard lock(oscMutex);
			fresh = oscHasGaze && NowMilliseconds() - oscTimestamp <= kOscStaleMilliseconds;
			if (fresh)
				directions = oscDirections;
		}
		if (!fresh)
			return sample;

		Vec2 leftUV;
		Vec2 rightUV;
		if (!ProjectDirection(0, directions[0], projection, leftUV) ||
			!ProjectDirection(1, directions[1], projection, rightUV)) {
			return sample;
		}

		sample.centre[0] = { std::clamp(leftUV.x + offsetX, 0.0f, 1.0f), std::clamp(leftUV.y + offsetY, 0.0f, 1.0f) };
		sample.centre[1] = { std::clamp(rightUV.x + offsetX, 0.0f, 1.0f), std::clamp(rightUV.y + offsetY, 0.0f, 1.0f) };
		sample.source = Source::kOsc;
		sample.valid = true;
		return sample;
	}

	void Provider::StartOsc()
	{
		std::lock_guard lock(lifecycleMutex);
		if (oscThread.joinable() || oscBindFailed.load())
			return;

		WinsockApi& winsock = Winsock();
		if (!winsock.IsLoaded()) {
			logger::warn("[Gaze] Could not load Winsock; OSC gaze disabled");
			oscBindFailed = true;
			return;
		}

		const std::uintptr_t handle = winsock.Socket(kAddressFamilyInet, kSocketDatagram, kProtocolUdp);
		if (handle == kInvalidSocket) {
			logger::warn("[Gaze] OSC socket() failed; OSC gaze disabled");
			oscBindFailed = true;
			return;
		}

		int reuse = 1;
		winsock.SetSockOpt(handle, kSocketLevel, kReuseAddress, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
		int timeout = kReceiveTimeoutMs;
		winsock.SetSockOpt(handle, kSocketLevel, kReceiveTimeout, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

		SockAddrIn address{};
		address.family = static_cast<std::int16_t>(kAddressFamilyInet);
		address.port = NetworkByteOrderPort(oscPort);
		address.address = 0;
		if (winsock.Bind(handle, &address, sizeof(address)) != 0) {
			logger::warn("[Gaze] OSC bind to UDP port {} failed; is another app using it? OSC gaze disabled", oscPort);
			winsock.CloseSocket(handle);
			oscBindFailed = true;
			return;
		}

		oscSocket = handle;
		oscRunning = true;
		oscListening = true;
		oscThread = std::thread([this, handle] { OscThread(handle); });
		logger::info("[Gaze] Listening for OSC eye tracking on UDP port {}", oscPort);
	}

	void Provider::StopOsc()
	{
		std::lock_guard lock(lifecycleMutex);
		if (!oscRunning.exchange(false))
			return;

		WinsockApi& winsock = Winsock();
		if (winsock.IsLoaded() && oscSocket != kInvalidSocket)
			winsock.CloseSocket(oscSocket);
		oscSocket = kInvalidSocket;
		oscListening = false;
		if (oscThread.joinable())
			oscThread.join();
	}

	void Provider::OscThread(std::uintptr_t a_socket)
	{
		std::array<std::uint8_t, 65536> buffer{};
		WinsockApi& winsock = Winsock();
		while (oscRunning.load(std::memory_order_relaxed)) {
			SockAddrIn from{};
			int fromLength = sizeof(from);
			const int received = winsock.RecvFrom(a_socket, reinterpret_cast<char*>(buffer.data()),
				static_cast<int>(buffer.size()), kNoSocketFlags, &from, &fromLength);
			if (received <= 0)
				continue;

			oscPackets.fetch_add(1, std::memory_order_relaxed);
			std::array<OscMessage, kMaxMessagesPerDatagram> messages;
			const std::size_t count = ParseOscDatagram(buffer.data(), static_cast<std::size_t>(received), messages);
			for (std::size_t i = 0; i < count; ++i) {
				std::lock_guard lock(oscMutex);
				lastOscAddress.assign(messages[i].address);
				if (messages[i].hasGaze) {
					oscDirections = { messages[i].left, messages[i].right };
					oscHasGaze = true;
					oscTimestamp = NowMilliseconds();
				}
			}
		}
		oscListening = false;
	}
}

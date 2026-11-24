#include "CoopSocket.h"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <winsock2.h>
#	include <ws2tcpip.h>
	using socklen_t = int;
	static int  LastError()       { return WSAGetLastError(); }
	static bool WouldBlock(int e) { return e == WSAEWOULDBLOCK; }
	static bool InProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
	static void CloseRaw(SOCKET s) { closesocket(s); }
	static bool SetNonBlocking(SOCKET s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
	static void StartUp()
	{
		static bool done = false;
		if (done) return;
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
		done = true;
	}
#else
#	include <cerrno>
#	include <fcntl.h>
#	include <netdb.h>
#	include <netinet/in.h>
#	include <netinet/tcp.h>
#	include <sys/select.h>
#	include <sys/socket.h>
#	include <unistd.h>
	using SOCKET = int;
	static constexpr SOCKET INVALID_SOCKET = -1;
	static int  LastError()       { return errno; }
	static bool WouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
	static bool InProgress(int e) { return e == EINPROGRESS; }
	static void CloseRaw(SOCKET s) { close(s); }
	static bool SetNonBlocking(SOCKET s)
	{
		int const flags = fcntl(s, F_GETFL, 0);
		return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
	}
	static void StartUp() {}
#endif

#include <cstring>


namespace CoopSocket
{

Handle Connect(std::string const& host, uint16_t const port, int const timeout_ms, std::string& err)
{
	StartUp();

	addrinfo hints{};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* found = nullptr;
	std::string const service = std::to_string(port);
	if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || !found)
	{
		err = "cannot find " + host;
		return INVALID;
	}

	err = "cannot reach " + host + ":" + service;
	Handle result = INVALID;
	for (addrinfo* a = found; a && result == INVALID; a = a->ai_next)
	{
		SOCKET const s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (s == INVALID_SOCKET) continue;
		if (!SetNonBlocking(s)) { CloseRaw(s); continue; }

		if (connect(s, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) != 0)
		{
			if (!InProgress(LastError())) { CloseRaw(s); continue; }

			fd_set w, e;
			FD_ZERO(&w); FD_SET(s, &w);
			FD_ZERO(&e); FD_SET(s, &e);
			timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
			if (select(static_cast<int>(s) + 1, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(s, &e))
			{
				CloseRaw(s);
				continue;
			}
			int so_error = 0;
			socklen_t len = sizeof(so_error);
			getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
			if (so_error != 0) { CloseRaw(s); continue; }
		}

		int one = 1; // small messages (commands) should not wait
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));
		result = static_cast<Handle>(s);
		err.clear();
	}
	freeaddrinfo(found);
	return result;
}


long Send(Handle const h, uint8_t const* const data, size_t const len)
{
	if (len == 0) return 0;
	int const chunk = len > (1 << 20) ? (1 << 20) : static_cast<int>(len);
	auto const n = send(static_cast<SOCKET>(h), reinterpret_cast<char const*>(data), chunk, 0);
	if (n >= 0) return static_cast<long>(n);
	return WouldBlock(LastError()) ? 0 : -1;
}


long Recv(Handle const h, uint8_t* const data, size_t const len)
{
	int const chunk = len > (1 << 20) ? (1 << 20) : static_cast<int>(len);
	auto const n = recv(static_cast<SOCKET>(h), reinterpret_cast<char*>(data), chunk, 0);
	if (n > 0) return static_cast<long>(n);
	if (n == 0) return -1; // closed
	return WouldBlock(LastError()) ? 0 : -1;
}


void Close(Handle const h)
{
	if (h != INVALID) CloseRaw(static_cast<SOCKET>(h));
}

}

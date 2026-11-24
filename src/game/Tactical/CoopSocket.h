#pragma once

// Bare TCP for co-op. Kept apart from the game headers: the Windows socket
// headers clash with the game's own type names.

#include <cstddef>
#include <cstdint>
#include <string>

namespace CoopSocket
{
	using Handle = intptr_t;
	constexpr Handle INVALID = -1;

	// Connect, waiting at most timeout_ms; the socket is non-blocking afterwards.
	// Returns INVALID and fills err on failure.
	Handle Connect(std::string const& host, uint16_t port, int timeout_ms, std::string& err);

	// Both return the number of bytes moved, 0 when the socket would block, and
	// -1 when the connection failed or (Recv) the other side closed it.
	long Send(Handle, uint8_t const* data, size_t len);
	long Recv(Handle, uint8_t* data, size_t len);

	void Close(Handle);
}

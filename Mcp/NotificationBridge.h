//MesenMCP - notification bridge
//
//Forwards emulator notifications (breakpoint hits, etc.) from the emulation
//thread to the MCP server thread. Break events are copied and stored so tools
//can poll/wait for them (wait_for_breakpoint) without relying on push
//notifications, which many MCP clients don't surface well.
#pragma once
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Debugger/DebugTypes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

class NotificationBridge : public INotificationListener, public std::enable_shared_from_this<NotificationBridge>
{
public:
	struct BreakInfo
	{
		bool Valid = false;
		uint64_t Sequence = 0;      //incremented on every break event
		int32_t BreakpointId = -1;
		CpuType SourceCpu = CpuType::Nes;
		BreakSource Source = BreakSource::Unspecified;
		uint32_t FrameCount = 0;
	};

	void ProcessNotification(ConsoleNotificationType type, void* parameter) override
	{
		switch(type) {
			case ConsoleNotificationType::CodeBreak: {
				BreakEvent* evt = (BreakEvent*)parameter;
				std::lock_guard<std::mutex> lock(_mutex);
				_break.Valid = true;
				_break.Sequence = ++_sequence;
				_break.BreakpointId = evt->BreakpointId;
				_break.SourceCpu = evt->SourceCpu;
				_break.Source = evt->Source;
				_cond.notify_all();
				break;
			}
			case ConsoleNotificationType::DebuggerResumed: {
				std::lock_guard<std::mutex> lock(_mutex);
				_break.Valid = false;
				break;
			}
			default:
				break;
		}
	}

	//Waits until a break event with a sequence > afterSequence occurs, or timeout.
	//Returns the break info (Valid=false on timeout).
	BreakInfo WaitForBreak(uint64_t afterSequence, uint32_t timeoutMs)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while(_sequence <= afterSequence) {
			if(_cond.wait_until(lock, deadline) == std::cv_status::timeout) {
				break;
			}
		}
		return _break;
	}

	BreakInfo GetLastBreak()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _break;
	}

	uint64_t CurrentSequence()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _sequence;
	}

private:
	std::mutex _mutex;
	std::condition_variable _cond;
	uint64_t _sequence = 0;
	BreakInfo _break;
};

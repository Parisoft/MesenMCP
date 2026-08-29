//MesenMCP - virtual controller input provider
//
//Injects controller button state from MCP tool calls into the emulated
//consoles (the same IInputProvider mechanism movies use - see MesenMovie).
//UpdateInputState() calls providers after the (headless: empty) key mappings,
//so whatever we set here IS the controller state for that poll.
#pragma once
#include "Core/Shared/Interfaces/IInputProvider.h"
#include "Core/Shared/BaseControlDevice.h"
#include "Core/Shared/BaseControlManager.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/SettingTypes.h"
#include "Utilities/SimpleLock.h"

#include <array>
#include <map>
#include <string>
#include <vector>

class VirtualInputProvider : public IInputProvider
{
public:
	static constexpr uint32_t MaxPort = 4; //controller ports 1-4

	struct PortState
	{
		bool Active = false;
		uint16_t ButtonMask = 0;     //bit per controller button (order per console)
		int32_t HoldFramesLeft = -1; //-1 = hold until changed; >=0 = auto-release countdown
	};

	VirtualInputProvider(Emulator* emu) : _emu(emu)
	{
		_emu->RegisterInputProvider(this);
	}

	~VirtualInputProvider()
	{
		_emu->UnregisterInputProvider(this);
	}

	//Called on the emulation thread for every control device on every poll.
	//Returns true when this provider owns the device (known controller type on
	//an active port), which stops other providers (e.g. movies) from
	//overriding the injected state.
	bool SetInput(BaseControlDevice* device) override
	{
		ControllerType type = device->GetControllerType();
		const ButtonMap* map = GetButtonMap(type);
		if(!map) {
			return false;
		}

		uint8_t port = device->GetPort(); //0-based
		if(port >= MaxPort) {
			return false;
		}

		auto lock = _lock.AcquireSafe();
		PortState& state = _ports[port];
		if(!state.Active) {
			return false;
		}

		device->ClearState();
		for(uint8_t bit = 0; bit < map->Count; bit++) {
			if(state.ButtonMask & (1 << bit)) {
				device->SetBitValue(bit, true);
			}
		}

		//Count down hold frames once per poll of this port
		if(state.HoldFramesLeft > 0) {
			state.HoldFramesLeft--;
			if(state.HoldFramesLeft == 0) {
				state.ButtonMask = 0;
				state.HoldFramesLeft = -1;
			}
		}
		return true;
	}

	//--- MCP-side control ---

	//Buttons by name; returns false + error for unknown names
	bool SetButtons(uint32_t port, const std::vector<std::string>& buttons, int32_t holdFrames, std::string& error)
	{
		if(port < 1 || port > MaxPort) {
			error = "port must be 1 to " + std::to_string(MaxPort);
			return false;
		}

		ControllerType type = GetControllerTypeForPort(port);
		const ButtonMap* map = GetButtonMap(type);
		if(!map) {
			error = "port " + std::to_string(port) + " has no known controller (type "
				+ std::to_string((int)type) + ")";
			return false;
		}

		uint16_t mask = 0;
		for(const std::string& name : buttons) {
			auto it = map->Names.find(name);
			if(it == map->Names.end()) {
				error = "unknown button '" + name + "' for this controller. Valid buttons: " + map->ButtonList;
				return false;
			}
			mask |= (1 << it->second);
		}

		auto lock = _lock.AcquireSafe();
		PortState& state = _ports[port - 1];
		state.Active = true;
		state.ButtonMask = mask;
		state.HoldFramesLeft = holdFrames < 0 ? -1 : holdFrames;
		return true;
	}

	void ReleasePort(uint32_t port)
	{
		if(port < 1 || port > MaxPort) {
			return;
		}
		auto lock = _lock.AcquireSafe();
		_ports[port - 1].Active = false;
		_ports[port - 1].ButtonMask = 0;
		_ports[port - 1].HoldFramesLeft = -1;
	}

	bool IsPortActive(uint32_t port)
	{
		if(port < 1 || port > MaxPort) {
			return false;
		}
		auto lock = _lock.AcquireSafe();
		return _ports[port - 1].Active;
	}

	uint16_t GetPortMask(uint32_t port)
	{
		if(port < 1 || port > MaxPort) {
			return 0;
		}
		auto lock = _lock.AcquireSafe();
		return _ports[port - 1].ButtonMask;
	}

private:
	struct ButtonMap
	{
		std::map<std::string, uint8_t> Names;
		uint8_t Count = 0;
		std::string ButtonList;
	};

	ControllerType GetControllerTypeForPort(uint32_t port)
	{
		//Ask the console what's plugged into the port
		shared_ptr<IConsole> console = _emu->GetConsole();
		if(!console || !console->GetControlManager()) {
			return ControllerType::None;
		}
		shared_ptr<BaseControlDevice> device = console->GetControlManager()->GetControlDevice((uint8_t)(port - 1));
		return device ? device->GetControllerType() : ControllerType::None;
	}

	static const ButtonMap* GetButtonMap(ControllerType type)
	{
		static const ButtonMap nes = {
			{ {"up",0},{"down",1},{"left",2},{"right",3},{"start",4},{"select",5},{"b",6},{"a",7} },
			8,
			"up, down, left, right, start, select, b, a"
		};
		static const ButtonMap snes = {
			{ {"a",0},{"b",1},{"x",2},{"y",3},{"l",4},{"r",5},{"select",6},{"start",7},
			  {"up",8},{"down",9},{"left",10},{"right",11} },
			12,
			"a, b, x, y, l, r, select, start, up, down, left, right"
		};
		static const ButtonMap gb = {
			{ {"up",0},{"down",1},{"left",2},{"right",3},{"start",4},{"select",5},{"b",6},{"a",7} },
			8,
			"up, down, left, right, start, select, b, a"
		};
		static const ButtonMap gba = {
			{ {"up",0},{"down",1},{"left",2},{"right",3},{"start",4},{"select",5},{"b",6},{"a",7},{"l",8},{"r",9} },
			10,
			"up, down, left, right, start, select, b, a, l, r"
		};

		switch(type) {
			case ControllerType::NesController:
			case ControllerType::FamicomController:
			case ControllerType::FamicomControllerP2:
				return &nes;
			case ControllerType::SnesController:
				return &snes;
			case ControllerType::GameboyController:
				return &gb;
			case ControllerType::GbaController:
				return &gba;
			default:
				return nullptr;
		}
	}

	Emulator* _emu;
	SimpleLock _lock;
	std::array<PortState, MaxPort> _ports;
};

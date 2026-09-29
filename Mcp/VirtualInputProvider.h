//MesenMCP - virtual controller input provider
//
//Injects controller button state from MCP tool calls into the emulated
//consoles (the same IInputProvider mechanism movies use - see MesenMovie).
//UpdateInputState() calls providers after the (headless: empty) key mappings,
//so whatever we set here IS the controller state for that poll.
//
//Registration is NOT a one-shot affair: the core throws the console (and its
//control manager, which owns the provider list) away and builds a new one on
//every ROM load / power cycle / reload, so SyncRegistration() has to be called
//again after each of those. See SyncRegistration() below.
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
		int32_t HoldFramesLeft = -1; //-1 = hold until changed; 0 = expired; >0 = polls left
	};

	VirtualInputProvider(Emulator* emu) : _emu(emu)
	{
		SyncRegistration();
	}

	~VirtualInputProvider()
	{
		//Unregister from the console we are attached to - if it still exists.
		//EmuSession stops the emulator (which destroys the console) before this
		//runs, so the weak_ptr is usually empty here.
		std::shared_ptr<IConsole> console = _registeredConsole.lock();
		if(console && console->GetControlManager()) {
			console->GetControlManager()->UnregisterInputProvider(this);
		}
	}

	//Makes sure this provider is on the input provider list of the console that
	//is CURRENTLY loaded.
	//
	//Why this has to be called repeatedly: Emulator::RegisterInputProvider()
	//forwards to the current console's control manager and silently does
	//nothing when no console exists yet. Emulator::InternalLoadRom() then does
	//`_console.reset(newConsole)` - a brand-new control manager with an EMPTY
	//provider list - for every ROM load, power cycle (ReloadRom) and reload.
	//A provider registered once in the constructor is therefore registered on
	//nothing at all, and every set_controller call is silently ignored by the
	//emulation thread (the tool still returns success).
	//
	//Cheap and idempotent: a no-op while the live console is the one we are
	//already attached to.
	void SyncRegistration()
	{
		std::shared_ptr<IConsole> console = _emu->GetConsole();
		if(!console || !console->GetControlManager()) {
			_registeredConsole.reset();
			return;
		}
		if(console == _registeredConsole.lock()) {
			return; //already attached to the live console
		}
		console->GetControlManager()->RegisterInputProvider(this);
		_registeredConsole = console;
	}

	bool IsRegistered()
	{
		std::shared_ptr<IConsole> console = _emu->GetConsole();
		return console && console == _registeredConsole.lock();
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

		if(state.HoldFramesLeft == 0) {
			//Hold window used up - release. (hold_frames=0 starts here, so it
			//presses nothing at all, as documented.)
			state.ButtonMask = 0;
			state.HoldFramesLeft = -1;
		}

		device->ClearState();
		for(uint8_t bit = 0; bit < map->Count; bit++) {
			if(state.ButtonMask & (1 << bit)) {
				device->SetBitValue(bit, true);
			}
		}

		//Count the hold window down once per poll of this port - the consoles
		//poll their control devices exactly once per frame.
		if(state.HoldFramesLeft > 0) {
			state.HoldFramesLeft--;
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

		//Attach to the live console before doing anything else - the buttons are
		//only seen by the game if this provider is on that console's list.
		SyncRegistration();

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

	//Console this provider is currently registered with. The core replaces the
	//console (and its control manager, which owns the provider list) on every
	//ROM load / power cycle, so this is compared against the live console by
	//SyncRegistration() and used to unregister in the destructor. Weak: never
	//keeps a console alive.
	std::weak_ptr<IConsole> _registeredConsole;
};

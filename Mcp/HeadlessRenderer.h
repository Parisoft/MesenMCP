//MesenMCP - headless rendering device
//
//Minimal IRenderingDevice implementation that keeps a copy of the most recent
//frame in memory instead of drawing it to a window. This is what allows the
//emulation core to run without SDL/X11 (or any display server): the frame is
//simply stored under a lock and can be retrieved at any time, e.g. to encode
//a screenshot (PNGHelper) or, later, to produce MCP image content.
#pragma once
#include "Core/Shared/Interfaces/IRenderingDevice.h"
#include "Core/Shared/RenderedFrame.h"
#include "Utilities/SimpleLock.h"

#include <atomic>
#include <vector>

class Emulator;

class HeadlessRenderer : public IRenderingDevice
{
private:
	Emulator* _emu = nullptr;

	SimpleLock _lock;
	std::vector<uint32_t> _frameBuffer;
	uint32_t _width = 0;
	uint32_t _height = 0;
	uint32_t _frameNumber = 0;
	std::atomic<bool> _hasFrame { false };

public:
	//Diagnostics: number of frames received from the video renderer
	std::atomic<uint32_t> _updateFrameCount { 0 };

	HeadlessRenderer(Emulator* emu);
	virtual ~HeadlessRenderer();

	//IRenderingDevice implementation
	void UpdateFrame(RenderedFrame& frame) override;
	void ClearFrame() override;
	void Render(RenderSurfaceInfo& emuHud, RenderSurfaceInfo& scriptHud) override;
	void Reset() override;
	void SetFullscreenMode(FullscreenSettings settings) override;

	//Returns a copy of the most recent frame (0xAARRGGBB pixels, row-major).
	//Returns false when no frame has been produced yet.
	bool GetLastFrame(std::vector<uint32_t>& output, uint32_t& width, uint32_t& height, uint32_t& frameNumber);
};

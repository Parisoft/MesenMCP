//MesenMCP - headless rendering device
#include "Mcp/HeadlessRenderer.h"

#include "Core/Shared/Emulator.h"
#include "Core/Shared/Video/VideoRenderer.h"

HeadlessRenderer::HeadlessRenderer(Emulator* emu) : _emu(emu)
{
	//Same self-registration pattern used by SdlRenderer/SoftwareRenderer
	_emu->GetVideoRenderer()->RegisterRenderingDevice(this);
}

HeadlessRenderer::~HeadlessRenderer()
{
	_emu->GetVideoRenderer()->UnregisterRenderingDevice(this);
}

void HeadlessRenderer::UpdateFrame(RenderedFrame& frame)
{
	size_t size = (size_t)frame.Width * frame.Height;

	auto lock = _lock.AcquireSafe();
	_frameBuffer.resize(size);
	if(frame.FrameBuffer) {
		memcpy(_frameBuffer.data(), frame.FrameBuffer, size * sizeof(uint32_t));
	} else {
		memset(_frameBuffer.data(), 0, size * sizeof(uint32_t));
	}
	_width = frame.Width;
	_height = frame.Height;
	_frameNumber = frame.FrameNumber;
	_hasFrame = true;

	_updateFrameCount++;
}

void HeadlessRenderer::ClearFrame()
{
	auto lock = _lock.AcquireSafe();
	_hasFrame = false;
}

void HeadlessRenderer::Render(RenderSurfaceInfo& emuHud, RenderSurfaceInfo& scriptHud)
{
	//Nothing to do - there is no screen. (Called ~30fps by the VideoRenderer thread;
	//HUD surfaces are composed by the core and simply ignored here.)
}

void HeadlessRenderer::Reset()
{
	ClearFrame();
}

void HeadlessRenderer::SetFullscreenMode(FullscreenSettings settings)
{
	//No-op in headless mode
}

bool HeadlessRenderer::GetLastFrame(std::vector<uint32_t>& output, uint32_t& width, uint32_t& height, uint32_t& frameNumber)
{
	if(!_hasFrame.load()) {
		return false;
	}

	auto lock = _lock.AcquireSafe();
	output = _frameBuffer;
	width = _width;
	height = _height;
	frameNumber = _frameNumber;
	return true;
}

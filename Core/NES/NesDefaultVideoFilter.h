#pragma once

#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "NES/NesTypes.h"

class NesConsole;

class NesDefaultVideoFilter : public BaseVideoFilter
{
private:
	uint32_t _calculatedPalette[512] = {};
	VideoConfig _videoConfig = {};
	NesConfig _nesConfig = {};
	PpuModel _ppuModel = PpuModel::Ppu2C02;

	void InitLookupTable();

protected:
	void DecodePpuBuffer(uint16_t* ppuOutputBuffer, uint32_t* outputBuffer);
	void OnBeforeApplyFilter() override;

public:
	NesDefaultVideoFilter(Emulator* emu);

	static void ApplyPalBorder(uint16_t* ppuOutputBuffer);

	static void GenerateFullColorPalette(uint32_t paletteBuffer[512], PpuModel model);
	static void GetFullPalette(uint32_t palette[512], NesConfig& nesCfg, PpuModel model);

	//Fills the palette with Mesen's built-in default 2C02 colors (all 512 entries,
	//including emphasis). Used by headless front-ends to seed NesConfig::UserPalette -
	//the core expects the palette to be provided by the front-end (the GUI pushes it
	//through its config file on startup).
	static void GetBuiltInPalette(uint32_t palette[512]);

	static uint32_t GetDefaultPixelBrightness(uint16_t colorIndex, PpuModel model);

	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
};
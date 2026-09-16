#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace juce
{
	class File;
	class PropertiesFile;
}

namespace mdJucePlugin::sampleImport
{
	// Machinedrum UW RAM sample slots R01..R48
	constexpr uint32_t g_slotCount = 48;

	// The UW's sample memory: 2.5 MB of 16-bit words shared by all RAM slots.
	constexpr uint64_t g_sampleMemoryWords = 2500000 / 2;

	// The machine plays a sample at the rate in its SDS header, so audio keeps its
	// own rate. Only rates above this are resampled, to keep memory use sane.
	constexpr uint32_t g_maxSampleRate = 48000;
	constexpr uint32_t g_resampleTarget = 44100;

	struct DecodedSample
	{
		std::vector<int16_t> samples;	// mono
		uint32_t sampleRate = 0;
		std::string name;				// 4 characters for the machine display
		std::string sourcePath;
		bool clipped = false;
		double seconds() const { return sampleRate ? static_cast<double>(samples.size()) / sampleRate : 0.0; }
	};

	// true for extensions JUCE can decode (wav, aif, aiff, flac, mp3, m4a, ...)
	bool isAudioFile(const juce::File& _file);
	bool isSysexFile(const juce::File& _file);

	// Decodes, mixes to mono and converts to 16 bit. Rates above g_maxSampleRate
	// are resampled to g_resampleTarget.
	std::optional<DecodedSample> decode(const juce::File& _file, std::string& _error);

	// Builds the SysEx stream for several samples in consecutive slots starting at _firstSlot.
	std::vector<uint8_t> encode(const std::vector<DecodedSample>& _samples, uint32_t _firstSlot);

	// What the plugin uploaded per slot, kept in the plugin config so the slot
	// chooser can show names and the memory check can add up lengths. The machine's
	// own content (factory samples, uploads from elsewhere) is not known.
	struct SlotInfo
	{
		std::string name;
		uint64_t words = 0;
	};
	using SlotLedger = std::map<uint32_t, SlotInfo>;

	SlotLedger loadLedger(juce::PropertiesFile& _config);
	void saveLedger(juce::PropertiesFile& _config, const SlotLedger& _ledger);

	// Words used by the ledger, ignoring the slots that are about to be overwritten.
	uint64_t usedWords(const SlotLedger& _ledger, uint32_t _firstSlot, size_t _count);

	std::string slotLabel(uint32_t _slot);	// "R01".."R48"
}

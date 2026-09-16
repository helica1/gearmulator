#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace md::sds
{
	// Builds the SysEx byte stream the Machinedrum UW accepts for one sample:
	// a MIDI Sample Dump Standard header, the Elektron sample name message and
	// the SDS data packets, exactly as mdsysexfile.h validates them.
	//
	// The Machinedrum plays a sample at the rate given in the header, so audio
	// does not have to be resampled before encoding. 16 bit resolution, mono.
	struct Options
	{
		uint8_t slot = 0;			// RAM slot 0..47 (R01..R48)
		std::string name;			// up to 4 characters, 7-bit ASCII, upper-cased
		uint32_t sampleRateHz = 44100;
		bool loop = false;			// forward loop between loopStart and loopEnd (sample indices)
		uint32_t loopStart = 0;
		uint32_t loopEnd = 0;
	};

	constexpr uint32_t g_slotCount = 48;
	constexpr uint8_t g_bitsPerSample = 16;
	constexpr size_t g_bytesPerWord = (g_bitsPerSample + 6) / 7;	// 3
	constexpr size_t g_wordsPerPacket = 120 / g_bytesPerWord;		// 40
	constexpr size_t g_headerSize = 21;
	constexpr size_t g_nameSize = 13;
	constexpr size_t g_packetSize = 127;

	// Number of SysEx bytes encode() produces for _sampleCount samples.
	size_t encodedSize(size_t _sampleCount);

	// Returns an empty vector if the options are invalid (slot >= 48, no samples,
	// loop points out of range, sample rate 0).
	std::vector<uint8_t> encode(const std::vector<int16_t>& _samples, const Options& _options);

	// Splits a stream produced by encode() into its individual SysEx messages,
	// the form the transfer engine and Hardware::sendMidi expect.
	std::vector<std::vector<uint8_t>> splitMessages(const std::vector<uint8_t>& _stream);

	// The 4-character name the machine will show, derived from any string
	// (file stem): upper-cased, non-printable characters replaced, padded.
	std::string makeName(const std::string& _text);
}

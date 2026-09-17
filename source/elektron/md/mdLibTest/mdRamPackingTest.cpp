#include "mdLib/mdrampacking.h"
#include "dsp56kEmu/assembler.h"

#include <iostream>
#include <stdexcept>

namespace
{
	using namespace dsp56k;

	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	struct Fixture
	{
		DefaultMemoryValidator validator;
		Memory memory{validator, 0x110000};
		PeripheralsNop x, y;
		DSP dsp{memory, &x, &y};
		Assembler assembler;
		unsigned cursor = md::g_ramPackingAddress;

		explicit Fixture(const unsigned _engine)
		{
			auto config = dsp.getJit().getConfig();
			config.enableOptimizer = _engine != 1 && _engine != 3;
			config.maxInstructionsPerBlock = _engine >= 3 ? 1 : 64;
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
			for(const auto word : md::g_ramPackingOriginal)
				memory.set(MemArea_P, cursor++, word);
			emit("jmp $140");
			cursor = 0x140;
			for(const auto line : {"move y:(r6+$19),a", "move y:(r6+$b),b", "asr b", "add b,a",
				"move a1,r0", "move y:>$ff,b", "move y:(r6+$b),a", "move y:(r6+$12),x1",
				"add b,a", "cmp x1,a"})
				emit(line);
			emit(("bge " + std::to_string(0x280 - cursor)).c_str());
			emit("move a1,y:(r6+$b)");
			emit("jmp $200");
			cursor = 0x200;
			for(const auto line : {"move #$a0,r4", "move x:>$ff,b", "do b,$209",
				"move y:(r4)+,a", "move y:(r4)+,b", "merge a1,b", "move b1,x:(r0)+", "jmp $300"})
				emit(line);
			cursor = 0x280;
			emit("move #$1,x0");
			emit("move x0,y:(r6+$c)");
			emit("jmp $300");
			cursor = 0x300;
			emit("jmp $300");
		}

		void emit(const char* const _instruction)
		{
			const auto result = assembler.assemble(_instruction);
			require(result.success(), "test instruction failed to assemble");
			for(unsigned i = 0; i < result.wordCount; ++i)
				memory.set(MemArea_P, cursor++, result.word[i]);
		}

		void run(const unsigned _engine, const unsigned _previous,
			const unsigned _batch, const bool _stop, const bool _completeTail)
		{
			dsp.writeReg(Reg_B, TReg56(uint64_t(_batch) << 24));
			dsp.regs().r[6].var = 0x800;
			dsp.regs().r[5].var = 0xa0;
			memory.set(MemArea_Y, 0x80b, _previous);
			memory.set(MemArea_Y, 0x80c, 0);
			memory.set(MemArea_Y, 0x818, 0x123456);
			memory.set(MemArea_Y, 0x812, _previous + _batch + (_stop ? 0 : 1));
			memory.set(MemArea_Y, 0x819, 0x2000);
			for(unsigned i = 0; i < 40; ++i)
			{
				memory.set(MemArea_Y, 0xa0 + i, 0x800 + i);
				memory.set(MemArea_X, 0x2000 + _previous / 2 + i, 0);
			}
			dsp.setPC(md::g_ramPackingAddress);
			dsp.getJit().checkModeChange();
			unsigned steps = 0;
			while(dsp.getPC() != 0x300 && ++steps < 2048)
			{
				if(_engine)
					dsp.execJit();
				else
					dsp.execInterpreter();
			}
			require(dsp.getPC() == 0x300, "test program did not terminate");
			const auto pairs = (_batch + (_completeTail ? _previous % 2 : 0) + 1) / 2;
			require(memory.get(MemArea_X, 0xff) == pairs, "wrong packed pair count");
			require(dsp.regs().r[5].var == 0xa0 + _previous % 2, "wrong carry prefix");
			require(memory.get(MemArea_Y, 0x80b) == (_stop ? _previous : _previous + _batch),
				"wrong recording length");
			require(bool(memory.get(MemArea_Y, 0x80c)) == _stop, "wrong stop decision");
			const auto written = _stop ? 0 : pairs;
			for(unsigned i = 0; i < written; ++i)
			{
				const unsigned high = i == 0 && _previous % 2 ? 0x456 : 0x800 + 2 * i;
				const unsigned low = 0x801 + 2 * i;
				require(memory.get(MemArea_X, 0x2000 + _previous / 2 + i)
					== ((high << 12) | low), "recorded sample changed");
			}
			require(memory.get(MemArea_X, 0x2000 + _previous / 2 + written) == 0,
				"wrote past packed extent");
			if(_completeTail && !_stop)
				require(2 * (_previous / 2 + written) >= _previous + _batch,
					"valid final sample left unwritten");
		}
	};
}

int main() try
{
	unsigned cases = 0;
	for(unsigned engine = 0; engine < 5; ++engine)
	{
		Fixture fixture(engine);
		for(const unsigned previous : {0u, 1u, 11259u})
			for(const unsigned batch : {1u, 2u, 3u, 4u, 31u, 32u})
				for(const bool stop : {false, true})
				{
					fixture.run(engine, previous, batch, stop, false);
					require(md::setRamPackingMode(fixture.dsp,
						md::RamRecordingMode::CompleteTail) == md::RamPackingUpdate::Applied,
						"complete-tail mode rejected original code");
					require(md::setRamPackingMode(fixture.dsp,
						md::RamRecordingMode::CompleteTail) == md::RamPackingUpdate::AlreadyApplied,
						"complete-tail mode is not idempotent");
					fixture.run(engine, previous, batch, stop, true);
					require(md::setRamPackingMode(fixture.dsp,
						md::RamRecordingMode::Original) == md::RamPackingUpdate::Applied,
						"original mode rejected complete-tail code");
					require(md::setRamPackingMode(fixture.dsp,
						md::RamRecordingMode::Original) == md::RamPackingUpdate::AlreadyApplied,
						"original mode is not idempotent");
					fixture.run(engine, previous, batch, stop, false);
					cases += 3;
				}
	}
	{
		Fixture fixture(0);
		fixture.dsp.setPC(md::g_ramPackingAddress + 1);
		require(md::setRamPackingMode(fixture.dsp, md::RamRecordingMode::CompleteTail)
			== md::RamPackingUpdate::Busy, "changed active original sequence");
		fixture.dsp.setPC(0x300);
		require(md::setRamPackingMode(fixture.dsp, md::RamRecordingMode::CompleteTail)
			== md::RamPackingUpdate::Applied, "could not prepare complete-tail guard case");
		fixture.dsp.setPC(md::g_ramPackingAddress + 1);
		require(md::setRamPackingMode(fixture.dsp, md::RamRecordingMode::Original)
			== md::RamPackingUpdate::Busy, "changed active complete-tail sequence");
		fixture.dsp.setPC(0x300);
		fixture.memory.set(MemArea_P, md::g_ramPackingAddress + 6, 0);
		const auto neighbor = fixture.memory.get(MemArea_P, md::g_ramPackingAddress + 1);
		require(md::setRamPackingMode(fixture.dsp, md::RamRecordingMode::Original)
			== md::RamPackingUpdate::UnexpectedCode, "accepted mismatched code");
		require(fixture.memory.get(MemArea_P, md::g_ramPackingAddress + 1) == neighbor,
			"partially changed mismatched code");
	}
	std::cout << "PASS " << cases << " RAM packing cases and bidirectional guards\n";
	return 0;
}
catch(const std::exception& _error)
{
	std::cerr << "FAIL " << _error.what() << '\n';
	return 1;
}

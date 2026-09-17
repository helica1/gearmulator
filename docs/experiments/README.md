# Experiments that did not ship

## Skipping idle polling loops in the DSP emulator (September 2026)

**Idea.** Elektron's DSP firmware waits by polling: short loops that read a pin or a peripheral register and
branch back (for example the Machinedrum voice DSP waiting for its frame-sync pin at `0xbb`, or for a DMA
transfer at `0xcf`). With the sequencer stopped these loops and the silent-track delay loops are most of
the voice DSP's cycles. Every pass of a steady polling loop only advances the instruction and cycle
counters, so the counters could jump ahead to the next point where something outside the loop can change.

**Prototype** (`dsp56300-idle-loop-skip.patch`, `md-idle-loop-skip-switch.patch`):

- The JIT block analysis flags blocks made only of instructions that cannot write memory, peripherals, loop
  or mode registers (`IdleSafe`), and the block that closes a loop of at most 16 words (`IdleLoopCandidate`).
- `Jit::runIdleLoop` runs that block; when the loop is taken it runs one more pass block by block with the
  dispatcher's own checks (peripheral due, interrupt pending, processing mode, scheduler stop target). If all
  registers are unchanged it advances the counters by the whole passes that fit strictly before the next
  peripheral deadline, the scheduler stop (`DSP::execUntilCycles`) and the ESSI cycle deadline.
- `GEARMULATOR_MD_IDLE_SKIP=1` enables it for the Machinedrum/Monomachine.

**Result.** Exact but not useful:

| Machinedrum OS 1.63, M5 Max | CPU playing | Skipped instructions (≈36 s) |
|---|---|---|
| Skip off | 37.7% | 0 |
| Skip on, minimum deadline distance 64 | 40.5% | 43 million |
| Skip on, minimum deadline distance 12 | 37.3% | 576 million |

Rendered audio with skipping on was bit-identical to skipping off (8 s capture, all six outputs), and the
instruction counters matched exactly.

**Why it does not help.** Instrumenting `Peripherals56303::exec` showed that the ESSI clock sets the next
peripheral deadline almost every time, usually 2 to 64 instructions ahead: the Machinedrum's two DSPs talk
over ESSI0 in network mode, and the accurate link model services every slot. So peripherals run every few
dozen instructions no matter what the firmware is doing, and a polling loop can only ever be skipped up to
the next slot. The skipped passes were cheap JIT code to begin with; the cost that remains is the per-slot
peripheral and link servicing (`EsxiClock::exec`, `dspExecPeripherals`, the link rendezvous in the scheduler).

**Where a real saving would have to come from.** Making per-slot ESSI/link servicing cheaper, for example
batching slot transfers when neither side can observe the difference. That changes the inter-DSP link model
that the X.13 fix and Joe Landers' transport work depend on, so it needs careful design and the link
scorecard tests (`mdTransportScorecardTest`, `mdLinkTapTest`) as the safety net.

Tools used: `mdCpuBench`, `mdDspProfile`, `mdAudioCaptureTool` (outputs compared with `cmp`).

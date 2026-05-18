/*
 * apu.c — SNES Audio Processing Unit orchestration.
 *
 * Coordinates the SPC700 CPU, S-DSP, hardware timers, and I/O ports that
 * comprise the APU subsystem. The main CPU communicates with the APU through
 * four bidirectional ports ($F4-$F7); the SPC700 accesses DSP registers via
 * $F2/$F3 and controls timers/ROM visibility through $F1.
 *
 * The 64-byte IPL boot ROM at $FFC0-$FFFF handles the initial data transfer
 * protocol: it waits for the main CPU to send audio program code through the
 * I/O ports, then jumps to the transferred program.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "apu.h"
#include "snes.h"
#include "spc.h"
#include "dsp.h"

// IPL boot ROM — 64 bytes of fixed code at $FFC0-$FFFF.
// Implements the SPC700 side of the CPU↔APU data transfer protocol.
static const uint8_t bootRom[0x40] = {
  0xcd, 0xef, 0xbd, 0xe8, 0x00, 0xc6, 0x1d, 0xd0, 0xfc, 0x8f, 0xaa, 0xf4, 0x8f, 0xbb, 0xf5, 0x78,
  0xcc, 0xf4, 0xd0, 0xfb, 0x2f, 0x19, 0xeb, 0xf4, 0xd0, 0xfc, 0x7e, 0xf4, 0xd0, 0x0b, 0xe4, 0xf5,
  0xcb, 0xf4, 0xd7, 0x00, 0xfc, 0xd0, 0xf3, 0xab, 0x01, 0x10, 0xef, 0x7e, 0xf4, 0x10, 0xeb, 0xba,
  0xf6, 0xda, 0x00, 0xba, 0xf4, 0xc4, 0xf4, 0xdd, 0x5d, 0xd0, 0xdb, 0x1f, 0x00, 0x00, 0xc0, 0xff
};

Apu* apu_init() {
  Apu* apu = (Apu * )malloc(sizeof(Apu));
  apu->spc = spc_init(apu);
  apu->dsp = dsp_init(apu->ram);
  return apu;
}

void apu_free(Apu* apu) {
  spc_free(apu->spc);
  dsp_free(apu->dsp);
  free(apu);
}

// Serialize/deserialize APU state. Saves everything from `ram` through the
// end of the struct (before `hist`), then delegates to DSP and SPC700.
void apu_saveload(Apu *apu, SaveLoadFunc *func, void *ctx) {
  func(ctx, apu->ram, offsetof(Apu, hist) - offsetof(Apu, ram));
  dsp_saveload(apu->dsp, func, ctx);
  spc_saveload(apu->spc, func, ctx);
}

// Reset the APU to power-on state. Boot ROM must be enabled first because
// spc_reset reads the reset vector from $FFFE-$FFFF (which is in the boot ROM).
void apu_reset(Apu* apu) {
  apu->romReadable = true; // before resetting spc, because it reads reset vector from it
  spc_reset(apu->spc);
  dsp_reset(apu->dsp);
  memset(apu->ram, 0, sizeof(apu->ram));
  apu->dspAdr = 0;
  apu->cycles = 0;
  memset(apu->inPorts, 0, sizeof(apu->inPorts));
  memset(apu->outPorts, 0, sizeof(apu->outPorts));
  for(int i = 0; i < 3; i++) {
    apu->timer[i].cycles = 0;
    apu->timer[i].divider = 0;
    apu->timer[i].target = 0;
    apu->timer[i].counter = 0;
    apu->timer[i].enabled = false;
  }
  apu->cpuCyclesLeft = 7;
  apu->hist.count = 0;
}

// Advance the APU by one master cycle (~1.024 MHz).
// Three subsystems are serviced:
//   1. SPC700 CPU: executes an opcode when its cycle budget runs out
//   2. S-DSP: processes one sample every 32 APU cycles (32040 Hz)
//   3. Timers: timers 0/1 tick every 128 cycles (~8 kHz),
//              timer 2 ticks every 16 cycles (~64 kHz)
void apu_cycle(Apu* apu) {
  // Run the SPC700 when its current opcode's cycles are spent.
  if(apu->cpuCyclesLeft == 0) {
    apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
  }
  apu->cpuCyclesLeft--;

  // DSP runs at 1/32 the APU clock rate (every 32 cycles = one sample).
  if((apu->cycles & 0x1f) == 0) {
    dsp_cycle(apu->dsp);
  }

  // Tick the 3 hardware timers. Each timer counts down APU cycles until
  // its next tick, then increments its divider toward the target value.
  // When divider == target, the 4-bit output counter increments.
  for(int i = 0; i < 3; i++) {
    if(apu->timer[i].cycles == 0) {
      apu->timer[i].cycles = i == 2 ? 16 : 128; // Timer 2 is 8× faster
      if(apu->timer[i].enabled) {
        apu->timer[i].divider++;
        if(apu->timer[i].divider == apu->timer[i].target) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter++;
          apu->timer[i].counter &= 0xf; // 4-bit counter wraps at 16
        }
      }
    }
    apu->timer[i].cycles--;
  }

  apu->cycles++;
}

// SPC700 memory read handler. Decodes the I/O register region ($F0-$FF)
// and the boot ROM overlay ($FFC0-$FFFF). All other addresses read from RAM.
//
// Register map (SPC700 side):
//   $F0/$F1        — Test/control (return 0 on read)
//   $F2            — DSP register address
//   $F3            — DSP register data (read from addressed register)
//   $F4-$F7       — Input ports (written by main CPU, read by SPC700)
//   $F8-$F9       — Extra RAM ports (also mapped in inPorts[] for convenience)
//   $FA-$FC       — Timer targets (write-only, return 0 on read)
//   $FD-$FF       — Timer output counters (read-and-clear, 4-bit)
uint8_t apu_cpuRead(Apu* apu, uint16_t adr) {
  switch(adr) {
    case 0xf0:
    case 0xf1:
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      return 0;  // Write-only registers return 0
    }
    case 0xf2: {
      return apu->dspAdr;
    }
    case 0xf3: {
      return dsp_read(apu->dsp, apu->dspAdr & 0x7f);
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7:
    case 0xf8:
    case 0xf9: {
      return apu->inPorts[adr - 0xf4];
    }
    case 0xfd:
    case 0xfe:
    case 0xff: {
      // Timer counters are read-and-clear: reading resets the counter to 0.
      uint8_t ret = apu->timer[adr - 0xfd].counter;
      apu->timer[adr - 0xfd].counter = 0;
      return ret;
    }
  }
  // If boot ROM is mapped and address is in the ROM range, return ROM data.
  if(apu->romReadable && adr >= 0xffc0) {
    return bootRom[adr - 0xffc0];
  }
  return apu->ram[adr];
}

// SPC700 memory write handler. Decodes I/O registers ($F0-$FC).
// All writes (including to I/O addresses) also go to RAM — the hardware
// mirrors register writes into the underlying RAM at the same address.
//
// Register map (SPC700 side):
//   $F0            — Test register (ignored)
//   $F1            — Control: bits 0-2 = timer enable, bit 4/5 = clear input ports,
//                    bit 7 = boot ROM visibility
//   $F2            — DSP address port
//   $F3            — DSP data port (write → DSP register, also logged to hist)
//   $F4-$F7       — Output ports (written by SPC700, read by main CPU)
//   $F8-$F9       — Extra RAM-backed I/O bytes
//   $FA-$FC       — Timer 0/1/2 target values
void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val) {
  switch(adr) {
    case 0xf0: {
      break; // Test register — writes are ignored
    }
    case 0xf1: {
      // Control register ($F1):
      // Bits 0-2: enable timers 0-2 (enabling a stopped timer resets it)
      for(int i = 0; i < 3; i++) {
        if(!apu->timer[i].enabled && (val & (1 << i))) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter = 0;
        }
        apu->timer[i].enabled = val & (1 << i);
      }
      // Bit 4: clear input ports 0-1 ($F4-$F5)
      if(val & 0x10) {
        apu->inPorts[0] = 0;
        apu->inPorts[1] = 0;
      }
      // Bit 5: clear input ports 2-3 ($F6-$F7)
      if(val & 0x20) {
        apu->inPorts[2] = 0;
        apu->inPorts[3] = 0;
      }
      // Bit 7: enable/disable boot ROM overlay at $FFC0-$FFFF
      apu->romReadable = val & 0x80;
      break;
    }
    case 0xf2: {
      apu->dspAdr = val;  // Select DSP register for next $F3 read/write
      break;
    }
    case 0xf3: {
      // DSP data write — record in history for frame-accurate replay,
      // then forward to the DSP (only valid register addresses < 0x80).
      int i = apu->hist.count;
      if (i != 256) {
        apu->hist.count = i + 1;
        apu->hist.addr[i] = apu->dspAdr;
        apu->hist.val[i] = val;
      }
      if(apu->dspAdr < 0x80) dsp_write(apu->dsp, apu->dspAdr, val);
      break;
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      apu->outPorts[adr - 0xf4] = val;  // SPC700→CPU output ports
      break;
    }
    case 0xf8:
    case 0xf9: {
      apu->inPorts[adr - 0xf4] = val;   // Extra RAM-backed bytes
      break;
    }
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      apu->timer[adr - 0xfa].target = val;  // Set timer divider target
      break;
    }
  }
  // All writes are mirrored to RAM (hardware behavior).
  apu->ram[adr] = val;
}

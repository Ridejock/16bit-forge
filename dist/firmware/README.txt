Optional coprocessor firmware goes here. None is included.

Some enhancement chips contain a program ROM inside the chip. Games using them need a dump of
that firmware, which you have to make yourself from hardware you own:

  DSP-1 / DSP-1B / DSP-2 / DSP-3 / DSP-4   dsp1.rom, dsp1b.rom, dsp2.rom, dsp3.rom, dsp4.rom
                                           (8192 bytes: program ROM then data ROM), or split
                                           files such as dsp1.program.rom + dsp1.data.rom
  ST010 / ST011                            st010.rom, st011.rom (53248 bytes), or split files
  Cx4                                      cx4.data.rom (3072 bytes) - optional: without it the
                                           emulator generates the same math tables itself

The emulator checks the size (and, where known, the checksum) and tells you what is missing.

SCSI Tape Emulation Support for QEMU
====================================

The project was proposed during GSoC 2026 but there were not enough slots allocated.

QEMU provides functionality to emulate SCSI hard discs and SCSI CD-ROM drives, but lacks an emulated SCSI TAPE drive. The goal of this project is to develop an emulation for a SCSI TAPE drive which is backed by a file in the host filesystem, similar to an ISO file which is used to emulate a CD-ROM drive.

This will involve writing code to emulate a SCSI TAPE drive and storing the data in a file, adding test coverage, and documenting how to use the new feature. Finally, it should be possible to backup files from the current emulated OS to tape via standard tools, e.g. tar and mt on Linux.

Current Status:
===============
- The basic set of required commands [REALIZE, UNREALIZE, INQUIRY, READ, TEST_UNIT_READY] are implemented.

Right now, current implementation is being tested by booting the MPE/iX which expects to see a tape after modification to seabios-hppa by Helge Deller to support booting from a tape. It is being tested on an emulated HP A400 and HP B160L machines. It HARD Booted successfully but hits a SeaBios trap #9, at 0x0:0x11700, IIR=0x0, IOR addr=0x0:0x0 cause the wrong data is read from the virtual tape after boot.

Seabios tape support branch:
============================
 https://github.com/hdeller/seabios-hppa/tree/devel-tape

Build:
======
.. code-block:: shell
./build/qemu-system-hppa -drive file=MPE75SLT.std,if=none,id=tape0,format=raw -device scsi-tape,drive=tape0 -nographic -smp cpus=1 -snapshot -bios ../seabios-hppa/out/hppa-firmware.img

Fix:
====
- Implement a TAP file format in the SCSI driver (record length headers/trailers)
- Handle READ and WRITE correctly.
- Add support for tracing for debugging


Todo:
=====
- Implement other SCSI commands [REQUEST SENSE, WRITE, WRITE FILEMARKS, SPACE, READ POSITION, MODE SENSE]
- Make it work with Linux utils like tar and mt which is used to test that read and write works correctly
- Testing and Documentation 


Project Mentor - Helge Deller <deller@gmx.de>

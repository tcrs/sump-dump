Simple tool to get the samples from a logic analyser supporting the SUMP
protocol. Note that I developed this using the OpenLogicSniffer bitfile for a
Papillio Pro (which is known to be a bit quirky) so YMMV.

Supports simple triggers and dumping data in raw binary, hex or VCD format.
GTKWave works well for viewing the VCD output.

Currently only tested on Linux, but should work fine on other UNIX platforms.

RLE mode doesn't seem to work for me (possibly just broken on the OLS), so it is
totally untested and will need fixing if you want to use it. Extended metadata
is also untested as (spotting a theme here?) the OLS support for it seems buggy.

No library dependencies required, just run `make`.

	Usage: ./sump-dump <tty> [<options>]
	
	Default mode is to dump sample data to stdout as hex, one sample per line.
	Example: ./sump /dev/ttyUSB1 trigger 0x1=0x1 groups 3 divisor 11 raw
	
	groups <num>: mask of channel groups to enable (default = all groups).
	        Each group is a block of 8 channels.
	trigger <mask>=<value>: trigger condition.
	        Capture will start when value of (channels & mask) == value.
	divisor <num>: clock divisor to use for capture rate (default = 1).
	samples <num>: number of samples to capture (default = max possible).
	before <num>: number of samples (out of those captured) to return preceding the trigger (default = 4).
	rle: enable RLE sample compression (EXPERIMENTAL) (default = false).
	raw: dump sample data in binary to stdout (default = false).
	vcd name=mask,mask..: dump samples in VCD format.
	    Each instance adds the named value to the output using the specified bits.
	    e.g. vcd clock=0x1 vcd data=0x6,0x80
	    will add two values: a single bit clock from sample bit 0, and a 3 bit data value
	    from sample bits 3,1,7 (in that order msb->lsb).
	extmeta: device supports extended metadata command (0x04) (default = false)
	        The following settings will be set from the metadata provided by the device
	sample_memory: bytes of sample memory provided by the device (SI K & M suffixes allowed) (default = 16KB)
	clk_freq: capture clock freqency (SI K & M suffixes allowed) (default = 100MHz)
	num_probes: number of probes provided by the device (default = 32)

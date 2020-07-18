/*
Copyright (c) 2017 Thomas Spurden <thomas@spurden.name>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <termios.h>
#include <time.h>
#include <strings.h>

static void perror_exit(char const* msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static void setup_serial(int fd)
{
	struct termios tios;
	if(tcgetattr(fd, &tios) == -1) {
		perror_exit("tcgetattr");
	}

	tios.c_iflag &= ~(INPCK | ISTRIP | IGNCR | ICRNL | INLCR | IXOFF | IXON);
	tios.c_oflag &= ~OPOST;
	tios.c_lflag &= ~(ISIG | ICANON | ECHO);
	tios.c_cflag &= ~(CSTOPB | PARENB | CSIZE);

	tios.c_iflag |= IGNBRK;
	tios.c_cflag |= (CS8 | CREAD);

	if(cfsetospeed(&tios, B115200) == -1) {
		perror_exit("cfsetospeed");
	}
	if(cfsetispeed(&tios, B115200) == -1) {
		perror_exit("cfsetospeed");
	}

	if(tcsetattr(fd, TCSANOW, &tios) == -1) {
		perror_exit("tcsetattr");
	}
}

struct cmd {
	uint8_t data[5];
	unsigned len;
};

static struct cmd const cmd_reset = { .data = { 0, }, .len = 1 };
static struct cmd const cmd_run = { .data = { 1, }, .len = 1 };
static struct cmd const cmd_id = { .data = { 2, }, .len = 1 };
static struct cmd const cmd_get_meta = { .data = { 4, }, .len = 1 };

static void write_tty(int fd, struct cmd const* cmd)
{
	fprintf(stderr, ">");
	for(unsigned i = 0; i < cmd->len; i += 1) {
		fprintf(stderr, " %02X", cmd->data[i]);
	}
	fprintf(stderr, "\n");

	ssize_t sz = write(fd, cmd->data, cmd->len);
	if(sz == -1) {
		perror_exit("Error writing command to tty");
	}
	if(sz != cmd->len) {
		perror_exit("Could not write whole command to tty");
	}
}

static void read_tty(int fd, uint8_t* buf, size_t bytes)
{
	size_t pos = 0;
	while(pos < bytes) {
		ssize_t sz = read(fd, &buf[pos], bytes - pos);
		if(sz == -1) {
			perror_exit("Error reading from tty");
		}
		pos += sz;
	}
}

static void cmd_divider(struct cmd* cmd, uint32_t div)
{
	assert((div >> 24) == 0);
	cmd->len = 5;
	cmd->data[0] = 0x80;
	cmd->data[1] = div & 0xFF;
	cmd->data[2] = (div >> 8) & 0xFF;
	cmd->data[3] = (div >> 16) & 0xFF;
	cmd->data[4] = 0;
}

static void cmd_counts(struct cmd* cmd, uint16_t read_count, uint16_t delay_count)
{
	cmd->len = 5;
	cmd->data[0] = 0x81;
	cmd->data[1] = read_count & 0xFF;
	cmd->data[2] = (read_count >> 8) & 0xFF;
	cmd->data[3] = delay_count & 0xFF;
	cmd->data[4] = (delay_count >> 8) & 0xFF;
}

static void cmd_flags(struct cmd* cmd, unsigned group_disable,
	bool demux, bool filter, bool external, bool inverted, bool rle)
{
	assert((group_disable >> 4) == 0);
	cmd->len = 5;
	cmd->data[0] = 0x82;
	cmd->data[1] = (group_disable << 2)
		| (demux? 0x01 : 0)
		| (filter? 0x02 : 0)
		| (external? 0x40 : 0)
		| (inverted? 0x80 : 0);
	cmd->data[2] = (rle? 0x01 : 0);
	cmd->data[3] = 0;
	cmd->data[4] = 0;
}

static void cmd_trig_mask(struct cmd* cmd, unsigned stage, uint32_t mask)
{
	assert(stage <= 3);
	cmd->len = 5;
	cmd->data[0] = 0xC0 | (stage << 2);
	cmd->data[1] = mask & 0xFF;
	cmd->data[2] = (mask >> 8) & 0xFF;
	cmd->data[3] = (mask >> 16) & 0xFF;
	cmd->data[4] = (mask >> 24) & 0xFF;
}

static void cmd_trig_value(struct cmd* cmd, unsigned stage, uint32_t vals)
{
	assert(stage <= 3);
	cmd->len = 5;
	cmd->data[0] = 0xC1 | (stage << 2);
	cmd->data[1] = vals & 0xFF;
	cmd->data[2] = (vals >> 8) & 0xFF;
	cmd->data[3] = (vals >> 16) & 0xFF;
	cmd->data[4] = (vals >> 24) & 0xFF;
}

static void cmd_trig_cfg(struct cmd* cmd, unsigned stage,
	uint16_t delay, unsigned level, unsigned channel,
	bool serial, bool start)
{
	assert(stage <= 3);
	assert(level <= 3);
	assert(channel <= 31);
	cmd->len = 5;
	cmd->data[0] = 0xC2 | (stage << 2);
	cmd->data[1] = delay & 0xFF;
	cmd->data[2] = (delay >> 8) & 0xFF;
	cmd->data[3] = ((channel & 0xF) << 4) | level;
	cmd->data[4] = (channel >> 4) | (serial? 0x4 : 0) | (start? 0x8 : 0);
}

#define MAX_VCD_VALUES 32
#define MAX_VCD_VALUE_BITS 32
#define MAX_VCD_NAME_LEN 32

struct cfg {
	uint32_t group_enable;
	uint32_t trigger_mask, trigger_value;
	uint32_t clk_divisor;
	uint32_t samples;
	uint32_t before_trig;
	bool rle, raw;
	bool ext_meta;

	/* Device info - either from etended metadata or provided on cmdline */
	uint32_t clk_freq_hz, sample_memory, num_probes;

	struct {
		uint32_t num_values;
		struct vcd_value {
			char name[MAX_VCD_NAME_LEN+1];
			uint32_t mask;
			uint32_t num_bits;
			uint32_t bitmasks[MAX_VCD_VALUE_BITS];
		} values[MAX_VCD_VALUES];
	} vcd;

	/* Calculated from other config values */
	uint32_t max_groups, group_mask;
	uint32_t num_groups_enabled;
};

void read_ident(int fd, struct cfg* cfg)
{

	for(unsigned i = 0; i < 5; i += 1) {
		write_tty(fd, &cmd_reset);
	}
	write_tty(fd, &cmd_id);
	uint8_t ident[4];
	read_tty(fd, ident, 4);
	if(memcmp(ident, "1ALS", 4) != 0) {
		fprintf(stderr, "Unknown ident: %c%c%c%c\n", ident[0], ident[1], ident[2], ident[3]);
		exit(EXIT_FAILURE);
	}
	fprintf(stderr, "Sump device found OK\n");

	if(!cfg->ext_meta) {
		return;
	}

	write_tty(fd, &cmd_get_meta);

	while(1) {
		uint8_t meta;
		read_tty(fd, &meta, 1);
		if(meta == 0) {
			/* End of metadata */
			break;
		}
		switch(meta >> 5) {
			case 0: { /* NULL terminated string */
					uint8_t val[256];
					unsigned i = 0;
					do {
						read_tty(fd, &val[i], 1);
						i += 1;
					}
					while(val[i - 1] != '\0' && i != sizeof(val));
					if(val[i - 1] != '\0') {
						fprintf(stderr, "Error: truncating excessively long extended metadata string\n");
						do {
							read_tty(fd, &val[sizeof(val) - 1], 1);
						}
						while(val[sizeof(val) - 1] != '\0');
					}
					fprintf(stderr, "str[%u] = \"%s\"\n", meta & 0x1f, (char const*)val);
				}
				break;
			case 1: { /* 32-bit uint */
					uint8_t valb[4];
					read_tty(fd, valb, 4);
					uint32_t val = (uint32_t)valb[3] | (valb[2] << 8) | (valb[1] << 16) | (valb[2] << 24);
					fprintf(stderr, "u32[%u] = 0x%08X\n", meta & 0x1f, val);

					/* Fill in relevant info */
					switch(meta & 0x1f) {
						case 0: cfg->num_probes = val; break;
						case 1: cfg->sample_memory = val; break;
						case 3: cfg->clk_freq_hz = val; break;
						default: break;
					}
				}
				break;
			case 2: { /* 8-bit uint */
					uint8_t val;
					read_tty(fd, &val, 1);
					fprintf(stderr, "u8[%u] = 0x%02X\n", meta & 0x1f, val);
				}
				break;
			default:
				fprintf(stderr, "Unexpected extended metadata type %u (from byte 0x%02X)\n", meta >> 5, meta);
				return;
		}
	}
}

static void write_vcd_value(FILE* dest, struct cfg const* cfg, unsigned vali, uint32_t sample)
{
	struct vcd_value const* vv = &cfg->vcd.values[vali];
	if(vv->num_bits == 1) {
		fprintf(dest, "%u%c\n", (sample & vv->mask)? 1 : 0, 33 + vali);
	}
	else {
		fprintf(dest, "b");
		for(unsigned biti = 0; biti < vv->num_bits; biti += 1) {
			fprintf(dest, "%u", (sample & vv->bitmasks[biti])? 1 : 0);
		}
		fprintf(dest, " %c\n", 33 + vali);
	}
}

static void write_vcd(FILE* dest, struct cfg const* cfg, uint8_t const* sample_buf, uint32_t num_samples)
{
	char const* const units[] = { "s", "ms", "us", "ns", "ps", "fs" };
	unsigned const tens[] = { 1, 10, 100 };

	/* capture frequency = (clk freq) / (clk divisor)
	 * period between samples = 1 / (capture_frequency)
	 */
	unsigned ntens = 0;
	double time_scale = 1.0;
	double divisor = (double)cfg->clk_divisor;
	double freq = (double)cfg->clk_freq_hz;
	while((divisor * time_scale) / freq < 100.0) {
		time_scale *= 10.0;
		ntens += 1;
	}
	double const period = (divisor * time_scale) / freq;

	unsigned const unit = ntens / 3;
	unsigned const unit_scale = tens[ntens % 3];

	assert(unit < 6);

	fprintf(stderr, "Captured at %lfHz, period = %lf * %u%s\n", freq / divisor, period, unit_scale, units[unit]);

	/* Header */
	time_t curtime = time(NULL);
	fprintf(dest, "$date\n  %s$end\n", ctime(&curtime));
	fprintf(dest, "$version\n   Sump dumper\n$end\n");
	fprintf(dest, "$timescale %u%s $end\n", unit_scale, units[unit]);
	for(unsigned vali = 0; vali < cfg->vcd.num_values; vali += 1) {
		struct vcd_value const* vv = &cfg->vcd.values[vali];
		fprintf(dest, "$var wire %u %c %s $end\n", vv->num_bits, 33 + vali, vv->name);
	}
	fprintf(dest, "$enddefinitions $end\n");
	fprintf(dest, "$dumpvars\n");
	for(unsigned vali = 0; vali < cfg->vcd.num_values; vali += 1) {
		write_vcd_value(dest, cfg, vali, 0);
	}
	fprintf(dest, "$end\n");
	/* Samples */
	uint32_t prev = 0;
	uint8_t const* ptr = &sample_buf[(num_samples * cfg->num_groups_enabled) - 1];
	for(unsigned i = 0; i < num_samples; i += 1) {
		/* Get all groups samples into single 32bit word */
		uint32_t cur = 0;
		for(unsigned j = 0; j < cfg->num_groups_enabled; j += 1) {
			cur <<= 8;
			cur |= *(ptr - (cfg->num_groups_enabled - j));
		}
		ptr -= cfg->num_groups_enabled;

		/* Write out changed values */
		uint32_t const changed = prev ^ cur;
		bool written_time = false;
		for(unsigned vali = 0; vali < cfg->vcd.num_values; vali += 1) {
			struct vcd_value const* vv = &cfg->vcd.values[vali];
			if(i == num_samples - 1 || (changed & vv->mask)) {
				if(!written_time) {
					unsigned current_time = (unsigned int)((double)i * period);
					fprintf(dest, "#%u\n", current_time);
					written_time = true;
				}
				write_vcd_value(dest, cfg, vali, cur);
			}
		}

		prev = cur;
	}
}

static void capture(int fd, struct cfg const* cfg)
{
	uint32_t group_dis = ~cfg->group_enable & cfg->group_mask;

	/* Reset (5 times as spec-ed */
	for(unsigned i = 0; i < 5; i += 1) {
		write_tty(fd, &cmd_reset);
	}

	struct cmd cmd;

	cmd_divider(&cmd, cfg->clk_divisor - 1);
	write_tty(fd, &cmd);

	if(cfg->trigger_mask == 0) {
		cmd_trig_mask(&cmd, 0, 0);
		write_tty(fd, &cmd);

		cmd_trig_value(&cmd, 0, 0);
		write_tty(fd, &cmd);

		cmd_trig_cfg(&cmd, 0, 0, 0, 0, false, true);
		write_tty(fd, &cmd);
	}
	else {
		cmd_trig_mask(&cmd, 0, cfg->trigger_mask);
		write_tty(fd, &cmd);
		cmd_trig_value(&cmd, 0, cfg->trigger_value);
		write_tty(fd, &cmd);
		cmd_trig_cfg(&cmd, 0, 0, 0, 0, false, true);
		write_tty(fd, &cmd);

		for(unsigned i = 1; i < 4; i += 1) {
			cmd_trig_mask(&cmd, i, 0);
			write_tty(fd, &cmd);
			cmd_trig_value(&cmd, i, 0);
			write_tty(fd, &cmd);
			cmd_trig_cfg(&cmd, i, 0, 3, 0, false, false);
			write_tty(fd, &cmd);
		}
	}

	uint32_t max_samples = (cfg->sample_memory / cfg->num_groups_enabled);
	uint32_t capture_samples = cfg->samples;
	if(cfg->samples > max_samples) {
		fprintf(stderr, "Warning: requested more samples than the maximum (%u).\n", max_samples);
		capture_samples = max_samples;
	}
	uint32_t before_samples = cfg->before_trig;
	if(cfg->before_trig > capture_samples) {
		fprintf(stderr, "Warning: requested more samples before trigger (%u) than number captured (%u).\n", cfg->before_trig, capture_samples);
		before_samples = capture_samples;
	}
	cmd_counts(&cmd, capture_samples / 4, (capture_samples - before_samples) / 4);
	write_tty(fd, &cmd);

	cmd_flags(&cmd, group_dis, false, false, false, false, cfg->rle);
	write_tty(fd, &cmd);

	write_tty(fd, &cmd_run);

	uint8_t* buf = malloc(capture_samples * cfg->num_groups_enabled);
	assert(buf);
	read_tty(fd, buf, capture_samples * cfg->num_groups_enabled);

	if(cfg->vcd.num_values) {
		write_vcd(stdout, cfg, buf, capture_samples);
	}
	else {
		uint8_t const* ptr = &buf[(capture_samples * cfg->num_groups_enabled) - 1];

		for(unsigned i = 0; i < capture_samples; i += 1) {
			if(cfg->raw) {
				fwrite(ptr - cfg->num_groups_enabled, 1, cfg->num_groups_enabled, stdout);
				ptr -= cfg->num_groups_enabled;
			}
			else {
				for(unsigned j = 0; j < cfg->num_groups_enabled; j += 1) {
					printf("%02X", *(ptr - (cfg->num_groups_enabled - j)));
				}
				printf("\n");
			}
			ptr -= cfg->num_groups_enabled;
		}
	}

	free(buf);
}

struct args {
	char** argv;
	unsigned argc;
	unsigned pos;
	void (*err)(struct args*, char*);
};

static char* args_pop(struct args* args)
{
	if(args->pos == args->argc) {
		return NULL;
	}
	else {
		return args->argv[args->pos++];
	}
}

static void args_number(struct args* args, uint32_t* num, char* msg)
{
	char* arg = args_pop(args);
	if(arg == NULL) {
		args->err(args, msg);
	}
	char* end;
	unsigned long long n = strtoull(arg, &end, 0);
	if(end == arg || end[0] != '\0' || n > UINT32_MAX) {
		args->err(args, msg);
	}

	*num = (uint32_t)n;
}

static void args_si_unit(struct args* args, uint32_t* num, char const* unit, char* msg)
{
	char* arg = args_pop(args);
	if(arg == NULL) {
		args->err(args, msg);
	}
	char* end;
	unsigned long long n = strtoull(arg, &end, 0);
	if(end == arg || n > UINT32_MAX) {
		args->err(args, msg);
	}
	switch(end[0]) {
		case 'M':
		case 'm':
			n *= 1000000;
			end += 1;
			break;
		case 'K':
		case 'k':
			n *= 1000;
			end += 1;
			break;
		case '\0':
			break;
		default:
			args->err(args, msg);
			break;
	}

	if(n > UINT32_MAX) {
		args->err(args, msg);
	}

	if(end[0] != '\0') {
		if(strcasecmp(end, unit) != 0) {
			args->err(args, msg);
		}
	}

	*num = (uint32_t)n;
}

static void args_numeqnum(struct args* args, uint32_t* num0, uint32_t* num1, char* msg)
{
	char* arg = args_pop(args);
	if(arg == NULL) {
		args->err(args, msg);
	}
	char* end;
	unsigned long long n0 = strtoull(arg, &end, 0);
	if(end == arg || end[0] != '=' || n0 > UINT32_MAX) {
		args->err(args, msg);
	}

	arg = &end[1];
	unsigned long long n1 = strtoull(arg, &end, 0);
	if(end == arg || end[0] != '\0' || n1 > UINT32_MAX) {
		args->err(args, msg);
	}

	*num0 = (uint32_t)n0;
	*num1 = (uint32_t)n1;
}

static void args_vcd_value(struct args* args, struct vcd_value* vv, char* msg)
{
	char* arg = args_pop(args);
	if(arg == NULL) {
		args->err(args, msg);
	}

	char* p = arg;
	while(p[0] != '=') {
		if(p[0] == '\0') {
			args->err(args, msg);
		}
		p += 1;
	}
	unsigned len = p - arg;
	if(len == 0 || len > MAX_VCD_NAME_LEN) {
		args->err(args, msg);
	}
	memset(vv->name, 0, sizeof(vv->name));
	strncpy(vv->name, arg, len);

	vv->mask = 0;
	p += 1;
	do {
		char* end;
		unsigned long long n = strtoull(p, &end, 0);
		if(end == p || n > UINT32_MAX) {
			args->err(args, msg);
		}
		if(n & vv->mask) {
			fprintf(stderr, "Warning: overlapping value bits in VCD spec\n");
		}
		vv->mask |= n;
		uint32_t tm = 0x80000000;
		for(unsigned i = 0; i < 32; i += 1) {
			if(tm & n) {
				if(vv->num_bits >= MAX_VCD_VALUE_BITS) {
					args->err(args, msg);
				}
				vv->bitmasks[vv->num_bits++] = tm;
			}
			tm >>= 1;
		}
		p = end;
	}
	while(p[0] == ',');
}

static void argerr(struct args* args, char* msg) {
	if(msg) {
		fprintf(stderr, "argument error: %s\n", msg);
	}
	fprintf(stderr, "Usage: %s <tty> [<options>]\n\n", args->argv[0]);
	fprintf(stderr, "Default mode is to dump sample data to stdout as hex, one sample per line.\n"
		"Example: %s /dev/ttyUSB1 trigger 0x1=0x1 groups 3 divisor 11 raw\n\n", args->argv[0]);
	fprintf(stderr,
		"groups <num>: mask of channel groups to enable (default = all groups).\n"
		"	Each group is a block of 8 channels.\n"
		"trigger <mask>=<value>: trigger condition.\n"
		"	Capture will start when value of (channels & mask) == value.\n"
		"divisor <num>: clock divisor to use for capture rate (default = 1).\n"
		"samples <num>: number of samples to capture (default = max possible).\n"
		"before <num>: number of samples (out of those captured) to return preceding the trigger (default = 4).\n"
		"after <num>: number of samples (out of those captured) to return after the trigger (default = samples - before)\n"
		"  This takes precedence over 'before'\n"
		"rle: enable RLE sample compression (EXPERIMENTAL) (default = false).\n"
		"raw: dump sample data in binary to stdout (default = false).\n"
		"vcd name=mask,mask..: dump samples in VCD format.\n"
		"    Each instance adds the named value to the output using the specified bits.\n"
		"    e.g. vcd clock=0x1 vcd data=0x6,0x80\n"
		"    will add two values: a single bit clock from sample bit 0, and a 3 bit data value\n"
		"    from sample bits 3,1,7 (in that order msb->lsb).\n"
		"extmeta: device supports extended metadata command (0x04) (default = false)\n"
		"	The following settings will be set from the metadata provided by the device\n"
		"sample_memory: bytes of sample memory provided by the device (SI K & M suffixes allowed) (default = 16KB)\n"
		"clk_freq: capture clock freqency (SI K & M suffixes allowed) (default = 100MHz)\n"
		"num_probes: number of probes provided by the device (default = 32)\n"
		);
	exit(EXIT_FAILURE);
}

int main(int argc, char** argv)
{
	struct args args = { .argv = argv, .argc = argc, .pos = 2, .err = argerr };
	if(argc < 2) {
		argerr(&args, NULL);
	}

	/* Only used to derive cfg.before_trig */
	uint32_t after_trig = UINT32_MAX;

	struct cfg cfg = {
		.trigger_mask = 0, .trigger_value = 0,
		.clk_divisor = 1,
		.before_trig = 4,
		.rle = false,
		.raw = false,
		.vcd = { .num_values = 0, },
		/* Default to papilio pro as that is what I use... */
		.num_probes = 32,
		.sample_memory = (1u << 16),
		.clk_freq_hz = 100000000,
		.ext_meta = false,
	};

	while(args.pos < args.argc) {
		char* opt = args_pop(&args);
		if(strcmp(opt, "groups") == 0) {
			args_number(&args, &cfg.group_enable, "Invalid groups parameter: must be number <= 0xF");
		}
		else if(strcmp(opt, "trigger") == 0) {
			args_numeqnum(&args, &cfg.trigger_mask, &cfg.trigger_value, "Invalid trigger parameter: must be number=number");
		}
		else if(strcmp(opt, "divisor") == 0) {
			args_number(&args, &cfg.clk_divisor, "Invalid clock divisor");
		}
		else if(strcmp(opt, "samples") == 0) {
			args_number(&args, &cfg.samples, "Invalid samples count");
		}
		else if(strcmp(opt, "before") == 0) {
			args_number(&args, &cfg.before_trig, "Invalid before trigger samples count");
		}
		else if(strcmp(opt, "after") == 0) {
			args_number(&args, &after_trig, "Invalid after trigger samples count");
		}
		else if(strcmp(opt, "rle") == 0) {
			cfg.rle = true;
		}
		else if(strcmp(opt, "raw") == 0) {
			cfg.raw = true;
		}
		else if(strcmp(opt, "clk_freq") == 0) {
			args_si_unit(&args, &cfg.clk_freq_hz, "hz", "Invalid clock frequency");
		}
		else if(strcmp(opt, "sample_memory") == 0) {
			args_si_unit(&args, &cfg.sample_memory, "B", "Invalid sample memory size");
		}
		else if(strcmp(opt, "num_probes") == 0) {
			args_number(&args, &cfg.num_probes, "Invalid probe count");
		}
		else if(strcmp(opt, "extmeta") == 0) {
			cfg.ext_meta = false;
		}
		else if(strcmp(opt, "vcd") == 0) {
			if(cfg.vcd.num_values == MAX_VCD_VALUES) {
				argerr(&args, "Too many VCD values specified");
			}
			args_vcd_value(&args, &cfg.vcd.values[cfg.vcd.num_values], "Invalid VCD value specifier");
			cfg.vcd.num_values += 1;
		}
		else {
			argerr(&args, "Unknown argument");
		}
	}

	int fd = open(argv[1], O_RDWR | O_NOCTTY);
	if(fd == -1) {
		fprintf(stderr, "Error opening %s: %s\n", argv[1], strerror(errno));
		exit(EXIT_FAILURE);
	}

	setup_serial(fd);

	read_ident(fd, &cfg);

	/* This goes after read_ident as some of the values used may be filled in
	 * from the extended metadata (if enabled) */
	cfg.max_groups = (cfg.num_probes + 7) / 8;
	cfg.group_mask = (1u << cfg.max_groups) - 1;

	/* Default to all groups */
	if(cfg.group_enable == 0) {
		cfg.group_enable = cfg.group_mask;
	}

	cfg.num_groups_enabled = 0;
	for(unsigned n = cfg.group_enable & cfg.group_mask; n; n >>= 1) {
		cfg.num_groups_enabled += 1;
	}

	/* Default to max samples */
	if(cfg.samples == 0) {
		cfg.samples = cfg.sample_memory / cfg.num_groups_enabled;
	}

	/* after_trig overrides before_trig */
	if(after_trig != UINT32_MAX) {
		cfg.before_trig = cfg.samples - (after_trig > cfg.samples? cfg.samples : after_trig);
	}

	if(cfg.group_enable > cfg.group_mask) {
		fprintf(stderr, "Warning: requested more channel groups (0x%X) than available (0x%X).\n", cfg.group_enable, cfg.group_mask);
	}

	if(cfg.clk_freq_hz == 0) {
		fprintf(stderr, "Must specify clock frequency (clk_freq)\n");
		exit(EXIT_FAILURE);
	}

	capture(fd, &cfg);

	close(fd);
}

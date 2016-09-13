#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/select.h>
#include "gb.h"
#include "memory.h"
#include "timing.h"
#include "z80_cpu.h"
#include "joypad.h"
#include "display.h"
#include "debugger.h"
#include "mbc.h"

void GB_attributed_logv(GB_gameboy_t *gb, GB_log_attributes attributes, const char *fmt, va_list args)
{
    char *string = NULL;
    vasprintf(&string, fmt, args);
    if (string) {
        if (gb->log_callback) {
            gb->log_callback(gb, string, attributes);
        }
        else {
            /* Todo: Add ANSI escape sequences for attributed text */
            printf("%s", string);
        }
    }
    free(string);
}

void GB_attributed_log(GB_gameboy_t *gb, GB_log_attributes attributes, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    GB_attributed_logv(gb, attributes, fmt, args);
    va_end(args);
}

void GB_log(GB_gameboy_t *gb,const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    GB_attributed_logv(gb, 0, fmt, args);
    va_end(args);
}

static char *default_input_callback(GB_gameboy_t *gb)
{
    char *expression = NULL;
    size_t size = 0;

    if (getline(&expression, &size, stdin) == -1) {
        /* The user doesn't have STDIN or used ^D. We make sure the program keeps running. */
        GB_set_async_input_callback(gb, NULL); /* Disable async input */
        return strdup("c");
    }

    if (!expression) {
        return strdup("");
    }

    size_t length = strlen(expression);
    if (expression[length - 1] == '\n') {
        expression[length - 1] = 0;
    }
    return expression;
}

static char *default_async_input_callback(GB_gameboy_t *gb)
{
#ifndef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval time = {0,};
    if (select(1, &set, NULL, NULL, &time) == 1) {
        if (feof(stdin)) {
            GB_set_async_input_callback(gb, NULL); /* Disable async input */
            return NULL;
        }
        return default_input_callback(gb);
    }
#endif
    return NULL;
}

void GB_init(GB_gameboy_t *gb)
{
    memset(gb, 0, sizeof(*gb));
    gb->magic = (uintptr_t)'SAME';
    gb->version = GB_STRUCT_VERSION;
    gb->ram = malloc(gb->ram_size = 0x2000);
    gb->vram = malloc(gb->vram_size = 0x2000);

    gb->mbc_rom_bank = 1;
    gb->last_rtc_second = time(NULL);
    gb->last_vblank = clock();
    gb->cgb_ram_bank = 1;

    /* Todo: this bypasses the rgb encoder because it is not set yet. */
    gb->sprite_palletes_rgb[4] = gb->sprite_palletes_rgb[0] = gb->background_palletes_rgb[0] = 0xFFFFFFFF;
    gb->sprite_palletes_rgb[5] = gb->sprite_palletes_rgb[1] = gb->background_palletes_rgb[1] = 0xAAAAAAAA;
    gb->sprite_palletes_rgb[6] = gb->sprite_palletes_rgb[2] = gb->background_palletes_rgb[2] = 0x55555555;
    gb->input_callback = default_input_callback;
    gb->async_input_callback = default_async_input_callback;
    gb->cartridge_type = &GB_cart_defs[0]; // Default cartridge type

    gb->io_registers[GB_IO_OBP0] = gb->io_registers[GB_IO_OBP1] = 0xFF;
    gb->io_registers[GB_IO_JOYP] = 0xF;
    gb->io_registers[GB_IO_SC] = 0x7E;
}

void GB_init_cgb(GB_gameboy_t *gb)
{
    memset(gb, 0, sizeof(*gb));
    gb->magic = (uintptr_t)'SAME';
    gb->version = GB_STRUCT_VERSION;
    gb->ram = malloc(gb->ram_size = 0x2000 * 8);
    gb->vram = malloc(gb->vram_size = 0x2000 * 2);
    gb->is_cgb = true;
    gb->cgb_mode = true;

    gb->mbc_rom_bank = 1;
    gb->last_rtc_second = time(NULL);
    gb->last_vblank = clock();
    gb->cgb_ram_bank = 1;
    gb->input_callback = default_input_callback;
    gb->async_input_callback = default_async_input_callback;
    gb->cartridge_type = &GB_cart_defs[0]; // Default cartridge type

    gb->io_registers[GB_IO_OBP0] = gb->io_registers[GB_IO_OBP1] = 0xFF;
    gb->io_registers[GB_IO_JOYP] = 0xF;
    gb->io_registers[GB_IO_SC] = 0x7C;
}

void GB_free(GB_gameboy_t *gb)
{
    if (gb->ram) {
        free(gb->ram);
    }
    if (gb->vram) {
        free(gb->vram);
    }
    if (gb->mbc_ram) {
        free(gb->mbc_ram);
    }
    if (gb->rom) {
        free(gb->rom);
    }
    if (gb->audio_buffer) {
        free(gb->audio_buffer);
    }
    if (gb->breakpoints) {
        free(gb->breakpoints);
    }
    for (int i = 0x200; i--;) {
        if (gb->bank_symbols[i]) {
            GB_map_free(gb->bank_symbols[i]);
        }
    }
    for (int i = 0x400; i--;) {
        if (gb->reversed_symbol_map.buckets[i]) {
            GB_symbol_t *next = gb->reversed_symbol_map.buckets[i]->next;
            free(gb->reversed_symbol_map.buckets[i]);
            gb->reversed_symbol_map.buckets[i] = next;
        }
    }
}

int GB_load_boot_rom(GB_gameboy_t *gb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return errno;
    fread(gb->boot_rom, sizeof(gb->boot_rom), 1, f);
    fclose(f);
    return 0;
}

int GB_load_rom(GB_gameboy_t *gb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return errno;
    fseek(f, 0, SEEK_END);
    gb->rom_size = (ftell(f) + 0x3FFF) & ~0x3FFF; /* Round to bank */
    /* And then round to a power of two */
    while (gb->rom_size & (gb->rom_size - 1)) {
        /* I promise this works. */
        gb->rom_size |= gb->rom_size >> 1;
        gb->rom_size++;
    }
    fseek(f, 0, SEEK_SET);
    gb->rom = malloc(gb->rom_size);
    memset(gb->rom, 0xFF, gb->rom_size); /* Pad with 0xFFs */
    fread(gb->rom, gb->rom_size, 1, f);
    fclose(f);
    GB_configure_cart(gb);

    return 0;
}

static bool dump_section(FILE *f, const void *src, uint32_t size)
{
    if (fwrite(&size, 1, sizeof(size), f) != sizeof(size)) {
        return false;
    }

    if (fwrite(src, 1, size, f) != size) {
        return false;
    }

    return true;
}

#define DUMP_SECTION(gb, f, section) dump_section(f, GB_GET_SECTION(gb, section), GB_SECTION_SIZE(section))

/* Todo: we need a sane and protable save state format. */
int GB_save_state(GB_gameboy_t *gb, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return errno;
    }

    if (fwrite(GB_GET_SECTION(gb, header), 1, GB_SECTION_SIZE(header), f) != GB_SECTION_SIZE(header)) goto error;
    if (!DUMP_SECTION(gb, f, core_state)) goto error;
    if (!DUMP_SECTION(gb, f, dma       )) goto error;
    if (!DUMP_SECTION(gb, f, mbc       )) goto error;
    if (!DUMP_SECTION(gb, f, hram      )) goto error;
    if (!DUMP_SECTION(gb, f, timing    )) goto error;
    if (!DUMP_SECTION(gb, f, apu       )) goto error;
    if (!DUMP_SECTION(gb, f, rtc       )) goto error;
    if (!DUMP_SECTION(gb, f, video     )) goto error;


    if (fwrite(gb->mbc_ram, 1, gb->mbc_ram_size, f) != gb->mbc_ram_size) {
        goto error;
    }

    if (fwrite(gb->ram, 1, gb->ram_size, f) != gb->ram_size) {
        goto error;
    }

    if (fwrite(gb->vram, 1, gb->vram_size, f) != gb->vram_size) {
        goto error;
    }

    errno = 0;

error:
    fclose(f);
    return errno;
}

/* Best-effort read function for maximum future compatibility. */
static bool read_section(FILE *f, void *dest, uint32_t size)
{
    uint32_t saved_size = 0;
    if (fread(&saved_size, 1, sizeof(size), f) != sizeof(size)) {
        return false;
    }

    if (saved_size <= size) {
        if (fread(dest, 1, saved_size, f) != saved_size) {
            return false;
        }
    }
    else {
        if (fread(dest, 1, size, f) != size) {
            return false;
        }
        fseek(f, saved_size - size, SEEK_CUR);
    }

    return true;
}

#define READ_SECTION(gb, f, section) read_section(f, GB_GET_SECTION(gb, section), GB_SECTION_SIZE(section))

int GB_load_state(GB_gameboy_t *gb, const char *path)
{
    GB_gameboy_t save;

    /* Every unread value should be kept the same. */
    memcpy(&save, gb, sizeof(save));

    FILE *f = fopen(path, "r");
    if (!f) {
        return errno;
    }

    if (fread(GB_GET_SECTION(&save, header), 1, GB_SECTION_SIZE(header), f) != GB_SECTION_SIZE(header)) goto error;
    if (!READ_SECTION(&save, f, core_state)) goto error;
    if (!READ_SECTION(&save, f, dma       )) goto error;
    if (!READ_SECTION(&save, f, mbc       )) goto error;
    if (!READ_SECTION(&save, f, hram      )) goto error;
    if (!READ_SECTION(&save, f, timing    )) goto error;
    if (!READ_SECTION(&save, f, apu       )) goto error;
    if (!READ_SECTION(&save, f, rtc       )) goto error;
    if (!READ_SECTION(&save, f, video     )) goto error;

    if (gb->magic != save.magic) {
        GB_log(gb, "File is not a save state, or is from an incompatible operating system.\n");
        errno = -1;
        goto error;
    }

    if (gb->version != save.version) {
        GB_log(gb, "Save state is for a different version of SameBoy.\n");
        errno = -1;
        goto error;
    }

    if (gb->mbc_ram_size != save.mbc_ram_size) {
        GB_log(gb, "Save state has non-matching MBC RAM size.\n");
        errno = -1;
        goto error;
    }

    if (gb->ram_size != save.ram_size) {
        GB_log(gb, "Save state has non-matching RAM size. Try changing emulated model.\n");
        errno = -1;
        goto error;
    }

    if (gb->vram_size != save.vram_size) {
        GB_log(gb, "Save state has non-matching VRAM size. Try changing emulated model.\n");
        errno = -1;
        goto error;
    }

    if (fread(gb->mbc_ram, 1, gb->mbc_ram_size, f) != gb->mbc_ram_size) {
        fclose(f);
        return EIO;
    }

    if (fread(gb->ram, 1, gb->ram_size, f) != gb->ram_size) {
        fclose(f);
        return EIO;
    }

    if (fread(gb->vram, 1, gb->vram_size, f) != gb->vram_size) {
        fclose(f);
        return EIO;
    }

    memcpy(gb, &save, sizeof(save));
    errno = 0;
error:
    fclose(f);
    return errno;
}

int GB_save_battery(GB_gameboy_t *gb, const char *path)
{
    if (!gb->cartridge_type->has_battery) return 0; // Nothing to save.
    if (gb->mbc_ram_size == 0 && !gb->cartridge_type->has_rtc) return 0; /* Claims to have battery, but has no RAM or RTC */
    FILE *f = fopen(path, "w");
    if (!f) {
        return errno;
    }

    if (fwrite(gb->mbc_ram, 1, gb->mbc_ram_size, f) != gb->mbc_ram_size) {
        fclose(f);
        return EIO;
    }
    if (gb->cartridge_type->has_rtc) {
        if (fwrite(&gb->rtc_real, 1, sizeof(gb->rtc_real), f) != sizeof(gb->rtc_real)) {
            fclose(f);
            return EIO;
        }

        if (fwrite(&gb->last_rtc_second, 1, sizeof(gb->last_rtc_second), f) != sizeof(gb->last_rtc_second)) {
            fclose(f);
            return EIO;
        }
    }

    errno = 0;
    fclose(f);
    return errno;
}

/* Loading will silently stop if the format is incomplete */
void GB_load_battery(GB_gameboy_t *gb, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }

    if (fread(gb->mbc_ram, 1, gb->mbc_ram_size, f) != gb->mbc_ram_size) {
        goto reset_rtc;
    }

    if (fread(&gb->rtc_real, 1, sizeof(gb->rtc_real), f) != sizeof(gb->rtc_real)) {
        goto reset_rtc;
    }

    if (fread(&gb->last_rtc_second, 1, sizeof(gb->last_rtc_second), f) != sizeof(gb->last_rtc_second)) {
        goto reset_rtc;
    }

    if (gb->last_rtc_second > time(NULL)) {
        /* We must reset RTC here, or it will not advance. */
        goto reset_rtc;
    }

    if (gb->last_rtc_second < 852076800) { /* 1/1/97. There weren't any RTC games that time,
                                            so if the value we read is lower it means it wasn't
                                            really RTC data. */
        goto reset_rtc;
    }
    goto exit;
reset_rtc:
    gb->last_rtc_second = time(NULL);
    gb->rtc_real.high |= 0x80; /* This gives the game a hint that the clock should be reset. */
exit:
    fclose(f);
    return;
}

void GB_run(GB_gameboy_t *gb)
{
    GB_update_joyp(gb);
    GB_debugger_run(gb);
    GB_cpu_run(gb);
}

void GB_set_pixels_output(GB_gameboy_t *gb, uint32_t *output)
{
    gb->screen = output;
}

void GB_set_vblank_callback(GB_gameboy_t *gb, GB_vblank_callback_t callback)
{
    gb->vblank_callback = callback;
}

void GB_set_log_callback(GB_gameboy_t *gb, GB_log_callback_t callback)
{
    gb->log_callback = callback;
}

void GB_set_input_callback(GB_gameboy_t *gb, GB_input_callback_t callback)
{
    if (gb->input_callback == default_input_callback) {
        gb->async_input_callback = NULL;
    }
    gb->input_callback = callback;
}

void GB_set_async_input_callback(GB_gameboy_t *gb, GB_input_callback_t callback)
{
    gb->async_input_callback = callback;
}

void GB_set_rgb_encode_callback(GB_gameboy_t *gb, GB_rgb_encode_callback_t callback)
{
    gb->rgb_encode_callback = callback;
}

void GB_set_infrared_callback(GB_gameboy_t *gb, GB_infrared_callback_t callback)
{
    gb->infrared_callback = callback;
}

void GB_set_infrared_input(GB_gameboy_t *gb, bool state)
{
    gb->infrared_input = state;
    gb->cycles_since_input_ir_change = 0;
    gb->ir_queue_length = 0;
}

void GB_queue_infrared_input(GB_gameboy_t *gb, bool state, long cycles_after_previous_change)
{
    if (gb->ir_queue_length == GB_MAX_IR_QUEUE) {
        GB_log(gb, "IR Queue is full\n");
        return;
    }
    gb->ir_queue[gb->ir_queue_length++] = (GB_ir_queue_item_t){state, cycles_after_previous_change};
}

void GB_set_sample_rate(GB_gameboy_t *gb, unsigned int sample_rate)
{
    if (gb->audio_buffer) {
        free(gb->audio_buffer);
    }
    gb->buffer_size = sample_rate / 25; // 40ms delay
    gb->audio_buffer = malloc((gb->buffer_size + 1) * sizeof(*gb->audio_buffer));
    gb->sample_rate = sample_rate;
    gb->audio_position = 0;
}
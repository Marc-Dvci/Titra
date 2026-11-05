/*
 * Serial line framing and the three frame types of docs/PROTOCOL.md.
 *
 * Every function here is pure: no allocation, no I/O, no global state. A frame is one ASCII line
 * "$<type>,<fields>*<checksum>" of at most TC_LINE_MAX bytes including "\r\n".
 */
#ifndef TITRA_FRAME_H
#define TITRA_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TC_LINE_MAX 80

/* A value absent from a frame ("--"). */
#define TC_ABSENT (-1)

/* Oximeter status bits (docs/PROTOCOL.md). */
#define TC_OX_SENSOR_OFF 0x01u
#define TC_OX_NO_SENSOR 0x02u
#define TC_OX_PULSE_SEARCH 0x04u
#define TC_OX_LOW_PERFUSION 0x08u
#define TC_OX_INTERFERENCE 0x10u
#define TC_OX_FLAGS_ALL 0x1Fu

/* Result codes shared by the parsers. */
typedef enum {
    TC_FRAME_OK = 0,
    TC_FRAME_EMPTY,       /* no bytes between '$' and '*' */
    TC_FRAME_NO_START,    /* the line does not begin with '$' */
    TC_FRAME_NO_CHECKSUM, /* no '*' followed by two hexadecimal digits at the end */
    TC_FRAME_CHECKSUM,    /* the checksum does not match */
    TC_FRAME_TYPE,        /* the frame type is not the one asked for */
    TC_FRAME_FIELDS,      /* too few or too many fields */
    TC_FRAME_VALUE,       /* a field is malformed or out of range */
    TC_FRAME_TOO_LONG     /* the line exceeds TC_LINE_MAX */
} tc_frame_result_t;

/* Splits a byte stream into candidate lines. A line starts at '$' and ends at '\n'; bytes before a '$' are
 * dropped, a '$' inside a line restarts it, and a line longer than TC_LINE_MAX is dropped whole. */
typedef struct {
    char buf[TC_LINE_MAX + 1];
    size_t len;
    int in_line;
    int overflow;
    uint32_t dropped_bytes;  /* bytes that never became part of a complete line */
    uint32_t dropped_lines;  /* lines dropped for length */
} tc_line_t;

void tc_line_init(tc_line_t *lb);

/* Feeds one byte. Returns 1 and sets *line and *len (no "\r\n", NUL-terminated) when a line is complete. */
int tc_line_feed(tc_line_t *lb, char c, const char **line, size_t *len);

/* XOR of every byte of s[0..n). */
unsigned tc_checksum(const char *s, size_t n);

/* Oximeter frame. Absent values are TC_ABSENT. pi_centi is the perfusion index in hundredths of a percent. */
typedef struct {
    uint32_t clock;
    int spo2;
    int pr;
    int pi_centi;
    unsigned flags;
} tc_ox_frame_t;

/* Blender command from the driver. FiO2 values are per mille. */
typedef struct {
    unsigned seq;
    int fio2_pm;
    int backup_pm;
    int alarm;
} tc_bl_frame_t;

/* Blender status. */
typedef struct {
    unsigned seq;
    int fio2_pm;
    char state; /* 'N', 'H' or 'F' */
} tc_bs_frame_t;

/* Parse a line without its "\r\n". */
tc_frame_result_t tc_ox_parse(const char *line, size_t len, tc_ox_frame_t *out);
tc_frame_result_t tc_bl_parse(const char *line, size_t len, tc_bl_frame_t *out);
tc_frame_result_t tc_bs_parse(const char *line, size_t len, tc_bs_frame_t *out);

/* Format a frame with its checksum and "\r\n". Returns the length written, or 0 if a value is out of range or
 * the buffer is too small. */
size_t tc_ox_format(char *buf, size_t cap, const tc_ox_frame_t *f);
size_t tc_bl_format(char *buf, size_t cap, const tc_bl_frame_t *f);
size_t tc_bs_format(char *buf, size_t cap, const tc_bs_frame_t *f);

const char *tc_frame_result_name(tc_frame_result_t r);

#ifdef __cplusplus
}
#endif

#endif

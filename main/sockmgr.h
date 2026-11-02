#pragma once
































#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>














#ifndef CONFIG_SOCKMGR_MAX_TRANSIENT
#define CONFIG_SOCKMGR_MAX_TRANSIENT 1
#endif


#ifndef CONFIG_SOCKMGR_WAIT_MS
#define CONFIG_SOCKMGR_WAIT_MS 1500
#endif




bool sockmgr_take(uint32_t timeout_ms);


void sockmgr_give(void);


int sockmgr_free(void);


int sockmgr_in_use(void);


uint32_t sockmgr_enfile_hits(void);
void sockmgr_note_enfile(void);


void sockmgr_format_status(char *buf, size_t len);

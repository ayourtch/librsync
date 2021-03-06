/*= -*- c-basic-offset: 4; indent-tabs-mode: nil; -*-
 *
 * librsync -- the library for network deltas
 * $Id$
 * 
 * Copyright (C) 2000, 2001 by Martin Pool <mbp@samba.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

                              /*
                               | Pick a window, Jimmy, you're leaving.
                               |   -- Martin Schwenke, regularly
                               */


/*
 * buf.c -- Buffers that map between stdio file streams and librsync
 * streams.  As the stream consumes input and produces output, it is
 * refilled from appropriate input and output FILEs.  A dynamically
 * allocated buffer of configurable size is used as an intermediary.
 *
 * TODO: Perhaps be more efficient by filling the buffer on every call
 * even if not yet completely empty.  Check that it's really our
 * buffer, and shuffle remaining data down to the front.
 *
 * TODO: Perhaps expose a routine for shuffling the buffers.
 */


#include <config.h>
#include <sys/types.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "librsync.h"
#include "trace.h"
#include "buf.h"
#include "util.h"

/* use fseeko instead of fseek for long file support if we have it */
#ifdef HAVE_FSEEKO
#define fseek fseeko
#endif

/**
 * File IO buffer sizes.
 */
int rs_inbuflen = 16000, rs_outbuflen = 16000;


struct rs_filebuf {
        FILE *f;
        char            *buf;
        size_t          buf_len;
};


rs_filebuf_t *rs_filebuf_new(FILE *f, size_t buf_len) 
{
    rs_filebuf_t *pf = rs_alloc_struct(rs_filebuf_t);

    pf->buf = rs_alloc(buf_len, "file buffer");
    pf->buf_len = buf_len;
    pf->f = f;

    return pf;
}


void rs_filebuf_free(rs_filebuf_t *fb) 
{
	free(fb->buf);
        rs_bzero(fb, sizeof *fb);
        free(fb);
}


/*
 * If the stream has no more data available, read some from F into
 * BUF, and let the stream use that.  On return, SEEN_EOF is true if
 * the end of file has passed into the stream.
 */
rs_result rs_infilebuf_fill(rs_job_t *job, rs_buffers_t *buf,
                            void *opaque)
{
    int                     len;
    rs_filebuf_t            *fb = (rs_filebuf_t *) opaque;
    FILE                    *f = fb->f;
        
    /* This is only allowed if either the buf has no input buffer
     * yet, or that buffer could possibly be BUF. */
    if (buf->next_in != NULL) {
        assert(buf->avail_in <= fb->buf_len);
        assert(buf->next_in >= fb->buf);
        assert(buf->next_in <= fb->buf + fb->buf_len);
    } else {
        assert(buf->avail_in == 0);
    }

    if (buf->eof_in || (buf->eof_in = feof(f))) {
        rs_trace("seen end of file on input");
        buf->eof_in = 1;
        return RS_DONE;
    }

    if (buf->avail_in)
        /* Still some data remaining.  Perhaps we should read
           anyhow? */
        return RS_DONE;
        
    len = fread(fb->buf, 1, fb->buf_len, f);
    if (len <= 0) {
        /* This will happen if file size is a multiple of input block len
         */
        if (feof(f)) {
            rs_trace("seen end of file on input");
            buf->eof_in = 1;
            return RS_DONE;
        }
        if (ferror(f)) {
            rs_error("error filling buf from file: %s",
                     strerror(errno));
            return RS_IO_ERROR;
        } else {
            rs_error("no error bit, but got %d return when trying to read",
                     len);
            return RS_IO_ERROR;
        }
    }
    buf->avail_in = len;
    buf->next_in = fb->buf;

    return RS_DONE;
}


int try_making_a_hole(FILE *f, int hole_size, char *zero_buffer) {
  /* compilation will fail here if off_t is too short */
  char assert_off_t_size[(sizeof(off_t) == sizeof(long long))-1] __attribute((unused));

  off_t cur = ftello(f);
  off_t size = 0;
  int ret = 0;

  size = fseeko(f, 0, SEEK_END);
  fseeko(f, cur, SEEK_SET);

  if(cur > size) {
    /* We are already past the end of file - just advance */
    if(0 == fseeko(f, hole_size, SEEK_CUR)) {
      ret = hole_size;
    }
  } else {
    if(cur + hole_size > size) {
      /* 
       * Part of the hole we are trying to build is 
       * within the file, part is outside the file.
       * This means we can get rid of the extra data 
       * truncating the file to the current position,
       * then seeking past the end of file to create a hole.
       */
      if( (0 == ftruncate(fileno(f), cur)) &&
          (0 == fseeko(f, hole_size, SEEK_CUR)) ) {
        ret = hole_size;
      }
    } else {
      /* 
       * Both the beginning and the end of the hole are
       * within the existing file... we can only write zeroes.
       */
      ret = fwrite(zero_buffer, 1, hole_size, f);
    }
  }
  return ret;
}

int write_with_holes(rs_filebuf_t *fb, int present, FILE *f) {
  char *pc = fb->buf;
  char *pc_start = NULL;
  int seek = 0;
  int count = 0;
  int ret = 0;
  while(present > 0) {
    seek = 0;
    pc_start = pc;
    while((0 == *pc) && (present > 0)) {
      pc++;
      present--;
      seek++;
    }
    if(seek > 0) {
      ret += try_making_a_hole(f, seek, pc_start);
    }
    count = 0;
    pc_start = pc;
    while((*pc) && (present > 0)) {
      count++;
      pc++;
      present--;
    }
    if(count > 0) {
      ret += fwrite(pc_start, 1, count, f);
    }
  }
  return ret;
}

/*
 * The buf is already using BUF for an output buffer, and probably
 * contains some buffered output now.  Write this out to F, and reset
 * the buffer cursor.
 */
rs_result rs_outfilebuf_drain(rs_job_t *job, rs_buffers_t *buf, void *opaque)
{
    int present;
    rs_filebuf_t *fb = (rs_filebuf_t *) opaque;
    FILE *f = fb->f;

    /* This is only allowed if either the buf has no output buffer
     * yet, or that buffer could possibly be BUF. */
    if (buf->next_out == NULL) {
        assert(buf->avail_out == 0);
                
        buf->next_out = fb->buf;
        buf->avail_out = fb->buf_len;
                
        return RS_DONE;
    }
        
    assert(buf->avail_out <= fb->buf_len);
    assert(buf->next_out >= fb->buf);
    assert(buf->next_out <= fb->buf + fb->buf_len);

    present = buf->next_out - fb->buf;
    if (present > 0) {
        int result;
                
        assert(present > 0);

        result = write_with_holes(fb, present, f);
        if (present != result) {
            rs_error("error draining buf to file: %s",
                     strerror(errno));
            return RS_IO_ERROR;
        }

        buf->next_out = fb->buf;
        buf->avail_out = fb->buf_len;
    }
        
    return RS_DONE;
}

/* a file seek that will flush the unfinished holes */

int rs_file_seek(FILE *f, off_t pos) {
    off_t cur;
    off_t end;
    char nul = 0;

    cur = ftello(f);
    if (fseek(f, 0, SEEK_END)) { 
      return -1; 
    }
    end = ftello(f);

    if ((end < cur) && (pos < cur)) {
      if(fseeko(f, cur-1, SEEK_SET)) {
        return -1;
      }
      if(1 != fwrite(&nul, 1, 1, f)) {
        return -1;
      }
    }
    return fseek(f, pos, SEEK_SET);
}


/**
 * Default copy implementation that retrieves a part of a stdio file.
 */
rs_result rs_file_copy_cb(void *arg, rs_long_t pos, size_t *len, void **buf)
{
    int        got;
    FILE       *f = (FILE *) arg;

    if (rs_file_seek(f, pos)) {
        rs_log(RS_LOG_ERR, "seek failed: %s", strerror(errno));
        return RS_IO_ERROR;
    }

    got = fread(*buf, 1, *len, f);
    if (got == -1) {
        rs_error(strerror(errno));
        return RS_IO_ERROR;
    } else if (got == 0) {
        rs_error("unexpected eof on fd%d", fileno(f));
        return RS_INPUT_ENDED;
    } else {
        *len = got;
        return RS_DONE;
    }
}

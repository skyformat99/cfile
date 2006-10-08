/*
 * cfile.c
 * This file is part of The PaulWay Libraries
 *
 * Copyright (C) 2006 - Paul Wayper (paulway@mabula.net)
 *
 * The PaulWay Libraries are free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * The PaulWay Libraries are distributed in the hope that they will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The PaulWay Libraries; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, 
 * Boston, MA  02110-1301  USA
 */
 
/* for cfopen/cfclose/cfprintf */
#include <zlib.h>
#include <bzlib.h>
#include <stdarg.h>
#include <string.h>
#include <talloc.h>
/*#include <errno.h>*/
#include <sys/types.h>
#include <sys/stat.h>

#include "cfile.h"

void *pwlib_context = NULL;

typedef enum filetype_enum { UNCOMPRESSED, GZIPPED, BZIPPED } CFile_type;

struct cfile_struct {
    char *name;
    CFile_type filetype;
    union {
        gzFile *gp;
        FILE *fp;
        BZFILE *bp;
    } fileptr;
};

static int _cf_destroyclose(void *void_fp) {
    CFile *fp = void_fp;
     /* Success = 0, failure = -1 here */
    if (!fp) return 0; /* If we're given null, then assume success */
/*    fprintf(stderr, "_cf_destroyclose(%p)\n", fp);*/
    if (fp->filetype == GZIPPED) {
        return gzclose(fp->fileptr.gp);
        /* I'm not sure of what zlib's error code is here, but one hopes
         * that it's compatible with fclose */
    } else if (fp->filetype == BZIPPED) {
        /* bzclose doesn't return anything - make something up. */
        BZ2_bzclose(fp->fileptr.bp);
        return 0;
    } else {
        return fclose(fp->fileptr.fp);
    }
    /* I'm not sure what should happen if the file couldn't be closed
     * here - the above application and talloc should handle it... */
}

static void finalise_open(CFile *fp) {
    talloc_set_destructor(fp, _cf_destroyclose);
}

static CFile_type file_extension_type(const char *name) {
    if        (strstr(name,".gz" ) != NULL) {
        return GZIPPED;
    } else if (strstr(name,".bz2") != NULL) {
        return BZIPPED;
    } else {
        return UNCOMPRESSED;
    }
}

CFile *cfopen(const char *name, const char *mode) {
    if (!pwlib_context) {
        pwlib_context = talloc_init("PWLib Talloc context");
    }
    CFile *fp = talloc_zero(pwlib_context, CFile);
    if (! fp) {
        fprintf(stderr,
            "Error: no memory for new file handle opening '%s'\n",
            name
        );
        return NULL;
    }
    talloc_set_name(fp, "CFile '%s' (mode '%s')", name, mode);
    fp->name = talloc_strdup(fp, name);
    /* If we have a '-' as a file name, dup stdin or stdout */
    if (strcmp(name, "-") == 0) {
        fp->filetype = UNCOMPRESSED;
        if (strstr(mode, "w") != 0) {
            fp->fileptr.fp = fdopen(1, mode);
        } else if (strstr(mode, "r") != 0) {
            fp->fileptr.fp = fdopen(0, mode);
        } else {
            fprintf(stderr,
                "Error: Can't open - with mode %s!\n", mode
            );
            fp->fileptr.fp = NULL;
        }
        if (! fp->fileptr.fp) {
            talloc_free(fp);
            return NULL;
        }
        finalise_open(fp);
        return fp;
    }
    /* At some stage we should really replace this with a better test,
     * Maybe one based on magic numbers */
#ifdef MAGIC_NONE
    /* We can only determine the file type if it exists - i.e. is being
     * read. */
    if (strstr(mode, "r") != NULL) {
        magic_t checker = magic_open(MAGIC_NONE);
        if (! checker) {
            /* Give up and go back to file extension */
            fp->filetype = file_extension_type(name);
        } else {
            char *type = magic_file(checker, name);
            if (strstr(type, "gzip compressed data") != NULL) {
                fp->filetype = GZIPPED;
            } else if (strstr(type, "bzip2 compressed data") != NULL) {
                fp->filetype = BZIPPED;
            } else {
                fp->filetype = UNCOMPRESSED;
            }
            magic_close(checker);
        }
    } else {
        fp->filetype = file_extension_type(name);
    }
#else
    fp->filetype = file_extension_type(name);
#endif
    /* Even though zlib allows reading of uncompressed files, let's
     * not complicate things too much at this stage :-) */
    if (fp->filetype == GZIPPED) {
        /* Should we do something about specifying a compression level? */
        fp->fileptr.gp = gzopen(name, mode);
    } else if (fp->filetype == BZIPPED) {
        fp->fileptr.bp = BZ2_bzopen(name, mode);
    } else {
        fp->fileptr.fp = fopen(name, mode);
    }
    if (!(fp->fileptr.fp)) {
        talloc_free(fp);
        return NULL;
    }
    finalise_open(fp);
    return fp;
}

CFile *cfdopen(int filedesc, const char *mode) {
    if (!pwlib_context) {
        pwlib_context = talloc_init("PWLib Talloc context");
    }
    CFile *fp = talloc_zero(pwlib_context, CFile);
    fp->filetype = UNCOMPRESSED;
    fp->fileptr.fp = fdopen(filedesc, mode);
    if (!(fp->fileptr.fp)) {
        talloc_free(fp);
        return NULL;
    }
    finalise_open(fp);
    return fp;
}

/* Returns the _uncompressed_ file size */

off_t cfsize(CFile *fp) {
    if (!fp) return 0;
    if (fp->filetype == GZIPPED) {
        FILE *rawfp = fopen(fp->name,"rb"); /* open the compressed file directly */
        if (!rawfp) {
            return 0;
        }
        fseek(rawfp,-4,2);
        int size; /* Make sure this is a 32-bit int! */
        fread(&size,4,1,rawfp);
        fclose(rawfp);
        return (off_t)size;
    } else if (fp->filetype == BZIPPED) {
        /* There's no file size information in the file.  We'd have to
         * bunzip it entirely, which I don't want to do.  Return zero
         * at the moment - caveat caller */
        return 0;
    } else {
        struct stat sp;
        if (stat(fp->name, &sp) == 0) {
            return sp.st_size;
        } else {
            return 0;
        }
    }
}

int cfeof(CFile *fp) {
    if (!fp) return 0;
    if (fp->filetype == GZIPPED) {
        return gzeof(fp->fileptr.gp);
    } else if (fp->filetype == BZIPPED) {
        return 0;
    } else {
        return feof(fp->fileptr.fp);
    }
}

char *cfgets(CFile *fp, char *str, int len) {
    if (!fp) return 0;
    if (fp->filetype == GZIPPED) {
        return gzgets(fp->fileptr.gp, str, len);
    } else if (fp->filetype == BZIPPED) {
        fprintf(stderr, "cfgets on bzip2 not implemented!\n");
        return NULL;
    } else {
        return fgets(str, len, fp->fileptr.fp);
    }
}

#define isafullline(line,len) ((line)[(len-1)] == '\n' || (line)[(len-1)] == '\r')
char *cfgetline(CFile *fp, char *line, int *maxline) {
    /* Get a line from the file into the buffer which has been preallocated
     * to maxline.  If the line from the file is too big to fit, we extend
     * the buffer and increase maxline.
     * If you pass a NULL pointer, it will allocate memory for you initially
     * (although against the pwlib's context rather than your own).
     * If maxline is zero, we'll reset it to something reasonable.  (These
     * two options allow you to start with a blank slate and let cfgetline
     * do all the work.
     * If you pass a negative maxline, it'll assume that the absolute value
     * is the size you want but will shrink the allocated memory down to the
     * minimum required to store the line afterward.
     * Otherwise, maxline is assumed to be the length of your string.  You'd
     * better have this right... :-)
     */
    /* Check for the 'shrink' option */
    char shrink = (*maxline < 0);
    if (shrink) {
        *maxline *= -1;
    }
    /* Check for a zero maxline and reset it */
    if (*maxline == 0) {
        *maxline = 80;
    }
    /* Allocate the string if it isn't already */
    if (NULL == line) {
        line = talloc_array(fp, char, *maxline);
    }
    /* Get the line thus far */
    cfgets(fp, line, *maxline);
    unsigned len = strlen(line);
    unsigned extend = 0;
    while (!cfeof(fp) && !isafullline(line,len)) {
        /* Add on how much we want to extend by */
        extend = len / 2;
        *maxline += extend;
        /* talloc_realloc automagically knows which context to use here :-) */
        line = talloc_realloc(fp, line, char, *maxline);
        /* Get more line */
        cfgets(fp, line + len, extend);
        /* And set our line length */
        len = strlen(line);
    }
    /* If we've been asked to shrink, do so */
    if (shrink) {
        *maxline = len + 1;
        line = talloc_realloc(fp, line, char, *maxline);
    }
    return line;
}



/* the writing function, however, has to know what type of file it's writing to */

int cfprintf(CFile *fp, const char *fmt, ...) {
    if (!fp) return 0;
    va_list ap;
    va_start(ap, fmt);
    int rtn;
    if (fp->filetype == GZIPPED) {
        char *buf = talloc_vasprintf(fp, fmt, ap);
        rtn = gzprintf(fp->fileptr.gp, "%s", buf);
        talloc_free(buf);
    } else if (fp->filetype == BZIPPED) {
        char *buf = talloc_vasprintf(fp, fmt, ap);
        rtn = BZ2_bzwrite(fp->fileptr.bp, buf, strlen(buf));
        talloc_free(buf);
    } else {
        rtn = vfprintf(fp->fileptr.fp, fmt, ap);
    }
    va_end(ap);
    return rtn;
}

int cfread(CFile *fp, void *ptr, size_t size, size_t num) {
    if (!fp) return 0;
    int rtn;
    if (fp->filetype == GZIPPED) {
        rtn = gzread(fp->fileptr.gp, ptr, size * num);
    } else if (fp->filetype == BZIPPED) {
        rtn = BZ2_bzread(fp->fileptr.bp, ptr, size * num);
    } else {
        rtn = fread(ptr, size, num, fp->fileptr.fp);
    }
    return rtn;
}

int cfwrite(CFile *fp, const void *ptr, size_t size, size_t num) {
    if (!fp) return 0;
    int rtn;
    if (fp->filetype == GZIPPED) {
        rtn = gzwrite(fp->fileptr.gp, ptr, size * num);
    } else if (fp->filetype == BZIPPED) {
        rtn = BZ2_bzwrite(fp->fileptr.bp, ptr, size * num);
    } else {
        rtn = fwrite(ptr, size, num, fp->fileptr.fp);
    }
    return rtn;
}

int cfclose(CFile *fp) {
    if (!fp) return 0;
    /* Now, according to theory, the talloc destructor should close the
     * file correctly and pass back it's return code */
    return talloc_free(fp);
}

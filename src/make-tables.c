/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

static void fail(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
};

static void make_ycbcr_tables(FILE *f) {
  // R
  fprintf(f, "const int16_t _openslide_R_Cr[256] = {");
  for (int i = 0; i < 256; i++) {
    if (!(i % 10)) {
      fprintf(f, "\n ");
    }
    fprintf(f, "%5d,", (int) round(1.402 * (i - 128)));
  }
  fprintf(f, "\n};\n\n");

  // G
  fprintf(f, "const int16_t _openslide_G_CbCr[256][256] = {");
  for (int i = 0; i < 256; i++) {
    fprintf(f, "\n  {");
    for (int j = 0; j < 256; j++) {
      if (!(j % 10)) {
        fprintf(f, "\n   ");
      }
      fprintf(f, "%5d,",
              (int) round(-0.34414 * (i - 128) - 0.71414 * (j - 128)));
    }
    fprintf(f, "\n  },");
  }
  fprintf(f, "\n};\n\n");

  // B
  fprintf(f, "const int16_t _openslide_B_Cb[256] = {");
  for (int i = 0; i < 256; i++) {
    if (!(i % 10)) {
      fprintf(f, "\n ");
    }
    fprintf(f, "%5d,", (int) round(1.772 * (i - 128)));
  }
  fprintf(f, "\n};\n\n");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fail("Usage: %s <outfile>", argv[0]);
  }

  // open file
  FILE *f = fopen(argv[1], "w");
  if (!f) {
    fail("Couldn't create %s", argv[1]);
  }
  fprintf(f, "// Generated by make-tables.c\n\n");
  fprintf(f, "#include <stdint.h>\n");
  fprintf(f, "#include \"openslide-private.h\"\n\n");

  // write tables for YCbCr -> RGB conversion
  make_ycbcr_tables(f);

  // close
  fclose(f);
  return 0;
}
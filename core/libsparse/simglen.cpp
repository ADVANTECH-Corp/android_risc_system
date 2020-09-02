/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sparse/sparse.h>
#include "sparse_file.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

void usage() {
  fprintf(stderr, "Output the partition size of sparse image. Usage: simglen <sparse_image_files> \n");
}

int main(int argc, char* argv[]) {
  int in;
  int i;
  struct sparse_file* s;

  if (argc < 2) {
    usage();
    exit(-1);
  }

  if (strcmp(argv[1], "-") == 0) {
    in = STDIN_FILENO;
  } else {
    in = open(argv[1], O_RDONLY | O_BINARY);
    if (in < 0) {
      exit(-1);
    }
  }

  s = sparse_file_import(in, true, false);
  if (!s) {
    exit(-1);
  }
  fprintf(stdout, "%ld\n", s->len);

  sparse_file_destroy(s);
  close(in);

  exit(0);
}

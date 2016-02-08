/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "exitcode.h"



/**
 * @file
 *
 * Everything we know about C filenames.
 *
 * We need to have some heuristics about input and output filenames to
 * understand command lines, because that's what cc does.
 *
 * @note As of 0.10, .s and .S files are never distributed, because
 * they might contain '.include' pseudo-operations, which are resolved
 * by the assembler.
 */



/**
 * Return a pointer to the extension, including the dot, or NULL.
 **/
 //返回文件的扩展名
 //golang filepath包可代替
char * dcc_find_extension(char *sfile)
{
    char *dot;

    dot = strrchr(sfile, '.');
    if (dot == NULL || dot[1] == '\0') {
        /* make sure there's space for one more character after the
         * dot */
        return NULL;
    }
    return dot;
}

/**
 * Return a pointer to the extension, including the dot, or NULL.
 * Same as dcc_find_extension(), but the argument and return
 * value are both pointers to const.
 **/
 //返回文件的扩展名, 常量版
const char * dcc_find_extension_const(const char *sfile) {
#if 0
  return dcc_find_extension((char *) sfile);
#else
  /* The following intermediate variable works around a bug in gcc 4.2.3 where
   * for the code above gcc spuriously reports "warning: passing argument 1
   * of 'dcc_find_extension' discards qualifiers from pointer target type",
   * despite the explicit cast. */
  char *sfile_nonconst = (char *)sfile;
  return dcc_find_extension(sfile_nonconst);
#endif
}


/**
 * Return a pointer to the basename of the file (everything after the
 * last slash.)  If there is no slash, return the whole filename,
 * which is presumably in the current directory.
 **/
 // 返回文件名最后一个斜杠之后的东西, 比如/etc/usr/gcc, 就返回gcc
 // 同样golang.file.filepath
const char * dcc_find_basename(const char *sfile)
{
    char *slash;

    if (!sfile)
        return sfile;

    slash = strrchr(sfile, '/');

    if (slash == NULL || slash[1] == '\0')
        return sfile;

    return slash+1;
}

/** Truncate the filename to its dirname (everything before the last slash).
 *  If the filename ends with a slash, just lop off the last slash.
 *  Note: this is destructive.
 */
 //截断文件名, 需要测试一下什么效果
void dcc_truncate_to_dirname(char *file)
{
    char *slash = 0;

    slash = strrchr(file, '/');

    if (slash == NULL) {
      file[0] = '\0';
    } else {
        *slash = '\0';
    }
}

//重新设置扩展名
static int dcc_set_file_extension(const char *sfile,
                                  const char *new_ext,
                                  char **ofile)
{
    char *dot, *o;

    o = strdup(sfile);
    if (!o) {
        rs_log_error("strdup failed (out of memory?)");
        return EXIT_DISTCC_FAILED;
    }
    dot = dcc_find_extension(o);
    if (!dot) {
        rs_log_error("couldn't find extension in \"%s\"", o);
        return EXIT_DISTCC_FAILED;
    }
    if (strlen(dot) < strlen(new_ext)) {
        rs_log_error("not enough space for new extension");
        return EXIT_DISTCC_FAILED;
    }
    strcpy(dot, new_ext);
    *ofile = o;

    return 0;
}


/*
 * Apple extensions:
 * file.mm, file.M
 * Objective-C++ source code which must be preprocessed. (APPLE ONLY)
 *
 * file.mii Objective-C++ source code which should not be
 * preprocessed. (APPLE ONLY)
 *
 * http://developer.apple.com/techpubs/macosx/DeveloperTools/gcc3/gcc/Overall-Options.html
 */



/**
 * If you preprocessed a file with extension @p e, what would you get?
 *
 * @param e original extension (e.g. ".c")
 *
 * @returns preprocessed extension, (e.g. ".i"), or NULL if
 * unrecognized.
 **/
 //将源文件的扩展名映射成预处理文件的扩展名
const char * dcc_preproc_exten(const char *e)
{
    if (e[0] != '.')
        return NULL;
    e++;
    if (!strcmp(e, "i") || !strcmp(e, "c")) {
        return ".i";
    //两个c是怎么回事?
    } else if (!strcmp(e, "c") || !strcmp(e, "cc")
               || !strcmp(e, "cpp") || !strcmp(e, "cxx")
               || !strcmp(e, "cp") || !strcmp(e, "c++")
               || !strcmp(e, "C") || !strcmp(e, "ii")) {
        return ".ii";
    //后面这些扩展名都不认识啊
    } else if(!strcmp(e,"mi") || !strcmp(e, "m")) {
        return ".mi";
    
    } else if(!strcmp(e,"mii") || !strcmp(e,"mm")
                || !strcmp(e,"M")) {
        return ".mii";
    
    } else if (!strcasecmp(e, "s")) {
        return ".s";
    
    } else {
        return NULL;
    }
}


/**
 * Does the extension of this file indicate that it is already
 * preprocessed?
 **/
 //相当于判断文件是否经过预处理, 其实是判定文件的扩展名, 如果是预处理文件的
 //扩展名, 就当做是预处理了的, 否则不是
int dcc_is_preprocessed(const char *sfile)
{
    const char *dot, *ext;
    dot = dcc_find_extension_const(sfile);
    if (!dot)
        return 0;
    ext = dot+1;

    switch (ext[0]) {
#ifdef ENABLE_REMOTE_ASSEMBLE//汇编需要特别的设置?
    case 's':
        /* .S needs to be run through cpp; .s does not *///这他么谁知道啊
        return !strcmp(ext, "s");
#endif
    case 'i':
        return !strcmp(ext, "i")
            || !strcmp(ext, "ii");
    case 'm':
        return !strcmp(ext, "mi")
            || !strcmp(ext, "mii");
    default:
        return 0;
    }
}


/**
 * Work out whether @p sfile is source based on extension
 **/
 // 同理判断文件是不是源文件, 主要还是看扩展名
int dcc_is_source(const char *sfile)
{
    const char *dot, *ext;
    dot = dcc_find_extension_const(sfile);
    if (!dot)
        return 0;
    ext = dot+1;

    /* you could expand this out further into a RE-like set of case
     * statements, but i'm not sure it's that important. */
    //这里需要考虑一下性能和正则表达式的问题吗?
    //或者是. 我们不应该写死
    switch (ext[0]) {
    case 'i':
        return !strcmp(ext, "i")
            || !strcmp(ext, "ii");
    case 'c':
        return !strcmp(ext, "c")
            || !strcmp(ext, "cc")
            || !strcmp(ext, "cpp")
            || !strcmp(ext, "cxx")
            || !strcmp(ext, "cp")
            || !strcmp(ext, "c++");
    case 'C':
        return !strcmp(ext, "C");
    case 'm':
        return !strcmp(ext,"m")
            || !strcmp(ext,"mm")
            || !strcmp(ext,"mi")
            || !strcmp(ext,"mii");
    case 'M':
        return !strcmp(ext, "M");
#ifdef ENABLE_REMOTE_ASSEMBLE
    case 's':
        return !strcmp(ext, "s");
    case 'S':
        return !strcmp(ext, "S");
#endif
    default:
        return 0;
    }
}



/**
 * Decide whether @p filename is an object file, based on its
 * extension.
 **/
 // 判断某个文件是不是obj文件, 其实就是判断扩展名是不是.o
int dcc_is_object(const char *filename)
{
    const char *dot;
    dot = dcc_find_extension_const(filename);
    if (!dot)
        return 0;

    return !strcmp(dot, ".o");
}


/* Some files should always be built locally... */
// 有一些文件总是需要本地处理, 比如以conftest., tmp.conftest.开头的
// 这里是否需要来个配置文件, 然后设置各种pattern, 
int
dcc_source_needs_local(const char *filename)
{
    const char *p;

    p = dcc_find_basename(filename);

    if (str_startswith("conftest.", p) || str_startswith("tmp.conftest.", p)) {
        rs_trace("autoconf tests are run locally: %s", filename);
        return EXIT_DISTCC_FAILED;
    }

    return 0;
}



/**
 * Work out the default object file name the compiler would use if -o
 * was not specified.  We don't need to worry about "a.out" because
 * we've already determined that -c or -S was specified.
 *
 * However, the compiler does put the output file in the current
 * directory even if the source file is elsewhere, so we need to strip
 * off all leading directories.
 *
 * @param sfile Source filename.  Assumed to match one of the
 * recognized patterns, otherwise bad things might happen.
 **/
 //这里是改名吗, 需要测试一下
 //来自arg.c中的调用, 因为没有.o参数, 于是将xx.c自动匹配成xx.o什么的
int dcc_output_from_source(const char *sfile,
                           const char *out_extn,
                           char **ofile)
{
    char *slash;

    if ((slash = strrchr(sfile, '/')))
        sfile = slash+1;
    if (strlen(sfile) < 3) {
        rs_log_error("source file %s is bogus", sfile);
        return EXIT_DISTCC_FAILED;
    }

    return dcc_set_file_extension(sfile, out_extn, ofile);
}

/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool <mbp@samba.org>
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


                /* "I have a bone to pick, and a few to break." */

/**
 * @file
 *
 * Functions for understanding and manipulating argument vectors.
 *
 * The few options explicitly handled by the client are processed in its
 * main().  At the moment, this is just --help and --version, so this function
 * never has to worry about them.
 *
 * We recognize two basic forms "distcc gcc ..." and "distcc ...", with no
 * explicit compiler name.  This second one is used if you have a Makefile
 * that can't manage two-word values for $CC; eventually it might support
 * putting a link to distcc on your path as 'gcc'.  We call this second one an
 * implicit compiler.
 *
 * We need to distinguish the two by working out whether the first argument
 * "looks like" a compiler name or not.  I think the two cases in which we
 * should assume it's implicit are "distcc -c hello.c" (starts with a hypen),
 * and "distcc hello.c" (starts with a source filename.)
 *
 * In the case of implicit compilation "distcc --help" will always give you
 * distcc's help, not gcc's, and similarly for --version.  I don't see much
 * that we can do about that.
 *
 * @todo We don't need to run the full argument scanner on the server, only
 * something simple to recognize input and output files.  That would perhaps
 * make the function simpler, and also mean that if argument recognizer bugs
 * are fixed in the future, they only need to be fixed on the client, not on
 * the server.  An even better solution is to have the client tell the server
 * where to put the input and output files.
 *
 * @todo Perhaps make the argument parser driven by a data table.  (Would that
 * actually be clearer?)  Perhaps use regexps to recognize strings.
 *
 * @todo We could also detect options like "-x cpp-output" or "-x
 * assembler-with-cpp", because they should override language detection based
 * on extension.  I haven't seen anyone use them yet though.  In fact, since
 * we don't assemble remotely it is moot for the only reported case, the
 * Darwin C library.  We would also need to update the option when passing it
 * to the server.
 *
 * @todo Perhaps assume that assembly code will not use both #include and
 * .include, and therefore if we preprocess locally we can distribute the
 * compilation?  Assembling is so cheap that it's not necessarily worth
 * distributing.
 **/


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "exitcode.h"
#include "snprintf.h"

//append一个参数到argv
int dcc_argv_append(char **argv, char *toadd)
{
    int l = dcc_argv_len(argv);
    argv[l] = toadd;//这么随便就加真的大丈夫?
    argv[l+1] = NULL;           /* just make sure */
    return 0;
}

//这就是打个log而已
static void dcc_note_compiled(const char *input_file, const char *output_file)
{
    const char *input_base, *output_base;

    input_base = dcc_find_basename(input_file);
    output_base = dcc_find_basename(output_file);

    rs_log(RS_LOG_INFO|RS_LOG_NONAME,
           "compile from %s to %s", input_base, output_base);
}

/**
 * Parse arguments, extract ones we care about, and also work out
 * whether it will be possible to distribute this invocation remotely.
 *
解析参数, 提取我们需要的信息, 并分析是否可以分布式处理

 * This is a little hard because the cc argument rules are pretty complex, but
 * the function still ought to be simpler than it already is.
 *
 这有点复杂, 毕竟编译器参数很TM复杂, 整个函数应该可以写得更简洁一些
 * This code is called on both the client and the server, though they use the
 * results differently.
 *
 这个代码可能类client和server调用, 虽然他们的结果并不一样
 * This function makes a copy of the arguments, modified to ensure that
 * the arguments include '-o <filename>'.  This is returned in *ret_newargv.
 * The copy is dynamically allocated and the caller is responsible for
 * deallocating it.
 *
 这个函数会复制argv, 然后修改之, 使其确保具有'-o <filename>'. 这会ret_newargv参数返回
 释放内存应该交由调用者处理
 * @returns 0 if it's ok to distribute this compilation, or an error code.
 **/
 //也许会返回个错误号码什么的
 // 处理一些需要本地操作的选项, 看看有没有-o, 没有就尽量自动补全
int dcc_scan_args(char *argv[], char **input_file, char **output_file,
                  char ***ret_newargv)
//第一个参数是输入参数, 后面的可能都是输出参数
{
    int seen_opt_c = 0, seen_opt_s = 0;
    int i;
    char *a;
    int ret;

     /* allow for -o foo.o */
    if ((ret = dcc_copy_argv(argv, ret_newargv, 2)) != 0)
        return ret;
        //这里应该是参加两个参数位来房子-o和foo.o
    argv = *ret_newargv;

    /* FIXME: new copy of argv is leaked *///这里是怎么泄露的?
    //copy了然而返回错误所以没有释放?

    dcc_trace_argv("scanning arguments", argv);

    /* Things like "distcc -c hello.c" with an implied compiler are
     * handled earlier on by inserting a compiler name.  At this
     * point, argv[0] should always be a compiler name. */
     // distcc -c这种应该高就被处理了, 所以在这里应当是正确的编译器
    if (argv[0][0] == '-') {
        //第一个参数是个option说明有问题
        rs_log_error("unrecognized distcc option: %s", argv[0]);
        exit(EXIT_BAD_ARGUMENTS);
    }

    *input_file = *output_file = NULL;
    //先把这两置空
    //这个循环主要是寻找-o参数(文件名), -c, -S两个flag, 以及判断必须在本地进行的情况(各种选项)
    for (i = 0; (a = argv[i]); i++) {
        //然后每个option去研究一下
        if (a[0] == '-') {
            //如果是选项
            if (!strcmp(a, "-E")) {// -E是仅仅进行预处理阶段, 不往下编译
                rs_trace("-E call for cpp must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) {//-MD is equivalent to -M -MF file
                //这两应该输出依赖头文件, MMD是不输出系统依赖
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF") || !strcmp(a, "-MT") ||
                       !strcmp(a, "-MQ")) {
                /* As above but with extra argument. */
                i++;
            } else if (!strncmp(a, "-MF", 3) || !strncmp(a, "-MT", 3) ||
                       !strncmp(a, "-MQ", 3)) {
                /* As above, without extra argument. */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                    produce a list of make-style dependencies on
                    header files, either to stdout or to a local file.
                    It implies -E, so only the preprocessor is run,
                    not the compiler.  There would be no point trying
                    to distribute it even if we could. */
                    //仅仅是预处理的情况就别分布式了, 必须本地
                rs_trace("%s implies -E (maybe) and must be local", a);
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-march=native")) {
                //有这个不知道什么选项的也必须本地
                rs_trace("-march=native generates code for local machine; "
                         "must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-mtune=native")) {
                //这个也不知道怎么回事啊
                rs_trace("-mtune=native optimizes for local machine; "
                         "must be local");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-Wa,", a)) {
                /* Look for assembler options that would produce output
                 * files and must be local.
                 *
                 * Writing listings to stdout could be supported but it might
                 * be hard to parse reliably. */
                if (strstr(a, ",-a") || strstr(a, "--MD")) {
                    rs_trace("%s must be local", a);
                    return EXIT_DISTCC_FAILED;
                    //这我已经不知道怎么回事
                }
            } else if (str_startswith("-specs=", a)) {
                rs_trace("%s must be local", a);
                //这个也不知道
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-S")) {
                //这应该是汇编的flag
                seen_opt_s = 1;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")
		       || !strcmp(a, "--coverage")) {
                rs_log_info("compiler will emit profile info; must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-frepo")) {
                rs_log_info("compiler will emit .rpo files; must be local");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-x", a)) {
                rs_log_info("gcc's -x handling is complex; running locally");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-dr", a)) {
                rs_log_info("gcc's debug option %s may write extra files; "
                            "running locally", a);
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-c")) {
                seen_opt_c = 1;
                //这是编译的flag
            } else if (!strcmp(a, "-o")) {
                /* Whatever follows must be the output */
                a = argv[++i];
                //找到了-o就直接跳去GOT_OUTPUT, 但是GOT_OUTPUT还在循环内
                goto GOT_OUTPUT;
            } else if (str_startswith("-o", a)) {
                a += 2;         /* skip "-o" */
                goto GOT_OUTPUT;
            }
        } else {//这个是 不以"-"开头的, 也许是源文件, 
            if (dcc_is_source(a)) {
                rs_trace("found input file \"%s\"", a);
                if (*input_file) {
                    //输入两个源文件就不work了
                    rs_log_info("do we have two inputs?  i give up");
                    return EXIT_DISTCC_FAILED;
                }
                *input_file = a;
            } else if (str_endswith(".o", a)) {
              GOT_OUTPUT:
                rs_trace("found object/output file \"%s\"", a);
                if (*output_file) {
                    rs_log_info("called for link?  i give up");
                    return EXIT_DISTCC_FAILED;
                }
                *output_file = a;
                //先把output_file赋值了, 然后因为刚刚已经++过一次, 所以循环体会跳过这个文件名
            }
        }
    }

    /* TODO: ccache has the heuristic of ignoring arguments that are not
     * extant files when looking for the input file; that's possibly
     * worthwile.  Of course we can't do that on the server. */
     //ccache有个科学的方法, 要不要参考一下?

    if (!seen_opt_c && !seen_opt_s) {
        //既没有-c, 也没有-S, 说明不是编译, 所以就直接返回
        rs_log_info("compiler apparently called not for compile");
        return EXIT_DISTCC_FAILED;
    }

    if (!*input_file) {
        //如果没有找到要编译的源文件, 也放弃
        rs_log_info("no visible input file");
        return EXIT_DISTCC_FAILED;
    }

    if (dcc_source_needs_local(*input_file))//一些特殊的文件是需要本地处理的
        return EXIT_DISTCC_FAILED;

    if (!*output_file) {
        /* This is a commandline like "gcc -c hello.c".  They want
         * hello.o, but they don't say so.  For example, the Ethereal
         * makefile does this.
         *
         * Note: this doesn't handle a.out, the other implied
         * filename, but that doesn't matter because it would already
         * be excluded by not having -c or -S.
         */
        char *ofile;

        /* -S takes precedence over -c, because it means "stop after
         * preprocessing" rather than "stop after compilation." */
        //-S的优先级更大, 因为这意味着预处理之后就停止, 而不是编译后停止
        if (seen_opt_s) {
            //自动补输出文件
            if (dcc_output_from_source(*input_file, ".s", &ofile))
                return EXIT_DISTCC_FAILED;
        } else if (seen_opt_c) {
            if (dcc_output_from_source(*input_file, ".o", &ofile))
                return EXIT_DISTCC_FAILED;
        } else {
            //这里不应该发生
            rs_log_crit("this can't be happening(%d)!", __LINE__);
            return EXIT_DISTCC_FAILED;
        }
        rs_log_info("no visible output file, going to add \"-o %s\" at end",
                      ofile);
        //把输出文件添加到argv尾部
        dcc_argv_append(argv, strdup("-o"));
        dcc_argv_append(argv, ofile);
        *output_file = ofile;
    }//这个endif是判断是否有output_file的, 没有就根据源文件名添加一个

    dcc_note_compiled(*input_file, *output_file);

    if (strcmp(*output_file, "-") == 0) {
        /* Different compilers may treat "-o -" as either "write to
         * stdout", or "write to a file called '-'".  We can't know,
         * so we just always run it locally.  Hopefully this is a
         * pretty rare case. */
         //不同的编译器对" -o -"的处理不同, 所以还是本地处理好了
        rs_log_info("output to stdout?  running locally");
        return EXIT_DISTCC_FAILED;
    }

    return 0;
}



/**
 * Used to change "-c" or "-S" to "-E", so that we get preprocessed
 * source.
 **/
int dcc_set_action_opt(char **a, const char *new_c)
{
    int gotone = 0;

    for (; *a; a++)
        if (!strcmp(*a, "-c") || !strcmp(*a, "-S")) {
            *a = strdup(new_c);
            if (*a == NULL) {
                rs_log_error("strdup failed");
                exit(EXIT_OUT_OF_MEMORY);
            }
            gotone = 1;
            /* keep going; it's not impossible they wrote "gcc -c -c
             * -c hello.c" */
        }

    if (!gotone) {
        rs_log_error("failed to find -c or -S");
        return EXIT_DISTCC_FAILED;
    } else {
        return 0;
    }
}



/**
 * Change object file or suffix of -o to @p ofname
 * Frees the old value, if it exists.
 *
 * It's crucially important that in every case where an output file is
 * detected by dcc_scan_args(), it's also correctly identified here.
 * It might be better to make the code shared.
 **/
int dcc_set_output(char **a, char *ofname)
{
    int i;

    for (i = 0; a[i]; i++)
        if (0 == strcmp(a[i], "-o") && a[i+1] != NULL) {
            rs_trace("changed output from \"%s\" to \"%s\"", a[i+1], ofname);
            free(a[i+1]);
            a[i+1] = strdup(ofname);
            if (a[i+1] == NULL) {
                rs_log_crit("failed to allocate space for output parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            dcc_trace_argv("command after", a);
            return 0;
        } else if (0 == strncmp(a[i], "-o", 2)) {
            char *newptr;
            rs_trace("changed output from \"%s\" to \"%s\"", a[i]+2, ofname);
            free(a[i]);
            if (asprintf(&newptr, "-o%s", ofname) == -1) {
                rs_log_crit("failed to allocate space for output parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            a[i] = newptr;
            dcc_trace_argv("command after", a);
            return 0;
        }

    rs_log_error("failed to find \"-o\"");
    return EXIT_DISTCC_FAILED;
}

/**
 * Change input file to a copy of @p ifname; called on compiler.
 * Frees the old value.
 *
 * @todo Unify this with dcc_scan_args
 *
 * @todo Test this by making sure that when the modified arguments are
 * run through scan_args, the new ifname is identified as the input.
 **/
int dcc_set_input(char **a, char *ifname)
{
    int i;

    for (i =0; a[i]; i++)
        if (dcc_is_source(a[i])) {
            rs_trace("changed input from \"%s\" to \"%s\"", a[i], ifname);
            free(a[i]);
            a[i] = strdup(ifname);
            if (a[i] == NULL) {
                rs_log_crit("failed to allocate space for input parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            dcc_trace_argv("command after", a);
            return 0;
        }

    rs_log_error("failed to find input file");
    return EXIT_DISTCC_FAILED;
}

/* Subroutine of dcc_expand_preprocessor_options().
 * Calculate how many extra arguments we'll need to convert
 * a "-Wp,..." option into regular gcc options.
 * Returns the number of extra arguments needed.
 */
 //直接将'-Wp,'开头的参数传到这里
static int count_extra_args(char *dash_Wp_option) {
    int extra_args = 0;
    char *comma = dash_Wp_option + strlen("-Wp");
    while (comma != NULL) {
        char *opt = comma + 1;//opt是逗号后一个位置
        comma = strchr(opt, ',');//更新逗号位置
        if (str_startswith("-MD,", opt) ||
            str_startswith("-MMD,", opt))//如果以'-MD'或'-MMD'开头, 
        {
            //那就在这个opt开始继续找逗号, 这种增加三个
            char *filename = comma + 1;
            comma = strchr(filename, ',');
            extra_args += 3;  /* "-MD", "-MF", filename. */
        } else {
            extra_args++;
            //其他情况就加1个
        }
    }
    return extra_args;
}

/* Subroutine of dcc_expand_preprocessor_options().
 * Convert a "-Wp,..." option into one or more regular gcc options.
 * Copy the resulting gcc options to dest_argv, which should be
 * pre-allocated by the caller.
 * Destructively modifies dash_Wp_option as it goes.
 * Returns 0 on success, nonzero for error (out of memory).
 */
 //"Wp,"选项是直接给预处理器传参数, 但是, 预处理器不应该给用户直接使用, 所以不建议使用
 //所以这里就把MD和MMD改成了MF
static int copy_extra_args(char **dest_argv, char *dash_Wp_option,
                           int extra_args) {
    int i = 0;
    char *comma = dash_Wp_option + strlen("-Wp");
    while (comma != NULL) {
        char *opt = comma + 1;
        comma = strchr(opt, ',');
        if (comma) *comma = '\0';
        dest_argv[i] = strdup(opt);//先把原始选项copy了
        if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
        i++;
        //再特别处理一下MD和MMD
        if (strcmp(opt, "-MD") == 0 || strcmp(opt, "-MMD") == 0) {
            char *filename;
            if (!comma) {
                rs_log_warning("'-Wp,-MD' or '-Wp,-MMD' option is missing "
                               "filename argument");
                break;
            }
            //带文件名的应该是这样的:    -Wp,-MD,FOO.d 
            //参考: https://llvm.org/svn/llvm-project/cfe/trunk/test/Driver/Wp-args.c
            //http://malihou2008.blog.163.com/blog/static/2118200452013102815732432/
            filename = comma + 1;
            comma = strchr(filename, ',');
            if (comma) *comma = '\0';
            dest_argv[i] = strdup("-MF");
            if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
            i++;
            dest_argv[i] = strdup(filename);
            if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
            i++;
            //综上是把MD, MMD改成MF
        }
    }
    assert(i == extra_args);
    return 0;
    /*
    r = []
    l = dest_argv.split(',')
    for i in l:
        r.append(i)
        if i=='-MD' or i =='-MMD':
            r.append('-MF',l[i+1])
            i++ #跳过filename
    return ' '.join(r)

    */
}


/*
 * Convert any "-Wp," options into regular gcc options.
 //把-Wp选项转换成正规形式
 * We do this because it simplifies the command-line
 * option handling elsewhere; 
// 我们这么做是为了简化命令以在别的地方方便处理
 this is the only place
 * that needs to parse "-Wp," options.
 // 这是唯一要解析'-Wp,'选项的地方
 * Returns 0 on success, nonzero for error (out of memory).
 *
 * The argv array pointed to by argv_ptr when this function
 * is called must have been dynamically allocated.  It remains
 * the caller's responsibility to deallocate it.
 */
 // Wp,是一种奇怪的预处理选项, 形如: gcc -Wp,-lang-c-c++-comments -c source.c
int dcc_expand_preprocessor_options(char ***argv_ptr) {
    /*
    for item,i in argv:
        if item.start_with('-Wp,'):
            argv.insert(i,extra_args(argv[i]))
            //注意这个选项的顺序不能改变
    */
    int i, j, ret;
    char **argv = *argv_ptr;
    char **new_argv;
    int argc = dcc_argv_len(argv);
    for (i = 0; argv[i]; i++) {
        if (str_startswith("-Wp,", argv[i])) {
            /* First, calculate how many extra arguments we'll need. */
            int extra_args = count_extra_args(argv[i]);
            assert(extra_args >= 1);

            new_argv = calloc(argc + extra_args, sizeof(char *));
            if (!new_argv) {
                return EXIT_OUT_OF_MEMORY;
            }
            for (j = 0; j < i; j++) {
                new_argv[j] = argv[j];
            }
            if ((ret = copy_extra_args(new_argv + i, argv[i],
                                       extra_args)) != 0) {
                free(new_argv);
                return ret;
            }
            for (j = i + 1; j <= argc; j++) {
                new_argv[j + extra_args - 1] = argv[j];
            }
            free(argv);
            *argv_ptr = argv = new_argv;
            i += extra_args - 1;
            argc += extra_args - 1;
        }
    }
    return 0;
}

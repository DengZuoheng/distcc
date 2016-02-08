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



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include <ctype.h>

#ifdef HAVE_FNMATCH_H
  #include <fnmatch.h>
#endif

#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <assert.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"
#include "hosts.h"
#include "bulk.h"
#include "implicit.h"
#include "exec.h"
#include "where.h"
#include "lock.h"
#include "timeval.h"
#include "compile.h"
#include "include_server_if.h"
#include "emaillog.h"
#include "dotd.h"

/**
 * This boolean is true iff --scan-includes option is enabled.
如果设置了 --scan-includes选项, 这个bool变量就设为true

 * If so, distcc will just run the source file through the include server,
 * and print out the list of header files that might be #included,
 * rather than actually compiling the sources.
 */
 // 如果设为true了, distcc就仅是在include server处理源文件, 然后输出可能包含的头文件,
 // 而不是真的去编译源文件.
int dcc_scan_includes = 0;

static const char *const include_server_port_suffix = "/socket";
static const char *const discrepancy_suffix = "/discrepancy_counter";

static int dcc_get_max_discrepancies_before_demotion(void)//demotion的意思是降级
{
    //获取一个从环境变量拿回来的值
    /* Warning: the default setting here should have the same value as in the
     * pump.in script! */
    static const int default_setting = 1;
    static int current_setting = 0;

    if (current_setting > 0)
        return current_setting;

    const char *user_setting = getenv("DISTCC_MAX_DISCREPANCY");
    if (user_setting) {
        int parsed_user_setting = atoi(user_setting);
        if (parsed_user_setting <= 0) {
            rs_log_error("Bad DISTCC_MAX_DISCREPANCY value: %s", user_setting);
            exit(EXIT_BAD_ARGUMENTS);
        }
        current_setting = parsed_user_setting;
    } else {
        current_setting = default_setting;
    }
    return current_setting;
}

/**
 * Return in @param filename the name of the file we use as unary counter of
 * discrepancies (a compilation failing on the server, but succeeding
 * locally. This function may return NULL in @param filename if the name cannot
 * be determined.
 filename参数是用来返回的, 应该只有一个filename, 这个是我们用于"一元运算计数器"
 用来计算差异(就是那种在远程服务器编译失败, 但是在本地成功的)
 函数有可能返回空
 **/
int dcc_discrepancy_filename(char **filename)
{
    const char *include_server_port = getenv("INCLUDE_SERVER_PORT");//这个环境变量不知道哪里来的
    *filename = NULL;
    if (include_server_port == NULL) {
        return 0;
    } else if (str_endswith(include_server_port_suffix,//普通的util函数
        //这个是全局变量, 就定义在上面
                            include_server_port)) {
        /* We're going to make a longer string from include_server_port: one
         * that replaces include_server_port_suffix with discrepancy_suffix. */
        int delta = strlen(discrepancy_suffix) -
            strlen(include_server_port_suffix);
        //这里要把前缀换掉
        assert (delta > 0);
        *filename = malloc(strlen(include_server_port) + 1 + delta);
        //然后开一段内存
        if (!*filename) {
            rs_log_error("failed to allocate space for filename");
            return EXIT_OUT_OF_MEMORY;
        }
        //然后复制过来
        strcpy(*filename, include_server_port);
        int slash_pos = strlen(include_server_port)
                        - strlen(include_server_port_suffix);
        /* Because include_server_port_suffix is a suffix of include_server_port
         * we expect to find a '/' at slash_pos in filename. */
        assert((*filename)[slash_pos] == '/');
        (void) strcpy(*filename + slash_pos, discrepancy_suffix);
        //然后替换前缀
        return 0;
        //实际上, 这里只是获取一个环境变量, 然后把前缀换掉, 就变成了文件名, 然后返回
    } else
        return 0;
}


/**
 * Return the length of the @param discrepancy_filename in newly allocated
 * memory; return 0 if it's not possible to determine the length (if
 * e.g. @param discrepancy_filename is NULL).
 **/
// 返回一个长度, 是那个什么文件的长度
static int dcc_read_number_discrepancies(const char *discrepancy_filename)
{
    if (!discrepancy_filename) return 0;
    struct stat stat_record;
    if (stat(discrepancy_filename, &stat_record) == 0) {
        size_t size = stat_record.st_size;
        /* Does size fit in an 'int'? */
        if ((((size_t) (int) size) == size) &&
            ((int) size) > 0)
            return ((int) size);
        else
            return INT_MAX;
    } else
        return 0;
}


/**
 *  Lengthen the file whose name is @param discrepancy_filename by one byte. Or,
 *  do nothing, if @param discrepancy_filename is NULL.
 **/
static int dcc_note_discrepancy(const char *discrepancy_filename)
{
    FILE *discrepancy_file;
    if (!discrepancy_filename) return 0;
    if (!(discrepancy_file = fopen(discrepancy_filename, "a"))) {
        rs_log_error("failed to open discrepancy_filename file: %s: %s",
                     discrepancy_filename,
                     strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (fputc('@', discrepancy_file) == EOF) {
        rs_log_error("failed to write to discrepancy_filename file: %s",
                     discrepancy_filename);
        fclose(discrepancy_file);
        return EXIT_IO_ERROR;
    }
    /* The file position is a property of the stream, so we are
    assured that exactly one process will take the 'if' branch when
    max_discrepancies_before_demotion failures is reached. */
    if (ftell(discrepancy_file) == 
        (long int)dcc_get_max_discrepancies_before_demotion()) {
        rs_log_warning("now using plain distcc, possibly due to "
                       "inconsistent file system changes during build");
    }
    fclose(discrepancy_file);
    return 0;
}


/**
 * In some cases, it is ill-advised to preprocess on the server. Check for such
 * situations. If they occur, then change protocol version.
 **/
static void dcc_perhaps_adjust_cpp_where_and_protover(
    char *input_fname,
    struct dcc_hostdef *host,
    char *discrepancy_filename)
{
    /* It's unfortunate that the variable that controls preprocessing is in the
       "host" datastructure. See elaborate complaint in dcc_build_somewhere. */
    //很遗憾决定在哪里预处理的设置是在host结构中. 请认真看dcc_build_somewhere
    //的各种抱怨

    /* Check whether there has been too much trouble running distcc-pump during
       this build. */
    // 检查这次build是否在跑distcc-pump过程中已经出现了太多的问题
    // 这个discrepancy_filename是从环境变量中get回来的,
    // max_discrepancies也是从环境变量中get回来的
    // dcc_read_number_dis...实现在上面
    if (dcc_read_number_discrepancies(discrepancy_filename) >=
        dcc_get_max_discrepancies_before_demotion()) {
        //如果discrepancies已经大于max_discrepancies
        // 就不使用distcc-pump了
        /* Give up on using distcc-pump */
        host->cpp_where = DCC_CPP_ON_CLIENT;//改成在本地预处理
        dcc_get_protover_from_features(host->compr,
                                       host->cpp_where,
                                       &host->protover);
    }

    /* Don't do anything silly for already preprocessed files. */
    if (dcc_is_preprocessed(input_fname)) {//已经预处理过的文件就别做傻事了
        /* Don't subject input file to include analysis. */
        rs_log_warning("cannot use distcc_pump on already preprocessed file"
                       " (such as emitted by ccache)");
        host->cpp_where = DCC_CPP_ON_CLIENT;
        // 相当于获取压不压缩, 在不在本地预处理的信息
        dcc_get_protover_from_features(host->compr,
                                       host->cpp_where,
                                       &host->protover);
    }
    /* Environment variables CPATH and two friends are hidden ways of passing
     * -I's. Beware! */
    if (getenv("CPATH") || getenv("C_INCLUDE_PATH")
        || getenv("CPLUS_INCLUDE_PATH")) {
        //如果各种path都拿不到, 就改成本地预处理
        rs_log_warning("cannot use distcc_pump with any of environment"
                       " variables CPATH, C_INCLUDE_PATH or CPLUS_INCLUDE_PATH"
                       " set, preprocessing locally");
        host->cpp_where = DCC_CPP_ON_CLIENT;
        dcc_get_protover_from_features(host->compr,
                                       host->cpp_where,
                                       &host->protover);
    }
}



/**
 * Do a time analysis of dependencies in dotd file.  First, if @param dotd_fname
 * is created before @param reference_time, then return NULL in result.  Second,
 * if one of the files mentioned in the @param dotd_fname is modified after time
 * @param reference_time, then return non-NULL in result. Otherwise return NULL
 * in result.  A non-NULL value in result is a pointer to a newly allocated
 * string describing the offending dependency.

 * If @param exclude_pattern is not NULL, then files matching the glob @param
 * exclude_pattern are not considered in the above comparison.
 *
 *  This function is not declared static --- for purposes of testing.
 **/
int dcc_fresh_dependency_exists(const char *dotd_fname,
                                const char *exclude_pattern,
                                time_t reference_time,
                                char **result)
{
    struct stat stat_dotd;
    off_t dotd_fname_size = 0;
    FILE *fp;
    int c;
    int res;
    char *dep_name;

    *result = NULL;
    /* Allocate buffer for dotd contents and open it. */
    res = stat(dotd_fname, &stat_dotd);
    if (res) {
        rs_trace("could not stat \"%s\": %s", dotd_fname, strerror(errno));
        return 0;
    }
    if (stat_dotd.st_mtime < reference_time) {
        /* That .d file appears to be too old; don't trust it for this
         * analysis. */
        rs_trace("old dotd file \"%s\"", dotd_fname);
        return 0;
    }
    dotd_fname_size = stat_dotd.st_size;
    /* Is dotd_fname_size representable as a size_t value ? */
    if ((off_t) (size_t) dotd_fname_size == dotd_fname_size) {
        dep_name = malloc((size_t) dotd_fname_size);
        if (!dep_name) {
            rs_log_error("failed to allocate space for dotd file");
            return EXIT_OUT_OF_MEMORY;
        }
    } else { /* This is exceedingly unlikely. */
        rs_trace("file \"%s\" is too big", dotd_fname);
        return 0;
    }
    if ((fp = fopen(dotd_fname, "r")) == NULL) {
        rs_trace("could not open \"%s\": %s", dotd_fname, strerror(errno));
        free(dep_name);
        return 0;
    }

    /* Find ':'. */
    while ((c = getc(fp)) != EOF && c != ':');
    if (c != ':') goto return_0;

    /* Process dependencies. */
    while (c != EOF) {
        struct stat stat_dep;
        int i = 0;
        /* Skip whitespaces and backslashes. */
        while ((c = getc(fp)) != EOF && (isspace(c) || c == '\\'));
        /* Now, we're at start of file name. */
        ungetc(c, fp);
        while ((c = getc(fp)) != EOF &&
               (!isspace(c) || c == '\\')) {
            if (i >= dotd_fname_size) {
                /* Impossible */
                rs_log_error("not enough room for dependency name");
                goto return_0;
            }
            if (c == '\\') {
                /* Skip the newline. */
                if ((c = getc(fp)) != EOF)
                    if (c != '\n') ungetc(c, fp);
            }
            else dep_name[i++] = c;
        }
        if (i != 0) {
            dep_name[i] = '\0';
#ifdef HAVE_FNMATCH_H
            if (exclude_pattern == NULL ||
                fnmatch(exclude_pattern, dep_name, 0) == FNM_NOMATCH) {
#else
            /* Tautology avoids compiler warning about unused variable. */
            if (exclude_pattern == exclude_pattern) {
#endif
                /* The dep_name is not excluded; now verify that it is not too
                 * young. */
                rs_log_info("Checking dependency: %s", dep_name);
                res = stat(dep_name, &stat_dep);
                if (res) goto return_0;
                if (stat_dep.st_ctime >= reference_time) {
                    fclose(fp);
                    *result = realloc(dep_name, strlen(dep_name) + 1);
                    if (*result == NULL) {
                        rs_log_error("realloc failed");
                        return EXIT_OUT_OF_MEMORY;
                    }
                    return 0;
                }
            }
        }
    }
  return_0:
    fclose(fp);
    free(dep_name);
    return 0;
}


/**
 * Invoke a compiler locally.  This is, obviously, the alternative to
 * dcc_compile_remote().
 *
 * The server does basically the same thing, but it doesn't call this
 * routine because it wants to overlap execution of the compiler with
 * copying the input from the network.
 *
 * This routine used to exec() the compiler in place of distcc.  That
 * is slightly more efficient, because it avoids the need to create,
 * schedule, etc another process.  The problem is that in that case we
 * can't clean up our temporary files, and (not so important) we can't
 * log our resource usage.
 *
 * This is called with a lock on localhost already held.
 **/
static int dcc_compile_local(char *argv[],
                             char *input_name)
{
    pid_t pid;
    int ret;
    int status;

    dcc_note_execution(dcc_hostdef_local, argv);
    dcc_note_state(DCC_PHASE_COMPILE, input_name, "localhost", DCC_LOCAL);

    /* We don't do any redirection of file descriptors when running locally,
     * so if for example cpp is being used in a pipeline we should be fine. */
    if ((ret = dcc_spawn_child(argv, &pid, NULL, NULL, NULL)) != 0)
        return ret;

    if ((ret = dcc_collect_child("cc", pid, &status, timeout_null_fd)))
        return ret;

    return dcc_critique_status(status, "compile", input_name,
                               dcc_hostdef_local, 1);
}


 /* Make the decision to send email about @param input_name, but only after a
  * little further investgation.
  *
  * We avoid sending email if there's a fresh dependency. To find out, we need
  * @param deps_fname, a .d file, created during the build.  We check each
  * dependency described there. If just one changed after the build started,
  * then we really don't want to hear about distcc-pump errors, because
  * dependencies shouldn't change. The files generated during the build are
  * exceptions. To disregard these, the distcc user may specify a glob pattern
  * in environment variable DISTCC_EXCLUDE_FRESH_FILES.
  *
  * Also, if there has been too many discrepancies (where the build has
  * succeeded remotely but failed locally), then we need to stop using
  * distcc-pump for the remainder of the build.  The present function
  * contributes to this logic: if it is determined that email must be sent, then
  * the count of such situations is incremented using the file @param
  * discrepancy_filename.
 */
static int dcc_please_send_email_after_investigation(
    const char *input_fname,
    const char *deps_fname,
    const char *discrepancy_filename) {

    int ret;
    char *fresh_dependency;
    const char *include_server_port = getenv("INCLUDE_SERVER_PORT");
    struct stat stat_port;
    rs_log_warning("remote compilation of '%s' failed, retried locally "
                   "and got a different result.", input_fname);
    if ((include_server_port != NULL) &&
        (stat(include_server_port, &stat_port)) == 0) {
        time_t build_start = stat_port.st_ctime;
        if (deps_fname) {
            const char *exclude_pattern =
                getenv("DISTCC_EXCLUDE_FRESH_FILES");

            if ((ret = dcc_fresh_dependency_exists(deps_fname,
                                                   exclude_pattern,
                                                   build_start,
                                                   &fresh_dependency))) {
                return ret;
            }
            if (fresh_dependency) {
                rs_log_warning("file '%s', a dependency of %s, "
                               "changed during the build", fresh_dependency,
                               input_fname);
                free(fresh_dependency);
                return dcc_note_discrepancy(discrepancy_filename);
            }
        }
    }
    dcc_please_send_email();
    return dcc_note_discrepancy(discrepancy_filename);
}

/**
 * Execute the commands in argv remotely or locally as appropriate.
 //视情况本地或远程执行argv中的命令
 *
 * We may need to run cpp locally; we can do that in the background
 * while trying to open a remote connection.
 *
 //我们可能需要在本地跑cpp; 我们可以在我们开远程连接的时候做这事

 * This function is slightly inefficient when it falls back to running
 * gcc locally, because cpp may be run twice.  
// 这个函数当我们要在本地运行gcc的时候可能会低效率, 因为cpp可能运行两次 //卧槽, 为什么两次?

 Perhaps we could adjust the command line to pass in the .i file.  
// 也许我们可以调整命令, 使其pass in the .i file, //卧槽, 点i文件是什么?
// 据http://www.cnblogs.com/ggjucheng/archive/2011/12/14/2287738.html这里说, .i是预处理文件
 
 On the other hand, if something has gone wrong, we should probably take the most
 * conservative course and run the command unaltered.  It should not
 * be a big performance problem because this should occur only rarely.
 // 如果出现了什么错误, 我们就以最保守的方式执行命令. 但是这不会成为严重的性能问题, 因为
 // 这并不常发生
 * @param argv Command to execute.  Does not include 0='distcc'.
 * Must be dynamically allocated.  This routine deallocates it.
 *
 // argv是要执行的命令, 们得保证argv[0]不是distcc, 以及argv是动态分配的, 以为这个函数会
 //把它析构掉
 * @param status On return, contains the waitstatus of the compiler or
 * preprocessor.  This function can succeed (in running the compiler) even if
 * the compiler itself fails.  
 // status是用来返回的, 包含编译器和预处理器的"等待状态"
 // 这个函数甚至在编译器出错了都会返回成功信息
 
 * If either the compiler or preprocessor fails,
 *   @p status is guaranteed to hold a failure value.
 //如果编译器和预处理器失败了, p参数会采集失败信息

//下面是实现描述
 * Implementation notes:
 *
 * This code might be simpler if we would only acquire one lock
 * at a time.  
// 如果我们一次只获取一个lock, 我们的代码会简单一些
 But we need to choose the server host in order
 * to determine whether it supports pump mode or not,
 * and choosing the server host requires acquiring its lock
 * (otherwise it might be busy when we we try to acquire it).
 // 但是, 我们需要选择host去判断其是否支持pump模式, 然后请求他的lock,
 // 另外, 我们请求锁时, 它可能是"忙"的,
 * So if the server chosen is not localhost, we need to hold the
 * remote host lock while we're doing local preprocessing or include
 * scanning.  Since local preprocessing/include scanning requires
 * us to acquire the local cpu lock, that means we need to hold two
 * locks at one time.
 *
 // 所以, 如果我们选择的不是localhost, 在我们本地预处理和include扫描的时候,
 //我们就需要掌握远程主机锁(remote host lock). 因为本地预处理和include扫描要求我们请求
 // 本地cpu锁, 这意味着我们同一时间需要掌握两个锁.

 * TODO: make pump mode a global flag, and drop support for
 * building with cpp mode on some hosts and not on others.
 * Then change the code so that we only choose the remote
 * host after local preprocessing/include scanning is finished
 * and the local cpu lock is released.
 */
// 这里有个todo!!!!!!!!!!!!
 // 弄一个pump mode的全局flag, 然后drop掉一些远程主机的cpp模式支持但另一些不drop,
 // 然后改变代码是的们在预处理/include扫描完成以及本地cpu lock释放后, 仅仅选择远程主机
 // 卧槽这什么意思?

static int
dcc_build_somewhere(char *argv[],
                    int sg_level,
                    int *status)
{
    char *input_fname = NULL, *output_fname, *cpp_fname, *deps_fname = NULL;
    char **files;
    char **server_side_argv = NULL;
    int server_side_argv_deep_copied = 0;
    char *server_stderr_fname = NULL;
    int needs_dotd = 0;
    int sets_dotd_target = 0;
    pid_t cpp_pid = 0;
    int cpu_lock_fd = -1, local_cpu_lock_fd = -1;
    int ret;
    int remote_ret = 0;
    struct dcc_hostdef *host = NULL;
    char *discrepancy_filename = NULL;
    char **new_argv;

    if ((ret = dcc_expand_preprocessor_options(&argv)) != 0)
    //这个函数实现在arg.c, 目的是吧"Wp,"形式的选项处理掉
        goto clean_up;//不成功就clean_up

    if ((ret = dcc_discrepancy_filename(&discrepancy_filename)))
        //这个实现在上面, 把discrepancy_filename存到参数里面
        goto clean_up;

    if (sg_level) /* Recursive distcc - run locally, and skip all locking. */
        //如果是递归调用, 就在本地运行
        goto run_local;

    /* TODO: Perhaps tidy up these gotos. *///各种goto被吐槽了

    /* FIXME: this may leak memory for argv. *///内存泄露应该不管我事吧

    ret = dcc_scan_args(argv, &input_fname, &output_fname, &new_argv);
    //扫描argv, 处理一些需要本地操作的选项, 看看有没有-o, 没有就尽量自动补全
    dcc_free_argv(argv);
    //因为扫描的过程中复制到了new_argv, 所以这里释放掉
    argv = new_argv;
    if (ret != 0) {
        /* we need to scan the arguments even if we already know it's
         * local, so that we can pick up distcc client options. */
        //dcc_scan_args的结果导致我们本地运行
        goto lock_local;
    }

#if 0
    /* turned off because we never spend long in this state. */
    dcc_note_state(DCC_PHASE_STARTUP, input_fname, NULL);
#endif
    if ((ret = dcc_make_tmpnam("distcc_server_stderr", ".txt",//这里创建临时文件是带有随机串保证唯一的
                               &server_stderr_fname))) {
        /* So we are failing locally to make a temp file to store the
         * server-side errors in; it's unlikely anything else will
         * work, but let's try the compilation locally.
         //这里代表我们甚至无法创建临时文件以储存服务器端的错误, 要是这样我们其他事情也没
         //什么希望了, 不过我们还是尝试去本地编译
         * FIXME: this will blame the server for a failure that is
         * local. However, we don't make any distrinction between
         * all the reasons dcc_compile_remote can fail either;
         * and some of those reasons are local.
         *///这里有个bug, 这里会错怪服务器, 因为可能错误是本地的, 因为我们并没有
         //区分dcc_compile_remote
        goto fallback;//卧槽!fallback应该怎么翻译
    }

    /* Lock ordering invariant: always acquire the lock for the
     * remote host (if any) first. *///加锁策略:总是先锁远程host

    /* Choose the distcc server host (which could be either a remote
     * host or localhost) and acquire the lock for it.  */
    //选择一个服务器, 可能是远程, 可能是本地, 然后请求加锁之
    if ((ret = dcc_pick_host_from_list_and_lock_it(&host, &cpu_lock_fd)) != 0) {//这个实现在where.c
        /* Doesn't happen at the moment: all failures are masked by
           returning localhost. */
        goto fallback;
    }
    //到这里已经取得了host和相应的锁id, 然而并没有作负载均衡, 排名靠前的总是被调用编译
    // 但是负载均衡是否有意义又是一个问题, 如果是独占用于编译的, 不均衡也没关系
    if (host->mode == DCC_MODE_LOCAL) {
        // 如果取得了一个local, 就本地处理了
        /* We picked localhost and already have a lock on it so no
         * need to lock it now. */
        goto run_local;
    }

    /* Lock the local CPU, since we're going to be doing preprocessing
     * or include scanning. */
    //对本地加锁, 以便进行预处理和include扫描(卧槽, 瞬间怀疑cpp这三个字母的理解)
    if ((ret = dcc_lock_local_cpp(&local_cpu_lock_fd)) != 0) {//这个local_cpu_lock_fd
        goto fallback;
    }
    //cpp_where意指在哪预处理, 所以cpp应该是comfile preprocessing的意思
    if (host->cpp_where == DCC_CPP_ON_SERVER) {
        //如果是要在远程预处理, 那可能不是一个好主意
        /* Perhaps it is not a good idea to preprocess on the server. */
        // 处理一些特殊情况, 把预处理改成本地进行
        dcc_perhaps_adjust_cpp_where_and_protover(input_fname, host,
                                                  discrepancy_filename);
    }
    if (dcc_scan_includes) {//如果需要扫描include
        ret = dcc_approximate_includes(host, argv);//这个定义在include_server_if.c
        goto unlock_and_clean_up;
    }
    if (host->cpp_where == DCC_CPP_ON_SERVER) {
        if ((ret = dcc_talk_to_include_server(argv, &files))) {
            /* Fallback to doing cpp locally */
            /* It's unfortunate that the variable that controls that is in the
             * "host" datastructure, even though in this case it's the client
             * that fails to support it,  but "host" is what gets passed
             * around in the client code. We are, in essense, throwing away
             * the host's capability to do cpp, so if this code was to execute
             * again (it won't, not in the same process) we wouldn't know if
             * the server supports it or not.
             */
            rs_log_warning("failed to get includes from include server, "
                           "preprocessing locally");
            if (dcc_getenv_bool("DISTCC_TESTING_INCLUDE_SERVER", 0))
                dcc_exit(ret);
            host->cpp_where = DCC_CPP_ON_CLIENT;
            dcc_get_protover_from_features(host->compr,
                                           host->cpp_where,
                                           &host->protover);
        } else {
            /* Include server succeeded. */
            /* We're done with local "preprocessing" (include scanning). */
            dcc_unlock(local_cpu_lock_fd);
            /* Don't try to unlock again in dcc_compile_remote. */
            local_cpu_lock_fd = -1;
        }

    }

    if (host->cpp_where == DCC_CPP_ON_CLIENT) {
        files = NULL;

        if ((ret = dcc_cpp_maybe(argv, input_fname, &cpp_fname, &cpp_pid) != 0))
            goto fallback;

        if ((ret = dcc_strip_local_args(argv, &server_side_argv)))
            goto fallback;

    } else {
        char *dotd_target = NULL;
        cpp_fname = NULL;
        cpp_pid = 0;
        dcc_get_dotd_info(argv, &deps_fname, &needs_dotd,
                          &sets_dotd_target, &dotd_target);
        server_side_argv_deep_copied = 1;
        if ((ret = dcc_copy_argv(argv, &server_side_argv, 2)))
            goto fallback;
        if (needs_dotd && !sets_dotd_target) {
           dcc_argv_append(server_side_argv, strdup("-MT"));
           if (dotd_target == NULL)
               dcc_argv_append(server_side_argv, strdup(output_fname));
           else
               dcc_argv_append(server_side_argv, strdup(dotd_target));
        }
    }
    if ((ret = dcc_compile_remote(server_side_argv,
                                  input_fname,
                                  cpp_fname,
                                  files,
                                  output_fname,
                                  needs_dotd ? deps_fname : NULL,
                                  server_stderr_fname,
                                  cpp_pid, local_cpu_lock_fd,
                  host, status)) != 0) {
        /* Returns zero if we successfully ran the compiler, even if
         * the compiler itself bombed out. */

        /* dcc_compile_remote() already unlocked local_cpu_lock_fd. */
        local_cpu_lock_fd = -1;

        goto fallback;
    }
    /* dcc_compile_remote() already unlocked local_cpu_lock_fd. */
    local_cpu_lock_fd = -1;

    dcc_enjoyed_host(host);

    dcc_unlock(cpu_lock_fd);
    cpu_lock_fd = -1;

    ret = dcc_critique_status(*status, "compile", input_fname, host, 1);
    if (ret == 0) {
        /* Try to copy the server-side errors on stderr.
         * If that fails, even though the compilation succeeded,
         * we haven't managed to give these errors to the user,
         * so we have to try again.
         * FIXME: Just like in the attempt to make a temporary file, this
         * is unlikely to fail, if it does it's unlikely any other
         * operation will work, and this makes the mistake of
         * blaming the server for what is (clearly?) a local failure.
         */
        if ((dcc_copy_file_to_fd(server_stderr_fname, STDERR_FILENO))) {
            rs_log_warning("Could not show server-side errors");
            goto fallback;
        }
        /* SUCCESS! */
        goto clean_up;
    }
    if (ret < 128) {
        /* Remote compile just failed, e.g. with syntax error.
           It may be that the remote compilation failed because
           the file has an error, or because we did something
           wrong (e.g. we did not send all the necessary files.)
           Retry locally. If the local compilation also fails,
           then we know it's the program that has the error,
           and it doesn't really matter that we recompile, because
           this is rare.
           If the local compilation succeeds, then we know it's our
           fault, and we should do something about it later.
           (Currently, we send email to an appropriate email address).
        */
        if (getenv("DISTCC_SKIP_LOCAL_RETRY")) {
            /* dont retry locally. We'll treat the remote failure as
               if it was a local one. But if we can't get the failures
               then we need to retry regardless.
            */
            if ((dcc_copy_file_to_fd(server_stderr_fname, STDERR_FILENO))) {
                rs_log_warning("remote compilation of '%s' failed",\
                               input_fname);
                rs_log_warning("Could not show server-side errors, retrying locally");
                goto fallback;
            }
            /* Not retrying */
            goto clean_up;
        } else {
            rs_log_warning("remote compilation of '%s' failed, retrying locally",
                           input_fname);
            remote_ret = ret;
            goto fallback;
        }
    }


  fallback:
    if (host)
        dcc_disliked_host(host);

    if (cpu_lock_fd != -1) {
        dcc_unlock(cpu_lock_fd);
        cpu_lock_fd = -1;
    }
    if (local_cpu_lock_fd != -1) {
        dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1;
    }

    if (!dcc_getenv_bool("DISTCC_FALLBACK", 1)) {
        rs_log_warning("failed to distribute and fallbacks are disabled");
        /* Try copying any server-side error message to stderr;
         * If we fail the user will miss all the messages from the server; so
         * we pretend we failed remotely.
         */
        if ((dcc_copy_file_to_fd(server_stderr_fname, STDERR_FILENO))) {
            rs_log_error("Could not print error messages from '%s'",
                         server_stderr_fname);
        }
        goto clean_up;
    }

    /* At this point, we can abandon the remote errors. */

    /* "You guys are so lazy!  Do I have to do all the work myself??" */
    if (host) {
        rs_log(RS_LOG_WARNING|RS_LOG_NONAME,
               "failed to distribute %s to %s, running locally instead",
               input_fname ? input_fname : "(unknown)",
               host->hostdef_string);
    } else {
        rs_log_warning("failed to distribute, running locally instead");
    }

  lock_local:
    dcc_lock_local(&cpu_lock_fd);

  run_local:
    /* Either compile locally, after remote failure, or simply do other cc tasks
       as assembling, linking, etc. */
    ret = dcc_compile_local(argv, input_fname);
    if (remote_ret != 0) {
        if (remote_ret != ret) {
            /* Oops! it seems what we did remotely is not the same as what we did
              locally. We normally send email in such situations (if emailing is
              enabled), but we attempt an a time analysis of source files in order
              to avoid doing so in case source files we changed during the build.
            */
            (void) dcc_please_send_email_after_investigation(
                input_fname,
                deps_fname,
                discrepancy_filename);
        } else if (host) {
            /* Remote compilation failed, but we failed to compile this file too.
             * Don't punish that server, it's innocent.
             */
            dcc_enjoyed_host(host);
        }
    }

  unlock_and_clean_up:
    if (cpu_lock_fd != -1) {
        dcc_unlock(cpu_lock_fd);
        cpu_lock_fd = -1; /* Not really needed, just for consistency. */
    }
    /* For the --scan_includes case. */
    if (local_cpu_lock_fd != -1) {
        dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1; /* Not really needed, just for consistency. */
    }

  clean_up:
    dcc_free_argv(argv);
    if (server_side_argv_deep_copied) {
        if (server_side_argv != NULL) {
          dcc_free_argv(server_side_argv);
        }
    } else {
        free(server_side_argv);
    }
    free(discrepancy_filename);
    return ret;
}

/*
 * argv must be dynamically allocated.
 * This routine will deallocate it.
 */
int dcc_build_somewhere_timed(char *argv[],
                              int sg_level,
                              int *status)
{
    struct timeval before, after, delta;
    int ret;

    if (gettimeofday(&before, NULL))//这个是系统调用, 获取时间用的
        rs_log_warning("gettimeofday failed");

    ret = dcc_build_somewhere(argv, sg_level, status);
    //这看起来是阻塞的不是?

    if (gettimeofday(&after, NULL)) {
        rs_log_warning("gettimeofday failed");
    } else {
        /* TODO: Show rate based on cpp size?  Is that meaningful? */
        //当然, 用文件大小来衡量复杂性是木有意义的
        timeval_subtract(&delta, &after, &before);
        //这是统计编译时间吗?
        rs_log(RS_LOG_INFO|RS_LOG_NONAME,
               "elapsed compilation time %ld.%06lds",
               delta.tv_sec, (long) delta.tv_usec);
    }

    return ret;
}

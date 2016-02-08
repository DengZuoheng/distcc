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


            /* 4: The noise of a multitude in the
             * mountains, like as of a great people; a
             * tumultuous noise of the kingdoms of nations
             * gathered together: the LORD of hosts
             * mustereth the host of the battle.
             *        -- Isaiah 13 */



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#if HAVE_LIBIBERTY
#if defined (HAVE_LIBIBERTY_H)
#include <libiberty.h>
#elif defined (HAVE_LIBIBERTY_LIBIBERTY_H)
#include <libiberty/libiberty.h>
#else
#error Need libiberty.h
#endif
#endif

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"
#include "hosts.h"
#include "bulk.h"
#include "implicit.h"
#include "compile.h"
#include "emaillog.h"


/* Name of this program, for trace.c */
const char *rs_program_name = "distcc";

/**
 * @file
 *
 * Entry point for the distcc client.
 *
 * There are three methods of use for distcc: explicit (distcc gcc -c
 * foo.c), implicit (distcc -c foo.c) and masqueraded (gcc -c foo.c,
 * where gcc is really a link to distcc).
 *
 * Detecting these is relatively easy by examining the first one or
 * two words of the command.  We also need to make sure that when we
 * go to run the compiler, we run the one intended by the user.
 *
 * In particular, for masqueraded mode, we want to make sure that we
 * don't invoke distcc recursively.
 **/
//显示帮助信息
static void dcc_show_usage(void)
{
    dcc_show_version("distcc");
    printf(
"Usage:\n"
"   distcc [--scan-includes] [COMPILER] [compile options] -o OBJECT -c SOURCE\n"
"   distcc [--help|--version|--show-hosts|-j]\n"
"\n"
"Options:\n"
"   COMPILER                   Defaults to \"cc\".\n"
"   --help                     Explain usage, and exit.\n"
"   --version                  Show version, and exit.\n"
"   --show-hosts               Show host list, and exit.\n"
"   -j                         Show the concurrency level, as calculated from\n"
"                              the host list, and exit.\n"
"   --scan-includes            Show the files that distcc would send to the\n"
"                              remote machine, and exit.  (Pump mode only.)\n"
#ifdef HAVE_GSSAPI
"   --show-principal           Show current distccd GSS-API principal and exit.\n"
#endif
"\n"
"Environment variables:\n"
"   See the manual page for a complete list.\n"
"   DISTCC_VERBOSE=1           Give debug messages.\n"
"   DISTCC_LOG                 Send messages to file, not stderr.\n"
"   DISTCC_SSH                 Command to run to open SSH connections.\n"
"   DISTCC_DIR                 Directory for host list and locks.\n"
#ifdef HAVE_GSSAPI
"   DISTCC_PRINCIPAL	      The name of the server principal to connect to.\n"
#endif
"\n"
"Server specification:\n"
"A list of servers is taken from the environment variable $DISTCC_HOSTS, or\n"
"$DISTCC_DIR/hosts, or ~/.distcc/hosts, or %s/distcc/hosts.\n"
"Each host can be given in any of these forms, see the manual for details:\n"
"\n"
"   localhost                  Run in place.\n"
"   HOST                       TCP connection, port %d.\n"
"   HOST:PORT                  TCP connection, specified port.\n"
"   @HOST                      SSH connection to specified host.\n"
"   USER@HOST                  SSH connection to specified username at host.\n"
"   HOSTSPEC,lzo               Enable compression.\n"
"   HOSTSPEC,cpp,lzo           Use pump mode (remote preprocessing).\n"
"   HOSTSPEC,auth              Enable GSS-API based mutual authenticaton.\n"
"   --randomize                Randomize the server list before execution.\n"
"\n"
"distcc distributes compilation jobs across volunteer machines running\n"
"distccd.  Jobs that cannot be distributed, such as linking, are run locally.\n"
"distcc should be used with make's -jN option to execute in parallel on\n"
"several machines.\n",
    SYSCONFDIR,
    DISTCC_DEFAULT_PORT);
}

//SIG_DFL default signal handling
//SIG_IGN signal is ignored

static RETSIGTYPE dcc_client_signalled (int whichsig)
{
    signal(whichsig, SIG_DFL);//第二个参数应该是回调函数, 这个意思是重新设成默认行为吗
    //这个意思是只响应一次, 然后就清空临时文件然后退出?

#ifdef HAVE_STRSIGNAL
    rs_log_info("%s", strsignal(whichsig));
#else
    rs_log_info("terminated by signal %d", whichsig);
#endif

    dcc_cleanup_tempfiles_from_signal_handler();//在cleanup.c中

    raise(whichsig);

}

//相当于绑定dcc客户端要响应的信号
static void dcc_client_catch_signals(void)
{
    signal(SIGTERM, &dcc_client_signalled);//终止信号
    signal(SIGINT, &dcc_client_signalled);//ctrl+信号
    signal(SIGHUP, &dcc_client_signalled);//sent to a process when its controlling terminal is closed
}

static void dcc_free_hostlist(struct dcc_hostdef *list) {
    while (list) {
        struct dcc_hostdef *l = list;
        list = list->next;
        dcc_free_hostdef(l);
    }
}

static void dcc_show_hosts(void) {
    struct dcc_hostdef *list, *l;
    int nhosts;
    //get一个host列表, 一个个打印出来
    if (dcc_get_hostlist(&list, &nhosts) != 0) {//这个定义在host.c
        //这个涉及解析HOST环境变量和
        rs_log_crit("Failed to get host list");
        return;
    }

    for (l = list; l; l = l->next)
        printf("%s\n", l->hostdef_string);

    dcc_free_hostlist(list);//释放列表内存
}

static void dcc_concurrency_level(void) {
    struct dcc_hostdef *list, *l;
    int nhosts;
    int nslots = 0;

    if (dcc_get_hostlist(&list, &nhosts) != 0) {
        rs_log_crit("Failed to get host list");
        return;
    }

    for (l = list; l; l = l->next)
        nslots += l->n_slots;
    //把host列表get回来, 然后把所有slots加起来

    dcc_free_hostlist(list);
    //然后free掉

    printf("%i\n", nslots);
}

#ifdef HAVE_GSSAPI
/*
 * Print out the name of the principal.
 */
static void dcc_gssapi_show_principal(void) {
    char *princ_env_val = NULL;

    if((princ_env_val = getenv("DISTCC_PRINCIPAL"))) {
	    printf("Principal is\t: %s\n", princ_env_val);
    } else {
        printf("Principal\t: Not Set.\n");
    }
}
#endif

/**
 * distcc client entry point.
 *
 * This is typically called by make in place of the real compiler. 在make中代替真的编译器
 *
 * Performs basic setup and checks for distcc arguments, and then kicks off
 * dcc_build_somewhere().
 **/
int main(int argc, char **argv)
{
    int status, sg_level, tweaked_path = 0;
    char **compiler_args = NULL; /* dynamically allocated */
    char *compiler_name; /* points into argv[0] */
    int ret;

    dcc_client_catch_signals();//这个定义就在上面, 这个也是清空临时文件, 不过是信号退出, 相当于异常中断
    atexit(dcc_cleanup_tempfiles);//退出时清空临时文件, 而这个是正常退出, 这个也在cleanup.c中定义
    atexit(dcc_remove_state_file);//退出时移除状态文件, 定义在state.h/state.c

    dcc_set_trace_from_env();
    //trace是回溯的意思, distcc.h中定义, traceenv中实现
    //实际上应该是初始化logger的意思
    dcc_setup_log_email();//这个定义在emaillog.c和emaillog.h
    //这是一个发邮件的logger, 完成之后调用系统功能发送邮件, 
    //golang中有smtp包, 但这不重要, 我们先不管

    dcc_trace_version();//回溯版本是几个意思?
    //这个函数定义在distcc.h, 实现在help.c, 相当于打印version信息

    compiler_name = (char *) dcc_find_basename(argv[0]);
    //这个dcc_find_basename定义在distcc.h, 实现在filename.c

#if HAVE_LIBIBERTY    
    /* Expand @FILE arguments. */
    expandargv(&argc, &argv);//这是lib iberty特有的, golang不一定有
    //可能得自己重写
#endif
 
    /* Ignore SIGPIPE; we consistently check error codes and will
     * see the EPIPE. */
    dcc_ignore_sigpipe(1);//这在util.h定义, 在util.c实现

    sg_level = dcc_recursion_safeguard();//这个定义在distcc.h, 实现在safeguard.c

    rs_trace("compiler name is \"%s\"", compiler_name);
    //complier_name是argv[0], 所以这是起始参数为distcc的情况
    if (strstr(compiler_name, "distcc") != NULL) {
        //如果编译器名中找到了distcc, 那多半有问题
        /* Either "distcc -c hello.c" or "distcc gcc -c hello.c" */
        if (argc <= 1) {//比如没有参数, 就warnning一下
            fprintf (stderr,
                     "%s: missing option/operand\n"
                     "Try `%s --help' for more information.\n",
                     argv[0], argv[0]);
            ret = EXIT_BAD_ARGUMENTS;
            goto out;
        }

        if (!strcmp(argv[1], "--help")) {
            //如果是help命令, 就打印usage信息
            dcc_show_usage();
            ret = 0;
            //然后退出, 退出时复杂操作, 所以goto
            goto out;
        }

        if (!strcmp(argv[1], "--version")) {
            //如果是version信息, 就打印version
            dcc_show_version("distcc");//实现在help.c
            ret = 0;
            goto out;
        }

        if (!strcmp(argv[1], "--show-hosts")) {
            dcc_show_hosts();//实现在上面
            ret = 0;
            goto out;
        }

        if (!strcmp(argv[1], "-j")) {
            dcc_concurrency_level();//实现就在上面, 把所有host的slot加起来就是了
            ret = 0;
            goto out;
        }

        if (!strcmp(argv[1], "--scan-includes")) {
            //这是一个flag设置
            if (argc <= 2) {
                fprintf (stderr,
                         "%s: missing operand\n"
                         "Try `%s --help' for more information.\n",
                         argv[0], argv[0]);
                ret = EXIT_BAD_ARGUMENTS;
                goto out;
            }
            dcc_scan_includes = 1;//全局变量, 定义在compile.h中
            argv++;//这里会改变argv[1]
        }

#ifdef HAVE_GSSAPI//https://en.wikipedia.org/wiki/Generic_Security_Services_Application_Program_Interface
	    if (!strcmp(argv[1], "--show-principal")) {
	        dcc_gssapi_show_principal();//定义在dopt.c中, 其实就是打印一个环境变量
	        ret = 0;
	        goto out;
	    }
#endif
            //这个find_compiler在implicit.h中定义,implicit.c中实现
        if ((ret = dcc_find_compiler(argv, &compiler_args)) != 0) {
            goto out;
        }
        /* compiler_args is now respectively either "cc -c hello.c" or
         * "gcc -c hello.c" */

#if 0
        /* I don't think we need to call this: if we reached this
         * line, our invocation name is something like 'distcc', and
         * that's never a problem for masquerading loops. */
        if ((ret = dcc_trim_path(compiler_name)) != 0)
            goto out;
#endif
    } else {//这就是起始参数不是distcc的情况
        /* Invoked as "cc -c hello.c", with masqueraded path */
        //就是用cc为始调用的
        if ((ret = dcc_support_masquerade(argv, compiler_name,
                                          &tweaked_path)) != 0)
            //tweaked_path在main函数开始的时候定义的, 初始为0
            //dcc_support_masquerade在distcc.h中定义, 在climasq.c中实现
            //应该是找到compiler_name的真实路径, 并将其设为环境变量中的PATH
            goto out;

        if ((ret = dcc_copy_argv(argv, &compiler_args, 0)) != 0) {
            //将argv复制到compiler_args
            goto out;
        }
        free(compiler_args[0]);
        compiler_args[0] = strdup(compiler_name);//将第一个参数替换成正确的compiler_name
        if (!compiler_args[0]) {
            rs_log_error("strdup failed - out of memory?");
            ret = EXIT_OUT_OF_MEMORY;
            goto out;
        }
    }

    if (sg_level - tweaked_path > 0) {//这两个参数是防止递归调用的
        //可能递归了
        rs_log_crit("distcc seems to have invoked itself recursively!");
        ret = EXIT_RECURSION;
        goto out;
    }
    //这句是重点, 实现在compile.c, 这就要100行好吗
    //然后就去执行编译了
    ret = dcc_build_somewhere_timed(compiler_args, sg_level, &status);//status是一个输出参数
    compiler_args = NULL; /* dcc_build_somewhere_timed already free'd it. */

    out:
    if (compiler_args) {
      dcc_free_argv(compiler_args);//这个在argutil.c中
    }
    dcc_maybe_send_email();
    dcc_exit(ret);
}

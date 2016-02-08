/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
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


                         /*
                          * And the magicians did so with their enchantments,
                          * and brought up frogs upon the land of Egypt.
                          *            --  Exodus 8:7
                          */



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "distcc.h"
#include "util.h"
#include "trace.h"
#include "exitcode.h"

/**
 * For masquerade mode, change the path to remove the directory containing the
 * distcc mask, so that invoking the same name will find the underlying
 * compiler instead.
 *
 //这个masquerade mode不知道什么意思, 这个函数把包含distcc掩码的path移除掉distcc, 使其
 // 可以调用隐含的编译器
 * @param progname basename under which distcc was introduced.  If we reached
 * this point, then it's the same as the name of the real compiler, e.g. "cc".
 *
 * @param did_masquerade specifies an integer that will be set to 1 if the
 * path was changed.
 *
 * @return 0 or standard error.
 **/
int dcc_support_masquerade(char *argv[], char *progname, int *did_masquerade)
{
    const char *envpath, *findpath, *p, *n;
    char *buf;
    size_t len;
    size_t findlen;

    if (!(envpath = getenv("PATH")))
        /* strange but true*///没有path变量也是算可以的
        return 0;

    if (!(buf = malloc(strlen(envpath)+1+strlen(progname)+1))) {
        rs_log_error("failed to allocate buffer for new PATH");
        //buf是用来储存编译器路径的, 因为整一个path环境变量可能就只有一个值
        return EXIT_OUT_OF_MEMORY;
    }

    /* Filter PATH to contain only the part that is past our dir.
     * If we were called explicitly, find the named dir on the PATH. */
    if (progname != argv[0]) {//如果argv[0]不等于programename, 就设置findpath
        //如果是从distcc.c主函数中调用过来的, 可能progname可能不等于argv[0], 
        // 因为progname是经过dcc_find_basename处理的
        findpath = dcc_abspath(argv[0], progname - argv[0] - 1);//第二个参数是path_len, 这怎么计算出来的?
        findlen = strlen(findpath);
        //应该说, 如果progname不等于argv[0], 说明第一个参数不是cc,gcc等仅仅是命令的东西, 应该是有路径信息的
    } else {
        findpath = NULL;
        findlen = 0;
    }

    for (n = p = envpath; *n; p = n) {//对path中每个路径执行一次循环体
        /* Find the length of this component of the path */
        n = strchr(p, ':');
        if (n)
            len = n++ - p;
        else {
            len = strlen(p);
            n = p + len;
        }
        if (findpath) {//有绝对路径的情况
            /* Looking for a component in the path equal to findpath */

            /* FIXME: This won't catch paths that are in fact the same, but
             * that are not the same string.  This might happen if you have
             * multiple slashes, or dots, or symlinks... *///这里说有问题, 可能因为
             //符号链接, 重复斜杠, 之类的事情导致实际上是同一个目录, 但不是同一个字符串
           if (len != findlen || strncmp(p, findpath, findlen) != 0)
                //判断是否相等, 不相等就继续
                continue;
        } else {//找不到绝对路径, 只能拼接起来测试能不能访问了
            /* Looking for a component in the path containing a file
             * progname. */

            /* FIXME: This gets a false match if you have a subdirectory that
             * happens to be of the right name, e.g. /usr/bin/distcc... */
            //这里也是有bug的
            strncpy(buf, p, (size_t) len);
            sprintf(buf + len, "/%s", progname);
            if (access(buf, X_OK) != 0)
                continue;
        }
        /* Set p to the part of the path past our match. */
        p = n;
        break;
    }

    if (*p != '\0') {
        int ret = dcc_set_path(p);//这就把环境变量的path给重设成找到编译器路径的那个目录了?
        //感觉这个函数还是很武断的
        if (ret)
            return ret;
        *did_masquerade = 1;
    }
    else {
        rs_trace("not modifying PATH");
        *did_masquerade = 0;
    }

    free(buf);
    return 0;
}

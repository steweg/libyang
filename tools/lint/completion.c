/**
 * @file completion.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief libyang's yanglint tool auto completion
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L /* strdup */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libyang.h"

#include "cmd.h"
#include "common.h"
#include "compat.h"
#include "linenoise/linenoise.h"

/* from the main.c */
extern struct ly_ctx *ctx;

/**
 * @brief Add a match to the completions array.
 *
 * @param[in] match Match to be added.
 * @param[in,out] matches Matches provided to the user as a completion hint.
 * @param[in,out] match_count Number of matches.
 */
static void
cmd_completion_add_match(const char *match, char ***matches, unsigned int *match_count)
{
    void *p;

    ++(*match_count);
    p = realloc(*matches, *match_count * sizeof **matches);
    if (!p) {
        YLMSG_E("Memory allocation failed (%s:%d, %s)", __FILE__, __LINE__, strerror(errno));
        return;
    }
    *matches = p;
    (*matches)[*match_count - 1] = strdup(match);
}

/**
 * @brief Provides completion for command names.
 *
 * @param[in] hint User input.
 * @param[out] matches Matches provided to the user as a completion hint.
 * @param[out] match_count Number of matches.
 */
static void
get_cmd_completion(const char *hint, char ***matches, unsigned int *match_count)
{
    int i;

    *match_count = 0;
    *matches = NULL;

    for (i = 0; commands[i].name; i++) {
        if (!strncmp(hint, commands[i].name, strlen(hint))) {
            cmd_completion_add_match(commands[i].name, matches, match_count);
        }
    }
}

/**
 * @brief Provides completion for module names.
 *
 * @param[in] hint User input.
 * @param[out] matches Matches provided to the user as a completion hint.
 * @param[out] match_count Number of matches.
 */
static void
get_model_completion(const char *hint, char ***matches, unsigned int *match_count)
{
    LY_ARRAY_COUNT_TYPE u;
    uint32_t idx = 0;
    const struct lys_module *module;

    *match_count = 0;
    *matches = NULL;

    while ((module = ly_ctx_get_module_iter(ctx, &idx))) {
        if (!strncmp(hint, module->name, strlen(hint))) {
            cmd_completion_add_match(module->name, matches, match_count);
        }

        LY_ARRAY_FOR(module->parsed->includes, u) {
            if (!strncmp(hint, module->parsed->includes[u].submodule->name, strlen(hint))) {
                cmd_completion_add_match(module->parsed->includes[u].submodule->name, matches, match_count);
            }
        }
    }
}

/**
 * @brief Add all child nodes of a single node to the completion hint.
 *
 * @param[in] last_node Node of which children will be added to the hint.
 * @param matches[out] Matches provided to the user as a completion hint.
 * @param match_count[out] Number of matches.
 */
static void
single_hint_add_children(const struct lysc_node *last_node, char ***matches, unsigned int *match_count)
{
    const struct lysc_node *node = NULL;
    char *match;

    if (!last_node) {
        return;
    }

    while ((node = lys_getnext(node, last_node, NULL, LYS_GETNEXT_WITHCASE | LYS_GETNEXT_WITHCHOICE))) {
        match = lysc_path(node, LYSC_PATH_LOG, NULL, 0);
        cmd_completion_add_match(match, matches, match_count);
        free(match);
    }
}

/**
 * @brief Add module and/or node's children names to the hint.
 *
 * @param[in] module Compiled schema module.
 * @param[in] parent Parent node of which children are potential matches.
 * @param[in] hint_node_name Node name contained within the hint specified by user.
 * @param[in,out] matches Matches provided to the user as a completion hint.
 * @param[in,out] match_count Number of matches.
 * @param[out] last_node Last processed node.
 */
static void
add_all_children_nodes(const struct lysc_module *module, const struct lysc_node *parent,
        const char *hint_node_name, char ***matches, unsigned int *match_count, const struct lysc_node **last_node)
{
    const struct lysc_node *node;
    char *match, *node_name = NULL;

    *last_node = NULL;

    if (!parent && !module) {
        return;
    }

    node = NULL;
    while ((node = lys_getnext(node, parent, module, LYS_GETNEXT_WITHCASE | LYS_GETNEXT_WITHCHOICE))) {
        if (parent && (node->module != parent->module)) {
            /* augmented node */
            if (asprintf(&node_name, "%s:%s", node->module->name, node->name) == -1) {
                YLMSG_E("Memory allocation failed (%s:%d, %s)", __FILE__, __LINE__, strerror(errno));
                break;
            }
        } else {
            node_name = strdup(node->name);
            if (!node_name) {
                YLMSG_E("Memory allocation failed (%s:%d, %s)", __FILE__, __LINE__, strerror(errno));
                break;
            }
        }
        if (!hint_node_name || !strncmp(hint_node_name, node_name, strlen(hint_node_name))) {
            /* adding just module names + their top level node(s) to the hint */
            *last_node = node;
            match = lysc_path(node, LYSC_PATH_LOG, NULL, 0);
            cmd_completion_add_match(match, matches, match_count);
            free(match);
        }
        free(node_name);
    }
}

/**
 * @brief Provides completion for schemas.
 *
 * @param[in] hint User input.
 * @param[out] matches Matches provided to the user as a completion hint.
 * @param[out] match_count Number of matches.
 */
static void
get_schema_completion(const char *hint, char ***matches, unsigned int *match_count)
{
    const struct lys_module *module;
    uint32_t idx;
    const char *start;
    char *end, *module_name = NULL, *path = NULL;
    const struct lysc_node *parent, *last_node;
    int rc = 0;
    size_t len;

    *match_count = 0;
    *matches = NULL;

    if (strlen(hint)) {
        if (hint[0] != '/') {
            return;
        }
        start = hint + 1;
    } else {
        start = hint;
    }

    end = strchr(start, ':');
    if (!end) {
        /* no module name */
        len = strlen(start);

        /* go through all the modules */
        idx = 0;
        while ((module = ly_ctx_get_module_iter(ctx, &idx))) {
            if (!module->implemented) {
                continue;
            }

            if (!len || !strncmp(start, module->name, len)) {
                /* add all their (matching) top level nodes */
                add_all_children_nodes(module->compiled, NULL, NULL, matches, match_count, &last_node);
            }
        }
    } else {
        /* module name known */
        module_name = strndup(start, end - start);
        if (!module_name) {
            YLMSG_E("Memory allocation failed (%s:%d, %s)", __FILE__, __LINE__, strerror(errno));
            rc = 1;
            goto cleanup;
        }

        module = ly_ctx_get_module_implemented(ctx, module_name);
        if (!module) {
            goto cleanup;
        }

        /* find the last '/', if it is at the beginning of the hint, only path up to the top level node is known,
         * else the name of the last node starts after the found '/' */
        start = strrchr(hint, '/');
        if (!start) {
            goto cleanup;
        }

        if (start == hint) {
            /* only the (incomplete) top level node path, add all (matching) top level nodes */
            add_all_children_nodes(module->compiled, NULL, end + 1, matches, match_count, &last_node);
            goto cleanup;
        }

        /* get rid of stuff after the last '/' to obtain the parent node */
        path = strndup(hint, start - hint);
        if (!path) {
            YLMSG_E("Memory allocation failed (%s:%d, %s)", __FILE__, __LINE__, strerror(errno));
            rc = 1;
            goto cleanup;
        }

        /* get the last parent in the hint (it may not exist) */
        parent = find_schema_path(ctx, path);

        /* add all (matching) child nodes of the parent */
        add_all_children_nodes(NULL, parent, start + 1, matches, match_count, &last_node);
    }

cleanup:
    if (!rc && (*match_count == 1)) {
        /* to avoid a single hint (space at the end), add all children as hints */
        single_hint_add_children(last_node, matches, match_count);
    }
    free(path);
    free(module_name);
}

/**
 * @brief Get the string before the hint, which autocompletion is for.
 *
 * @param[in] buf Complete user input.
 * @param[in] hint Hint part of the user input.
 * @return Pointer to the last string.
 */
static const char *
get_last_str(const char *buf, const char *hint)
{
    const char *ptr;

    if (buf == hint) {
        return buf;
    }

    ptr = hint - 1;
    while (ptr[0] == ' ') {
        --ptr;
        if (buf == ptr) {
            return buf;
        }
    }

    while (ptr[-1] != ' ') {
        --ptr;
        if (buf == ptr) {
            return buf;
        }
    }

    return ptr;
}

/* callback */
void
complete_cmd(const char *buf, const char *hint, linenoiseCompletions *lc)
{
    struct autocomplete {
        const char *cmd;    /**< command */
        const char *opt;    /**< optional option */
        int last_opt;       /**< whether to autocomplete even if an option is last in the hint */

        void (*ln_cb)(const char *, const char *, linenoiseCompletions *);  /**< linenoise callback to call */
        void (*yl_cb)(const char *, char ***, unsigned int *);              /**< yanglint callback to call */
    } ac[] = {
        {"add", NULL, 1, linenoisePathCompletion, NULL},
        {"searchpath", NULL, 0, linenoisePathCompletion, NULL},
        {"data", NULL, 0, linenoisePathCompletion, NULL},
        {"print", NULL, 0, NULL, get_model_completion},
        {"feature", NULL, 0, NULL, get_model_completion},
        {"print", "-P", 1, NULL, get_schema_completion},
    };
    size_t cmd_len;
    const char *last;
    char **matches = NULL;
    unsigned int match_count = 0, i;

    if (buf == hint) {
        /* command autocomplete */
        get_cmd_completion(hint, &matches, &match_count);

    } else {
        for (i = 0; i < (sizeof ac / sizeof *ac); ++i) {
            cmd_len = strlen(ac[i].cmd);
            if (strncmp(buf, ac[i].cmd, cmd_len) || (buf[cmd_len] != ' ')) {
                /* not this command */
                continue;
            }

            last = get_last_str(buf, hint);
            if (ac[i].opt && strncmp(ac[i].opt, last, strlen(ac[i].opt))) {
                /* autocompletion for (another) option */
                continue;
            }
            if (!ac[i].last_opt && (last[0] == '-')) {
                /* autocompletion for the command, not an option */
                continue;
            }
            if ((last != buf) && (last[0] != '-')) {
                /* autocompleted */
                return;
            }

            /* callback */
            if (ac[i].ln_cb) {
                ac[i].ln_cb(buf, hint, lc);
            } else {
                ac[i].yl_cb(hint, &matches, &match_count);
            }
            break;
        }
    }

    /* transform matches into autocompletion, if needed */
    for (i = 0; i < match_count; ++i) {
        linenoiseAddCompletion(lc, matches[i]);
        free(matches[i]);
    }
    free(matches);
}

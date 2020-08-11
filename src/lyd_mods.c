/**
 * @file lyd_mods.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Sysrepo module data routines
 *
 * @copyright
 * Copyright 2019 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "common.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include "../modules/sysrepo_yang.h"
#include "../modules/ietf_datastores_yang.h"
#if SR_YANGLIB_REVISION == 2019-01-04
# include "../modules/ietf_yang_library@2019_01_04_yang.h"
#elif SR_YANGLIB_REVISION == 2016-06-21
# include "../modules/ietf_yang_library@2016_06_21_yang.h"
#else
# error "Unknown yang-library revision!"
#endif

#include "../modules/sysrepo_monitoring_yang.h"
#include "../modules/ietf_netconf_yang.h"
#include "../modules/ietf_netconf_with_defaults_yang.h"
#include "../modules/ietf_netconf_notifications_yang.h"
#include "../modules/ietf_origin_yang.h"

static sr_error_info_t *sr_lydmods_add_data_deps_r(struct lyd_node *sr_mod, const struct lysc_node *data_root, int output,
        struct lyd_node *sr_deps);

sr_error_info_t *
sr_lydmods_exists(int *exists)
{
    sr_error_info_t *err_info = NULL;
    char *path = NULL;

    /* get internal startup file path */
    if ((err_info = sr_path_startup_file(SR_YANG_MOD, &path))) {
        goto cleanup;
    }

    /* check the existence of the data file */
    if (access(path, F_OK) == -1) {
        if (errno != ENOENT) {
            SR_ERRINFO_SYSERRNO(&err_info, "access");
            goto cleanup;
        }
        *exists = 0;
    } else {
        *exists = 1;
    }

cleanup:
    free(path);
    return err_info;
}

sr_error_info_t *
sr_lydmods_print(struct lyd_node **sr_mods)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *sr_ly_mod;
    char *path;
    mode_t um;

    assert(sr_mods && *sr_mods && !strcmp((*sr_mods)->schema->module->name, SR_YANG_MOD));

    /* get the module */
    sr_ly_mod = (*sr_mods)->schema->module;

    /* validate */
    if (lyd_validate_module(sr_mods, sr_ly_mod, 0, NULL)) {
        sr_errinfo_new_ly(&err_info, sr_ly_mod->ctx);
        return err_info;
    }

    /* get path */
    if ((err_info = sr_path_startup_file(SR_YANG_MOD, &path))) {
        return err_info;
    }

    /* set umask so that the correct permissions are set in case this file does not exist */
    um = umask(00000);

    /* store the data tree */
    if (lyd_print_path(path, *sr_mods, LYD_LYB, LYD_PRINT_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, sr_ly_mod->ctx);
    }
    umask(um);
    free(path);

    return err_info;
}

/**
 * @brief Add inverse dependency node but only if there is not already similar one.
 *
 * @param[in] sr_mod Module with the inverse dependency.
 * @param[in] inv_dep_mod Name of the module that depends on @p sr_mod.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_add_inv_data_dep(struct lyd_node *sr_mod, const char *inv_dep_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *node;

    /* does it exist already? */
    LY_LIST_FOR(LYD_CHILD(sr_mod), node) {
        if (strcmp(node->schema->name, "inverse-data-deps")) {
            continue;
        }

        if (!strcmp(SR_LY_TERM_VALUE(node), inv_dep_mod)) {
            /* exists already */
            return NULL;
        }
    }

    if (lyd_new_term(sr_mod, NULL, "inverse-data-deps", inv_dep_mod, NULL)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mod));
    }

    return err_info;
}

/**
 * @brief Add module into sysrepo module data.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @param[in] ly_mod Module to add.
 * @param[out] sr_mod_p Added module.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_add_module(struct lyd_node *sr_mods, const struct lys_module *ly_mod, struct lyd_node **sr_mod_p)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mod;
    LY_ARRAY_COUNT_TYPE u;

    if (lyd_new_list(sr_mods, NULL, "module", &sr_mod, ly_mod->name)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        return err_info;
    }
    if (ly_mod->revision && lyd_new_term(sr_mod, NULL, "revision", ly_mod->revision, NULL)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        return err_info;
    }

    /* enable all the features */
    LY_ARRAY_FOR(ly_mod->compiled->features, u) {
        if (ly_mod->compiled->features[u].flags & LYS_FENABLED) {
            if (lyd_new_term(sr_mod, NULL, "enabled-feature", ly_mod->compiled->features[u].name, NULL)) {
                sr_errinfo_new_ly(&err_info, ly_mod->ctx);
                return err_info;
            }
        }
    }

    if (sr_mod_p) {
        *sr_mod_p = sr_mod;
    }
    return NULL;
}

/**
 * @brief Add module and all of its implemented imports into sysrepo module data (if not there already), recursively.
 * All new modules have their data files created and YANG modules stored as well.
 *
 * @param[in] sr_mods Internal sysrepo data.
 * @param[in] ly_mod Module with implemented imports to add.
 * @param[in] log_first If set to 0, nothing will be logged on success. Set to 2 to log installing module
 * and its dependencies.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_add_module_with_imps_r(struct lyd_node *sr_mods, const struct lys_module *ly_mod, int log_first)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mod;
    const struct lysp_submodule *lysp_submod;
    struct ly_set *set = NULL;
    char *xpath = NULL;
    LY_ARRAY_COUNT_TYPE i, j;

    if ((err_info = sr_store_module_files(ly_mod))) {
        goto cleanup;
    }

    if (ly_mod->implemented) {
        /* check the module was not already added */
        if (asprintf(&xpath, "module[name='%s']", ly_mod->name) == -1) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }
        if (lyd_find_xpath(sr_mods, xpath, &set)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
            goto cleanup;
        } else if (!set->count) {
            /* install the module and create its startup data file */
            if ((err_info = sr_lydmods_add_module(sr_mods, ly_mod, &sr_mod))) {
                goto cleanup;
            }
            if ((err_info = sr_create_startup_file(ly_mod))) {
                goto cleanup;
            }
            if (log_first == 2) {
                SR_LOG_INF("Module \"%s\" was installed.", ly_mod->name);

                /* the rest of the modules will be dependencies */
                --log_first;
            } else if (log_first == 1) {
                SR_LOG_INF("Dependency module \"%s\" was installed.", ly_mod->name);
            }
        } /* else module has already been added */
    }

    /* all newly implemented modules will be added also from imports and includes, recursively */
    LY_ARRAY_FOR(ly_mod->parsed->imports, i) {
        if ((err_info = sr_lydmods_add_module_with_imps_r(sr_mods, ly_mod->parsed->imports[i].module, log_first))) {
            goto cleanup;
        }
    }

    LY_ARRAY_FOR(ly_mod->parsed->includes, i) {
        lysp_submod = ly_mod->parsed->includes[i].submodule;
        LY_ARRAY_FOR(lysp_submod->imports, j) {
            if ((err_info = sr_lydmods_add_module_with_imps_r(sr_mods, lysp_submod->imports[j].module, log_first))) {
                goto cleanup;
            }
        }
    }

cleanup:
    free(xpath);
    ly_set_free(set, NULL);
    return err_info;
}

/**
 * @brief Add (collect) operation data dependencies into internal sysrepo data.
 *
 * @param[in] sr_mod Module of the data.
 * @param[in] op_root Root node of the operation data to inspect.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_add_op_deps(struct lyd_node *sr_mod, const struct lysc_node *op_root)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_op_deps, *ly_cur_deps;
    struct ly_set *set = NULL;
    struct lysc_action *act;
    char *data_path = NULL, *xpath = NULL;
    const struct ly_ctx *ly_ctx = op_root->module->ctx;

    assert(op_root->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF));

    data_path = lysc_path(op_root, LYSC_PATH_DATA, NULL, 0);
    SR_CHECK_MEM_GOTO(!data_path, err_info, cleanup);
    if (asprintf(&xpath, "op-deps[xpath='%s']", data_path) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }

    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mod, xpath, &set), err_info, cleanup);
    if (set->count == 1) {
        /* already exists */
        goto cleanup;
    }
    assert(!set->count);

    if (lyd_new_inner(sr_mod, NULL, "op-deps", &sr_op_deps)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* operation dep xpath */
    if (lyd_new_term(sr_op_deps, NULL, "xpath", data_path, NULL)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* collect dependencies of nested data and put them into correct containers */
    switch (op_root->nodetype) {
    case LYS_NOTIF:
        if (lyd_new_inner(sr_op_deps, NULL, "in", &ly_cur_deps)) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            goto cleanup;
        }

        if ((err_info = sr_lydmods_add_data_deps_r(sr_mod, op_root, 0, ly_cur_deps))) {
            goto cleanup;
        }
        break;
    case LYS_RPC:
    case LYS_ACTION:
        act = (struct lysc_action *)op_root;

        /* input */
        if (lyd_new_inner(sr_op_deps, NULL, "in", &ly_cur_deps)) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            goto cleanup;
        }
        if ((err_info = sr_lydmods_add_data_deps_r(sr_mod, act->input.data, 0, ly_cur_deps))) {
            goto cleanup;
        }

        /* output */
        if (lyd_new_inner(sr_op_deps, NULL, "out", &ly_cur_deps)) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            goto cleanup;
        }
        if ((err_info = sr_lydmods_add_data_deps_r(sr_mod, act->output.data, 1, ly_cur_deps))) {
            goto cleanup;
        }
        break;
    default:
        SR_ERRINFO_INT(&err_info);
        goto cleanup;
    }

cleanup:
    ly_set_free(set, NULL);
    free(data_path);
    free(xpath);
    return err_info;
}

/**
 * @brief Add a dependency into internal sysrepo data.
 *
 * @param[in] sr_deps Internal sysrepo data dependencies to add to.
 * @param[in] dep_type Dependency type.
 * @param[in] mod_name Name of the module with the dependency.
 * @param[in] node Node causing the dependency.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_moddep_add(struct lyd_node *sr_deps, sr_mod_dep_type_t dep_type, const char *mod_name, const struct lysc_node *node)
{
    const struct lysc_node *data_child;
    char *data_path = NULL, *expr = NULL;
    struct lyd_node *sr_instid;
    struct ly_set *set = NULL;
    sr_error_info_t *err_info = NULL;

    assert(((dep_type == SR_DEP_REF) && mod_name) || ((dep_type == SR_DEP_INSTID) && node));

    if (dep_type == SR_DEP_REF) {
        if (asprintf(&expr, "module[.='%s']", mod_name) == -1) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }
    } else {
        /* find the instance node(s) */
        switch (node->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_CONTAINER:
        case LYS_LIST:
        case LYS_ANYDATA:
        case LYS_ANYXML:
        case LYS_NOTIF:
            /* data-instantiable nodes, we are fine */
            break;
        case LYS_CHOICE:
        case LYS_CASE:
            /* not data-instantiable nodes, we need to find all such nodes */
            assert(dep_type != SR_DEP_INSTID);
            data_child = NULL;
            while ((data_child = lys_getnext(data_child, node, NULL, LYS_GETNEXT_NOSTATECHECK))) {
                if ((err_info = sr_lydmods_moddep_add(sr_deps, dep_type, mod_name, data_child))) {
                    goto cleanup;
                }
            }
            return NULL;
        default:
            SR_ERRINFO_INT(&err_info);
            goto cleanup;
        }

        /* create xpath of the node */
        data_path = lysc_path(node, LYSC_PATH_DATA, NULL, 0);
        if (!data_path || (asprintf(&expr, "inst-id[xpath='%s']", data_path) == -1)) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }
    }

    /* check that there is not a duplicity */
    if (lyd_find_xpath(sr_deps, expr, &set)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_deps));
        goto cleanup;
    } else if (set->count > 1) {
        SR_ERRINFO_INT(&err_info);
        goto cleanup;
    } else if (set->count) {
        /* already exists */
        goto cleanup;
    }

    /* create new dependency */
    if (dep_type == SR_DEP_REF) {
        if (lyd_new_term(sr_deps, NULL, "module", mod_name, NULL)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_deps));
            goto cleanup;
        }
    } else {
        if (lyd_new_inner(sr_deps, NULL, "inst-id", &sr_instid)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_deps));
            goto cleanup;
        }
        if (lyd_new_term(sr_instid, NULL, "xpath", data_path, NULL)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_deps));
            goto cleanup;
        }
        if (mod_name && lyd_new_term(sr_instid, NULL, "default-module", mod_name, NULL)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_deps));
            goto cleanup;
        }
    }

cleanup:
    ly_set_free(set, NULL);
    free(expr);
    free(data_path);
    return err_info;
}

/**
 * @brief Check whether an atom (node) is foreign with respect to the expression.
 *
 * @param[in] atom Node to check.
 * @param[in] top_node Top-level node for the expression.
 * @return Foreign dependency module, NULL if atom is not foreign.
 */
static struct lys_module *
sr_lydmods_moddep_expr_atom_is_foreign(const struct lysc_node *atom, const struct lysc_node *top_node)
{
    assert(atom && top_node && (top_node->parent || (top_node->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF))));

    while (atom->parent && (atom != top_node)) {
        atom = atom->parent;
    }

    if (atom == top_node) {
        /* shared parent, local node */
        return NULL;
    }

    if (top_node->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF)) {
        /* outside operation, foreign node */
        return (struct lys_module *)atom->module;
    }

    if (atom->module != top_node->module) {
        /* foreing top-level node module (so cannot be augment), foreign node */
        return (struct lys_module *)atom->module;
    }

    /* same top-level modules, local node */
    return NULL;
}

/**
 * @brief Collect dependencies from an XPath expression.
 *
 * @param[in] ctx_node Expression context node.
 * @param[in] expr Expression.
 * @param[in] lyxp_opt libyang lyxp options.
 * @param[out] dep_mods Array of dependent modules.
 * @param[out] dep_mod_count Dependent module count.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_moddep_expr_get_dep_mods(const struct lysc_node *ctx_node, const struct lyxp_expr *expr, int lyxp_opt,
        struct lys_module ***dep_mods, size_t *dep_mod_count)
{
    sr_error_info_t *err_info = NULL;
    struct ly_set *set;
    const struct lysc_node *top_node;
    struct lys_module *dep_mod;
    size_t i, j;

    /* find out if we are in an operation, otherwise simply find top-level node */
    top_node = ctx_node;
    while (!(top_node->nodetype & (LYS_ACTION | LYS_NOTIF)) && top_node->parent) {
        top_node = top_node->parent;
    }

    /* get all atoms of the XPath condition */
    if (lys_atomize_xpath(ctx_node, lyxp_get_expr(expr), lyxp_opt, &set)) {
        sr_errinfo_new_ly(&err_info, ctx_node->module->ctx);
        return err_info;
    }

    /* find all top-level foreign nodes (augment nodes are not considered foreign now) */
    for (i = 0; i < set->count; ++i) {
        if ((dep_mod = sr_lydmods_moddep_expr_atom_is_foreign(set->snodes[i], top_node))) {
            /* check for duplicities */
            for (j = 0; j < *dep_mod_count; ++j) {
                if ((*dep_mods)[j] == dep_mod) {
                    break;
                }
            }

            /* add a new dependency module */
            if (j == *dep_mod_count) {
                *dep_mods = sr_realloc(*dep_mods, (*dep_mod_count + 1) * sizeof **dep_mods);
                if (!*dep_mods) {
                    *dep_mod_count = 0;
                    SR_ERRINFO_MEM(&err_info);
                    goto cleanup;
                }

                (*dep_mods)[*dep_mod_count] = dep_mod;
                ++(*dep_mod_count);
            }
        }
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    return err_info;
}

/**
 * @brief Collect dependencies from a type.
 *
 * @param[in] type Type to inspect.
 * @param[in] node Type node.
 * @param[in] sr_deps Internal sysrepo data dependencies to add to.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_moddep_type(const struct lysc_type *type, const struct lysc_node *node, struct lyd_node *sr_deps)
{
    sr_error_info_t *err_info = NULL;
    const struct lysc_type_union *uni;
    LY_ARRAY_COUNT_TYPE u;
    struct lys_module **dep_mods = NULL;
    size_t dep_mod_count = 0;

    switch (type->basetype) {
    case LY_TYPE_INST:
        if ((node->nodetype == LYS_LEAF) && ((struct lysc_node_leaf *)node)->dflt) {
            if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(node,
                    ((struct lysc_node_leaf *)node)->dflt->canonical_cache, 0, &dep_mods, &dep_mod_count))) {
                return err_info;
            }
            assert(dep_mod_count < 2);
        }

        err_info = sr_lydmods_moddep_add(sr_deps, SR_DEP_INSTID, (dep_mod_count ? dep_mods[0]->name : NULL), node);
        free(dep_mods);
        if (err_info) {
            return err_info;
        }
        break;
    case LY_TYPE_LEAFREF:
        if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(node, ((struct lysc_type_leafref *)type)->path,
                0, &dep_mods, &dep_mod_count))) {
            return err_info;
        }
        assert(dep_mod_count < 2);

        if (dep_mod_count) {
            /* a foregin module is referenced */
            err_info = sr_lydmods_moddep_add(sr_deps, SR_DEP_REF, dep_mods[0]->name, NULL);
            free(dep_mods);
            if (err_info) {
                return err_info;
            }
        }
        break;
    case LY_TYPE_UNION:
        uni = (struct lysc_type_union *)type;
        LY_ARRAY_FOR(uni->types, u) {
            if ((err_info = sr_lydmods_moddep_type(uni->types[u], node, sr_deps))) {
                return err_info;
            }
        }
        break;
    default:
        /* no dependency */
        break;
    }

    return NULL;
}

/**
 * @brief Add (collect) (operation) data dependencies into internal sysrepo data tree
 * starting with a subtree, recursively.
 *
 * @param[in] sr_mod Module of the data from sysrepo data tree.
 * @param[in] data_root Root node of the data to inspect.
 * @param[in] output Whether input or output whould be traversed if @p data_root is an RPC/action.
 * @param[in] sr_deps Internal sysrepo data dependencies to add to.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_add_data_deps_r(struct lyd_node *sr_mod, const struct lysc_node *data_root, int output, struct lyd_node *sr_deps)
{
    sr_error_info_t *err_info = NULL;
    struct lys_module **dep_mods;
    size_t dep_mod_count;
    const struct lysc_node *elem;
    struct lysc_type *type;
    struct lysc_when **when;
    struct lysc_must *musts;
    LY_ARRAY_COUNT_TYPE u;
    int atom_opts;

    LYSC_TREE_DFS_BEGIN(data_root, elem) {
        /* skip disabled nodes */
        if (lysc_node_is_disabled(elem, 0)) {
            LYSC_TREE_DFS_continue = 1;
        } else {
            type = NULL;
            when = NULL;
            musts = NULL;
            dep_mods = NULL;
            dep_mod_count = 0;
            atom_opts = LYXP_SCNODE_SCHEMA;

            switch (elem->nodetype) {
            case LYS_LEAF:
                type = ((struct lysc_node_leaf *)elem)->type;
                when = ((struct lysc_node_leaf *)elem)->when;
                musts = ((struct lysc_node_leaf *)elem)->musts;
                break;
            case LYS_LEAFLIST:
                type = ((struct lysc_node_leaflist *)elem)->type;
                when = ((struct lysc_node_leaflist *)elem)->when;
                musts = ((struct lysc_node_leaflist *)elem)->musts;
                break;
            case LYS_CONTAINER:
                when = ((struct lysc_node_container *)elem)->when;
                musts = ((struct lysc_node_container *)elem)->musts;
                break;
            case LYS_CHOICE:
                when = ((struct lysc_node_choice *)elem)->when;
                break;
            case LYS_LIST:
                when = ((struct lysc_node_list *)elem)->when;
                musts = ((struct lysc_node_list *)elem)->musts;
                break;
            case LYS_ANYDATA:
            case LYS_ANYXML:
                when = ((struct lysc_node_anydata *)elem)->when;
                musts = ((struct lysc_node_anydata *)elem)->musts;
                break;
            case LYS_CASE:
                when = ((struct lysc_node_case *)elem)->when;
                break;
            case LYS_RPC:
            case LYS_ACTION:
                if (elem == data_root) {
                    /* handling the specific RPC/action dependencies */
                    if (output) {
                        musts = ((struct lysc_action *)elem)->input.musts;
                        atom_opts = LYXP_SCNODE_OUTPUT;
                    } else {
                        musts = ((struct lysc_action *)elem)->output.musts;
                    }
                } else {
                    /* operation, put the dependencies separately */
                    if ((err_info = sr_lydmods_add_op_deps(sr_mod, elem))) {
                        return err_info;
                    }
                    LYSC_TREE_DFS_continue = 1;
                }
                break;
            case LYS_NOTIF:
                if (elem == data_root) {
                    /* handling the specific notification dependencies */
                    musts = ((struct lysc_notif *)elem)->musts;
                } else {
                    /* operation, put the dependencies separately */
                    if ((err_info = sr_lydmods_add_op_deps(sr_mod, elem))) {
                        return err_info;
                    }
                    LYSC_TREE_DFS_continue = 1;
                }
                break;
            default:
                SR_ERRINFO_INT(&err_info);
                return err_info;
            }

            /* collect the dependencies */
            if (type) {
                if ((err_info = sr_lydmods_moddep_type(type, elem, sr_deps))) {
                    return err_info;
                }
            }
            LY_ARRAY_FOR(when, u) {
                if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(elem, when[u]->cond, atom_opts, &dep_mods,
                        &dep_mod_count))) {
                    return err_info;
                }
            }
            LY_ARRAY_FOR(musts, u) {
                if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(elem, musts[u].cond, atom_opts, &dep_mods,
                        &dep_mod_count))) {
                    free(dep_mods);
                    return err_info;
                }
            }

            /* add those collected from when and must */
            for (u = 0; u < dep_mod_count; ++u) {
                if ((err_info = sr_lydmods_moddep_add(sr_deps, SR_DEP_REF, dep_mods[u]->name, NULL))) {
                    free(dep_mods);
                    return err_info;
                }
            }
            free(dep_mods);
        }

        LYSC_TREE_DFS_END(data_root, elem);
    }

    return NULL;
}

/**
 * @brief Add all data, operational, and inverse dependencies into internal sysrepo data tree.
 *
 * @param[in] sr_mod Module data node from sysrepo data tree.
 * @param[in] ly_mod Parsed libyang module.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_add_all_deps(struct lyd_node *sr_mod, const struct lys_module *ly_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lysc_node *root;
    struct ly_set *set = NULL, *set2;
    struct lyd_node *ly_data_deps;
    uint16_t i;
    char *xpath;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR lyrc;

#ifndef NDEBUG
    /* there can be no dependencies yet */
    assert(!lyd_find_xpath(sr_mod, "data-deps | op-deps", &set));
    assert(set && !set->count);
    ly_set_free(set, NULL);
    set = NULL;
#endif

    /* create new data deps */
    if (lyd_new_inner(sr_mod, NULL, "data-deps", &ly_data_deps)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        goto cleanup;
    }

    /* add data, RPC, notif deps */
    LY_LIST_FOR(ly_mod->compiled->data, root) {
        if ((err_info = sr_lydmods_add_data_deps_r(sr_mod, root, 0, ly_data_deps))) {
            goto cleanup;
        }
    }
    LY_ARRAY_FOR(ly_mod->compiled->rpcs, u) {
        if ((err_info = sr_lydmods_add_data_deps_r(sr_mod, (struct lysc_node *)&ly_mod->compiled->rpcs[u], 0, ly_data_deps))) {
            goto cleanup;
        }
    }
    LY_ARRAY_FOR(ly_mod->compiled->notifs, u) {
        if ((err_info = sr_lydmods_add_data_deps_r(sr_mod, (struct lysc_node *)&ly_mod->compiled->notifs[u], 0,
                ly_data_deps))) {
            goto cleanup;
        }
    }

    /* add inverse data deps */
    if (lyd_find_xpath(sr_mod, "data-deps/module", &set)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        goto cleanup;
    }

    for (i = 0; i < set->count; ++i) {
        if (asprintf(&xpath, "module[name='%s']", SR_LY_TERM_VALUE(set->dnodes[i])) == -1) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }

        /* find the dependent module */
        lyrc = lyd_find_xpath(LYD_PARENT(sr_mod), xpath, &set2);
        free(xpath);
        if (lyrc) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }
        assert(set2->count == 1);

        /* add inverse dependency */
        err_info = sr_lydmods_add_inv_data_dep(set2->dnodes[0], SR_LY_CHILD_VALUE(sr_mod));
        ly_set_free(set2, NULL);
        if (err_info) {
            goto cleanup;
        }
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    return err_info;
}

sr_error_info_t *
sr_lydmods_create(struct ly_ctx *ly_ctx, struct lyd_node **sr_mods_p)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    struct lyd_node *sr_mods = NULL;
    uint32_t i;

#define SR_INSTALL_INT_MOD(yang_mod, dep) \
    if (lys_parse_mem(ly_ctx, yang_mod, LYS_IN_YANG, &ly_mod)) { \
        sr_errinfo_new_ly(&err_info, ly_ctx); \
        goto error; \
    } \
    if ((err_info = sr_lydmods_add_module_with_imps_r(sr_mods, ly_mod, 0))) { \
        goto error; \
    } \
    SR_LOG_INF("Sysrepo internal%s module \"%s\" was installed.", dep ? " dependency" : "", ly_mod->name)

    ly_mod = ly_ctx_get_module_implemented(ly_ctx, SR_YANG_MOD);
    SR_CHECK_INT_RET(!ly_mod, err_info);

    /* create empty container */
    SR_CHECK_INT_RET(lyd_new_inner(NULL, ly_mod, "sysrepo-modules", &sr_mods), err_info);

    /* for internal libyang modules create files and store in the persistent module data tree */
    i = 0;
    while ((i < ly_ctx_internal_module_count(ly_ctx)) && (ly_mod = ly_ctx_get_module_iter(ly_ctx, &i))) {
        /* module must be implemented */
        if (ly_mod->implemented) {
            if ((err_info = sr_lydmods_add_module_with_imps_r(sr_mods, ly_mod, 0))) {
                goto error;
            }
            SR_LOG_INF("Libyang internal module \"%s\" was installed.", ly_mod->name);
        }
    }

    /* install ietf-datastores and ietf-yang-library */
    SR_INSTALL_INT_MOD(ietf_datastores_yang, 1);
    SR_INSTALL_INT_MOD(ietf_yang_library_yang, 0);

    /* install sysrepo-monitoring */
    SR_INSTALL_INT_MOD(sysrepo_monitoring_yang, 0);

    /* install ietf-netconf (implemented dependency) and ietf-netconf-with-defaults */
    SR_INSTALL_INT_MOD(ietf_netconf_yang, 1);
    SR_INSTALL_INT_MOD(ietf_netconf_with_defaults_yang, 0);

    /* install ietf-netconf-notifications */
    SR_INSTALL_INT_MOD(ietf_netconf_notifications_yang, 0);

    /* install ietf-origin */
    SR_INSTALL_INT_MOD(ietf_origin_yang, 0);

    *sr_mods_p = sr_mods;
    return NULL;

error:
    lyd_free_all(sr_mods);
    return err_info;

#undef SR_INSTALL_INT_MOD
}

sr_error_info_t *
sr_lydmods_parse(struct ly_ctx *ly_ctx, struct lyd_node **sr_mods_p)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;
    char *path;

    assert(ly_ctx && sr_mods_p);

    /* get internal startup file path */
    if ((err_info = sr_path_startup_file(SR_YANG_MOD, &path))) {
        goto cleanup;
    }

    /* load sysrepo data even if the stored data used an older revision of the sysrepo module */
    if (lyd_parse_data_path(ly_ctx, path, LYD_LYB, LYD_PARSE_LYB_MOD_UPDATE | LYD_PARSE_STRICT | LYD_PARSE_ONLY
            | LYD_PARSE_TRUSTED, 0, &sr_mods)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* success */

cleanup:
    free(path);
    if (err_info) {
        lyd_free_all(sr_mods);
    } else {
        *sr_mods_p = sr_mods;
    }
    return err_info;
}

/**
 * @brief Check dependencies from a type.
 *
 * @param[in] type Type to inspect.
 * @param[in] node Type node.
 * @param[out] dep_mods Array of dependent modules.
 * @param[out] dep_mod_count Dependent module count.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_moddep_check_type(const struct lysc_type *type, const struct lysc_node *node, struct lys_module ***dep_mods,
        size_t *dep_mod_count)
{
    sr_error_info_t *err_info = NULL;
    struct lysc_node_leaf *sleaf;
    struct lysc_node_leaflist *sllist;
    struct lysc_type_union *uni;
    LY_ARRAY_COUNT_TYPE u;

    switch (type->basetype) {
    case LY_TYPE_INST:
        switch (node->nodetype) {
        case LYS_LEAF:
            sleaf = (struct lysc_node_leaf *)node;
            if (sleaf->dflt && (err_info = sr_lydmods_moddep_expr_get_dep_mods(node, sleaf->dflt->canonical_cache, 0,
                    dep_mods, dep_mod_count))) {
                return err_info;
            }
            break;
        case LYS_LEAFLIST:
            sllist = (struct lysc_node_leaflist *)node;
            LY_ARRAY_FOR(sllist->dflts, u) {
                if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(node, sllist->dflts[u]->canonical_cache, 0,
                        dep_mods, dep_mod_count))) {
                    return err_info;
                }
            }
            break;
        default:
            break;
        }
        break;
    case LY_TYPE_UNION:
        uni = (struct lysc_type_union *)type;
        LY_ARRAY_FOR(uni->types, u) {
            if ((err_info = sr_lydmods_moddep_check_type(uni->types[u], node, dep_mods, dep_mod_count))) {
                return err_info;
            }
        }
        break;
    default:
        /* no dependency, leafref must be handled by libyang */
        break;
    }

    return NULL;
}

static sr_error_info_t *
sr_lydmods_check_deps_r(const struct lysc_node *root, struct lys_module ***dep_mods, size_t *dep_mod_count)
{
    sr_error_info_t *err_info = NULL;
    const struct lysc_node *elem;
    struct lysc_type *type;
    struct lysc_when **when;
    struct lysc_must *musts;
    int input, atom_opts;
    LY_ARRAY_COUNT_TYPE u;

    LYSC_TREE_DFS_BEGIN(root, elem) {
        /* skip disabled nodes */
        if (lysc_node_is_disabled(elem, 0)) {
            LYSC_TREE_DFS_continue = 1;
        } else {
            type = NULL;
            when = NULL;
            musts = NULL;
            input = 0;
            atom_opts = LYXP_SCNODE_SCHEMA;

            switch (elem->nodetype) {
            case LYS_LEAF:
                type = ((struct lysc_node_leaf *)elem)->type;
                when = ((struct lysc_node_leaf *)elem)->when;
                musts = ((struct lysc_node_leaf *)elem)->musts;
                break;
            case LYS_LEAFLIST:
                type = ((struct lysc_node_leaflist *)elem)->type;
                when = ((struct lysc_node_leaflist *)elem)->when;
                musts = ((struct lysc_node_leaflist *)elem)->musts;
                break;
            case LYS_CONTAINER:
                when = ((struct lysc_node_container *)elem)->when;
                musts = ((struct lysc_node_container *)elem)->musts;
                break;
            case LYS_CHOICE:
                when = ((struct lysc_node_choice *)elem)->when;
                break;
            case LYS_LIST:
                when = ((struct lysc_node_list *)elem)->when;
                musts = ((struct lysc_node_list *)elem)->musts;
                break;
            case LYS_ANYDATA:
            case LYS_ANYXML:
                when = ((struct lysc_node_anydata *)elem)->when;
                musts = ((struct lysc_node_anydata *)elem)->musts;
                break;
            case LYS_CASE:
                when = ((struct lysc_node_case *)elem)->when;
                break;
            case LYS_RPC:
            case LYS_ACTION:
                input = 1;
                musts = ((struct lysc_action *)elem)->input.musts;
                break;
            case LYS_NOTIF:
                musts = ((struct lysc_notif *)elem)->musts;
                break;
            default:
                SR_ERRINFO_INT(&err_info);
                return err_info;
            }

collect_deps:
            /* collect the dependencies */
            if (type) {
                if ((err_info = sr_lydmods_moddep_check_type(type, elem, dep_mods, dep_mod_count))) {
                    return err_info;
                }
            }
            LY_ARRAY_FOR(when, u) {
                if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(elem, when[u]->cond, atom_opts, dep_mods,
                        dep_mod_count))) {
                    return err_info;
                }
            }
            LY_ARRAY_FOR(musts, u) {
                if ((err_info = sr_lydmods_moddep_expr_get_dep_mods(elem, musts[u].cond, atom_opts, dep_mods,
                        dep_mod_count))) {
                    return err_info;
                }
            }

            if (input) {
                /* collect deps for output as well */
                musts = ((struct lysc_action *)elem)->output.musts;
                input = 0;
                atom_opts = LYXP_SCNODE_OUTPUT;
                goto collect_deps;
            }
        }

        LYSC_TREE_DFS_END(root, elem);
    }

    return LY_SUCCESS;
}

/**
 * @brief Check data dependencies of a module.
 *
 * @param[in] ly_mod Libyang module to check.
 * @param[in] sr_mods Sysrepo module data.
 * @param[out] fail Whether any dependant module was not implemented.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_check_all_deps(const struct lys_module *ly_mod, const struct lyd_node *sr_mods, int *fail)
{
    sr_error_info_t *err_info = NULL;
    struct lys_module **dep_mods = NULL;
    size_t dep_mod_count = 0;
    const struct lysc_node *root;
    char *xpath;
    struct ly_set *set;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR lyrc;

    /* data, RPCs, notifs */
    LY_LIST_FOR(ly_mod->compiled->data, root) {
        if ((err_info = sr_lydmods_check_deps_r(root, &dep_mods, &dep_mod_count))) {
            goto cleanup;
        }
    }
    LY_ARRAY_FOR(ly_mod->compiled->rpcs, u) {
        if ((err_info = sr_lydmods_check_deps_r((struct lysc_node *)&ly_mod->compiled->rpcs[u], &dep_mods,
                &dep_mod_count))) {
            goto cleanup;
        }
    }
    LY_ARRAY_FOR(ly_mod->compiled->notifs, u) {
        if ((err_info = sr_lydmods_check_deps_r((struct lysc_node *)&ly_mod->compiled->notifs[u], &dep_mods,
                &dep_mod_count))) {
            goto cleanup;
        }
    }

    /* check all the dependency modules */
    for (u = 0; u < dep_mod_count; ++u) {
        if (!dep_mods[u]->implemented) {
            /* maybe it is scheduled to be installed? */
            if (asprintf(&xpath, "installed-module[name='%s']", dep_mods[u]->name) == -1) {
                SR_ERRINFO_MEM(&err_info);
                goto cleanup;
            }
            lyrc = lyd_find_xpath(sr_mods, xpath, &set);
            free(xpath);
            if (lyrc) {
                sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
                goto cleanup;
            }
            assert(set->count < 2);

            if (!set->count) {
                SR_LOG_WRN("Module \"%s\" depends on module \"%s\", which is not implemented.", ly_mod->name,
                        dep_mods[u]->name);
                *fail = 1;
            }
            ly_set_free(set, NULL);
        }
    }

cleanup:
    free(dep_mods);
    return err_info;
}

/**
 * @brief Load new installed modules into context from sysrepo module data.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @param[in] new_ctx Context to load the new modules into.
 * @param[out] change Whether any new modules were loaded.
 * @param[out] fail Whether any new dependant modules were not implemented.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_ctx_install_modules(const struct lyd_node *sr_mods, struct ly_ctx *new_ctx, int *change, int *fail)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    struct ly_set *set = NULL, *feat_set = NULL;
    uint32_t i, j;

    assert(sr_mods);

    if (lyd_find_xpath(sr_mods, "/" SR_YANG_MOD ":sysrepo-modules/installed-module/module-yang", &set)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
        goto cleanup;
    }
    for (i = 0; i < set->count; ++i) {
        /* load the new module, it can still fail on, for example, duplicate namespace */
        if (lys_parse_mem(new_ctx, SR_LY_TERM_VALUE(set->dnodes[i]), LYS_IN_YANG, &ly_mod)) {
            sr_log_wrn_ly(new_ctx);
            SR_LOG_WRN("Installing module \"%s\" failed.", SR_LY_CHILD_VALUE(set->dnodes[i]));
            *fail = 1;
            goto cleanup;
        }

        /* collect all enabled features */
        if (lyd_find_xpath(set->dnodes[i], "enabled-feature", &feat_set)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
            goto cleanup;
        }

        /* enable all the features */
        for (j = 0; j < feat_set->count; ++j) {
            if (lys_feature_enable_force(ly_mod, SR_LY_TERM_VALUE(feat_set->dnodes[j]))) {
                sr_errinfo_new_ly(&err_info, new_ctx);
                goto cleanup;
            }
        }

        /* check that all the dependant modules are implemented */
        if ((err_info = sr_lydmods_check_all_deps(ly_mod, sr_mods, fail)) || *fail) {
            goto cleanup;
        }

        ly_set_free(feat_set, NULL);
        feat_set = NULL;
        *change = 1;
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    ly_set_free(feat_set, NULL);
    return err_info;
}

/**
 * @brief Load updated modules into context.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @param[in] new_ctx Context to load updated modules into.
 * @param[out] change Whether there were any updated modules.
 * @param[out] fail Whether any new dependant modules were not implemented.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_ctx_update_modules(const struct lyd_node *sr_mods, struct ly_ctx *new_ctx, int *change, int *fail)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    struct ly_set *set = NULL, *feat_set = NULL;
    uint32_t i, j;

    assert(sr_mods);

    /* find updated modules and change internal module data tree */
    if (lyd_find_xpath(sr_mods, "/" SR_YANG_MOD ":sysrepo-modules/module/updated-yang", &set)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
        goto cleanup;
    }
    for (i = 0; i < set->count; ++i) {
        /* load the updated module */
        if (lys_parse_mem(new_ctx, SR_LY_TERM_VALUE(set->dnodes[i]), LYS_IN_YANG, &ly_mod)) {
            sr_errinfo_new_ly(&err_info, new_ctx);
            goto cleanup;
        }

        /* collect all enabled features */
        if (lyd_find_xpath(LYD_PARENT(set->dnodes[i]), "enabled-feature", &feat_set)) {
            sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
            goto cleanup;
        }

        /* enable all the features */
        for (j = 0; j < feat_set->count; ++j) {
            if (lys_feature_enable_force(ly_mod, SR_LY_TERM_VALUE(feat_set->dnodes[j]))) {
                sr_errinfo_new_ly(&err_info, new_ctx);
                goto cleanup;
            }
        }
        ly_set_free(feat_set, NULL);
        feat_set = NULL;

        /* check that all the dependant modules are implemented */
        if ((err_info = sr_lydmods_check_all_deps(ly_mod, sr_mods, fail)) || *fail) {
            goto cleanup;
        }

        *change = 1;
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    ly_set_free(feat_set, NULL);
    return err_info;
}

/**
 * @brief Update context module features based on sysrepo module data.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @param[in] new_ctx Context with modules to update features.
 * @param[out] change Whether there were any feature changes.
 * @param[out] fail Whether any new dependant modules were not implemented.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_ctx_change_features(const struct lyd_node *sr_mods, struct ly_ctx *new_ctx, int *change, int *fail)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mod;
    struct lyd_node_inner *inner;
    const struct lys_module *ly_mod, *imp_ly_mod;
    struct ly_set *set = NULL, *feat_set = NULL;
    const char *feat_name, **f_names = NULL;
    uint8_t *f_state_old = NULL, *f_state_new = NULL;
    uint32_t i;
    int enable;
    LY_ARRAY_COUNT_TYPE u;

    assert(sr_mods);

    LY_LIST_FOR(LYD_CHILD(sr_mods), sr_mod) {
        /* find all changed features of the particular module */
        if (lyd_find_xpath(sr_mod, "changed-feature", &set)) {
            SR_ERRINFO_INT(&err_info);
            return err_info;
        } else if (!set->count) {
            /* no changed features */
            ly_set_free(set, NULL);
            set = NULL;
            continue;
        }

        /* get the module */
        ly_mod = ly_ctx_get_module_implemented(new_ctx, SR_LY_CHILD_VALUE(sr_mod));
        if (!ly_mod) {
            /* this can happen only if the module is also scheduled to be removed */
#ifndef NDEBUG
            struct lyd_node *node;
            LY_LIST_FOR(LYD_CHILD(sr_mod), node) {
                if (!strcmp(node->schema->name, "removed")) {
                    break;
                }
            }
            assert(node);
#endif
            SR_LOG_WRN("Module \"%s\" is scheduled for both removal and feature changes, ignoring them.",
                    SR_LY_CHILD_VALUE(sr_mod));
            ly_set_free(set, NULL);
            set = NULL;
            continue;
        }

        /* update the features */
        for (i = 0; i < set->count; ++i) {
            inner = set->objs[i];
            assert(!strcmp(inner->child->schema->name, "name"));
            assert(!strcmp(inner->child->next->schema->name, "change"));
            feat_name = SR_LY_TERM_VALUE(inner->child);
            enable = !strcmp(SR_LY_TERM_VALUE(inner->child->next), "enable") ? 1 : 0;

            if (enable && lys_feature_enable_force(ly_mod, feat_name)) {
                sr_errinfo_new_ly(&err_info, ly_mod->ctx);
                SR_ERRINFO_INT(&err_info);
                goto cleanup;
            } else if (!enable && lys_feature_disable_force(ly_mod, feat_name)) {
                sr_errinfo_new_ly(&err_info, ly_mod->ctx);
                SR_ERRINFO_INT(&err_info);
                goto cleanup;
            }
        }
        ly_set_free(set, NULL);
        set = NULL;

        /* check that all the dependant modules are implemented */
        if ((err_info = sr_lydmods_check_all_deps(ly_mod, sr_mods, fail)) || *fail) {
            goto cleanup;
        }

        /* check that all module dependencies that import this module are implemented */
        i = 0;
        while ((imp_ly_mod = ly_ctx_get_module_iter(ly_mod->ctx, &i))) {
            if ((imp_ly_mod == ly_mod) || !imp_ly_mod->implemented) {
                continue;
            }

            LY_ARRAY_FOR(imp_ly_mod->parsed->imports, u) {
                if (imp_ly_mod->parsed->imports[u].module == ly_mod) {
                    break;
                }
            }
            if (u == LY_ARRAY_COUNT(imp_ly_mod->parsed->imports)) {
                continue;
            }

            if ((err_info = sr_lydmods_check_all_deps(imp_ly_mod, sr_mods, fail)) || *fail) {
                goto cleanup;
            }
        }

        *change = 1;
    }

    /* success */

cleanup:
    free(f_names);
    free(f_state_old);
    free(f_state_new);
    ly_set_free(set, NULL);
    ly_set_free(feat_set, NULL);
    return err_info;
}

/**
 * @brief Check whether some removed module is not a dependency of a non-removed module.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @param[in] new_ctx Context with all scheduled module changes applied.
 * @param[out] fail Whether any scheduled module removal failed.
 * @return err_info, NULL on error.
 */
static sr_error_info_t *
sr_lydmods_sched_check_removed_modules(const struct lyd_node *sr_mods, const struct ly_ctx *new_ctx, int *fail)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *node;
    struct ly_set *set = NULL;
    const char *mod_name, *revision;
    const struct lys_module *ly_mod;
    uint32_t i;

    assert(sr_mods);

    /* find all removed modules */
    if (lyd_find_xpath(sr_mods, "/" SR_YANG_MOD ":sysrepo-modules/module[removed]", &set)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
        goto cleanup;
    } else if (!set->count) {
        /* nothing to do */
        goto cleanup;
    }

    /* check that the removed modules are not implemented in the new context */
    for (i = 0; i < set->count; ++i) {
        /* learn about the module */
        mod_name = NULL;
        revision = NULL;
        LY_LIST_FOR(LYD_CHILD(set->dnodes[i]), node) {
            if (!strcmp(node->schema->name, "name")) {
                mod_name = SR_LY_TERM_VALUE(node);
            } else if (!strcmp(node->schema->name, "revision")) {
                revision = SR_LY_TERM_VALUE(node);
                break;
            }
        }
        assert(mod_name);

        ly_mod = ly_ctx_get_module(new_ctx, mod_name, revision);
        if (ly_mod && ly_mod->implemented) {
            /* this module cannot be removed */
            SR_LOG_WRN("Cannot remove module \"%s\" because some other installed module depends on it.", mod_name);

            /* we failed, do not apply any scheduled changes */
            *fail = 1;
            goto cleanup;
        }
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    return err_info;
}

/**
 * @brief Load a module into context (if not already there) based on its information from sysrepo module data.
 *
 * @param[in] sr_mod Module from sysrepo mdoule data to load.
 * @param[in] ly_ctx Context to load the module into.
 * @param[out] ly_mod Optionally return the loaded module.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_ctx_load_module(const struct lyd_node *sr_mod, struct ly_ctx *ly_ctx, const struct lys_module **ly_mod_p)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *node;
    const struct lys_module *ly_mod;
    struct ly_set *feat_set = NULL;
    const char *mod_name, *revision;
    uint32_t i;

    /* learn about the module */
    mod_name = NULL;
    revision = NULL;
    LY_LIST_FOR(LYD_CHILD(sr_mod), node) {
        if (!strcmp(node->schema->name, "name")) {
            mod_name = SR_LY_TERM_VALUE(node);
        } else if (!strcmp(node->schema->name, "revision")) {
            revision = SR_LY_TERM_VALUE(node);
            break;
        }
    }
    assert(mod_name);

    /* the module is not supposed to be loaded yet, but is in case of LY internal modules and dependency modules */
    ly_mod = ly_ctx_get_module(ly_ctx, mod_name, revision);
    if (!ly_mod || !ly_mod->implemented) {
        /* load the module */
        ly_mod = ly_ctx_load_module(ly_ctx, mod_name, revision);
    }
    if (!ly_mod) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* collect all currently enabled features */
    if (lyd_find_xpath(sr_mod, "enabled-feature", &feat_set)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mod));
        goto cleanup;
    }

    /* enable all the features */
    for (i = 0; i < feat_set->count; ++i) {
        if (lys_feature_enable_force(ly_mod, SR_LY_TERM_VALUE(feat_set->dnodes[i]))) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            goto cleanup;
        }
    }

    /* success */

cleanup:
    ly_set_free(feat_set, NULL);
    if (!err_info && ly_mod_p) {
        *ly_mod_p = ly_mod;
    }
    return err_info;
}

sr_error_info_t *
sr_lydmods_ctx_load_modules(const struct lyd_node *sr_mods, struct ly_ctx *ly_ctx, int removed, int updated, int *change)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mod, *node;

    LY_LIST_FOR(LYD_CHILD(sr_mods), sr_mod) {
        if (!strcmp(sr_mod->schema->name, "installed-module")) {
            continue;
        }
        if (!removed || !updated) {
            LY_LIST_FOR(LYD_CHILD(sr_mod), node) {
                /* check that the module was not removed or updated */
                if (!removed && !strcmp(node->schema->name, "removed")) {
                    break;
                } else if (!updated && !strcmp(node->schema->name, "updated-yang")) {
                    break;
                }
            }
            if (node) {
                if (change) {
                    *change = 1;
                }
                continue;
            }
        }

        /* load the module */
        if ((err_info = sr_lydmods_ctx_load_module(sr_mod, ly_ctx, NULL))) {
            return err_info;
        }
    }

    return NULL;
}

/**
 * @brief Check that persistent (startup) module data can be loaded into updated context.
 * On success print the new updated LYB data.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @param[in] new_ctx Context with all scheduled module changes.
 * @param[out] fail Whether any data failed to be parsed.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_update_data(const struct lyd_node *sr_mods, const struct ly_ctx *new_ctx, int *fail)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *old_start_data = NULL, *new_start_data = NULL, *old_run_data = NULL, *new_run_data = NULL, *mod_data;
    struct ly_ctx *old_ctx = NULL;
    struct ly_set *set = NULL, *startup_set = NULL;
    const struct lys_module *ly_mod;
    char *start_data_json = NULL, *run_data_json = NULL, *path;
    uint32_t idx;
    int exists;
    LY_ERR lyrc;

    set = ly_set_new();
    SR_CHECK_MEM_GOTO(!set, err_info, cleanup);

    /* first build context without any scheduled changes */
    if ((err_info = sr_ly_ctx_new(&old_ctx))) {
        goto cleanup;
    }
    if ((err_info = sr_lydmods_ctx_load_modules(sr_mods, old_ctx, 1, 1, NULL))) {
        goto cleanup;
    }

    /* parse all the startup/running data using the old context (that must succeed) */
    idx = 0;
    while ((ly_mod = ly_ctx_get_module_iter(old_ctx, &idx))) {
        if (!ly_mod->implemented) {
            /* we need data of only implemented modules */
            continue;
        }

        /* append startup data */
        if ((err_info = sr_module_file_data_append(ly_mod, SR_DS_STARTUP, &old_start_data))) {
            goto cleanup;
        }

        /* check that running data file exists */
        if ((err_info = sr_path_ds_shm(ly_mod->name, SR_DS_RUNNING, 1, &path))) {
            goto cleanup;
        }
        exists = sr_file_exists(path);
        free(path);

        if (exists) {
            /* append running data */
            if ((err_info = sr_module_file_data_append(ly_mod, SR_DS_RUNNING, &old_run_data))) {
                goto cleanup;
            }
        }

        /* remember this module from the new context */
        ly_mod = ly_ctx_get_module_implemented(new_ctx, ly_mod->name);
        if (ly_mod) {
            ly_set_add(set, (void *)ly_mod, LY_SET_OPT_USEASLIST);
        } /* else the module was removed */
    }

    /* print the data of all the modules into JSON */
    if (lyd_print_mem(&start_data_json, old_start_data, LYD_JSON, LYD_PRINT_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, old_ctx);
        goto cleanup;
    }
    if (lyd_print_mem(&run_data_json, old_run_data, LYD_JSON, LYD_PRINT_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, old_ctx);
        goto cleanup;
    }

    /* try to load it into the new updated context skipping any unknown nodes */
    if (lyd_parse_data_mem(new_ctx, start_data_json, LYD_JSON, LYD_PARSE_NO_STATE | LYD_PARSE_ONLY | LYD_PARSE_TRUSTED,
            0, &new_start_data)) {
        /* it failed, some of the scheduled changes are not compatible with the stored data, abort them all */
        sr_log_wrn_ly(new_ctx);
        *fail = 1;
        goto cleanup;
    }
    if (lyd_parse_data_mem(new_ctx, run_data_json, LYD_JSON, LYD_PARSE_NO_STATE | LYD_PARSE_ONLY | LYD_PARSE_TRUSTED,
            0, &new_run_data)) {
        sr_log_wrn_ly(new_ctx);
        *fail = 1;
        goto cleanup;
    }

    /* check that any startup data can be loaded and are valid */
    if (lyd_find_xpath(sr_mods, "installed-module/data", &startup_set)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mods));
        goto cleanup;
    }
    for (idx = 0; idx < startup_set->count; ++idx) {
        /* this was parsed before */
        lyd_parse_data_mem(new_ctx, SR_LY_TERM_VALUE(startup_set->dnodes[idx]), LYD_JSON,
                LYD_PARSE_NO_STATE | LYD_PARSE_STRICT | LYD_PARSE_ONLY | LYD_PARSE_TRUSTED, 0, &mod_data);
        if (!mod_data) {
            continue;
        }

        /* remember this module */
        ly_set_add(set, (void *)lyd_owner_module(mod_data), LY_SET_OPT_USEASLIST);

        /* link to the new startup/running data */
        if (!new_start_data) {
            lyrc = lyd_dup_siblings(mod_data, NULL, LYD_DUP_RECURSIVE | LYD_DUP_WITH_FLAGS, &new_start_data);
            SR_CHECK_MEM_GOTO(lyrc, err_info, cleanup);
        } else if (lyd_merge_siblings(&new_start_data, mod_data, 0)) {
            sr_errinfo_new_ly(&err_info, new_ctx);
            goto cleanup;
        }
        if (!new_run_data) {
            new_run_data = mod_data;
        } else if (lyd_merge_siblings(&new_run_data, mod_data, LYD_MERGE_DESTRUCT)) {
            sr_errinfo_new_ly(&err_info, new_ctx);
            goto cleanup;
        }
    }

    /* fully validate complete startup and running datastore */
    if (lyd_validate_all(&new_start_data, new_ctx, LYD_VALIDATE_NO_STATE, NULL) ||
            lyd_validate_all(&new_run_data, new_ctx, LYD_VALIDATE_NO_STATE, NULL)) {
        sr_log_wrn_ly(new_ctx);
        *fail = 1;
        goto cleanup;
    }

    /* print all modules data with the updated module context and free them, no longer needed */
    for (idx = 0; idx < set->count; ++idx) {
        ly_mod = set->objs[idx];

        /* startup data */
        mod_data = sr_module_data_unlink(&new_start_data, ly_mod);
        if ((err_info = sr_module_file_data_set(ly_mod->name, SR_DS_STARTUP, mod_data, O_CREAT, SR_FILE_PERM))) {
            lyd_free_siblings(mod_data);
            goto cleanup;
        }
        lyd_free_siblings(mod_data);

        /* running data */
        mod_data = sr_module_data_unlink(&new_run_data, ly_mod);
        if ((err_info = sr_module_file_data_set(ly_mod->name, SR_DS_RUNNING, mod_data, O_CREAT, SR_FILE_PERM))) {
            lyd_free_siblings(mod_data);
            goto cleanup;
        }
        lyd_free_siblings(mod_data);
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    ly_set_free(startup_set, NULL);
    lyd_free_siblings(old_start_data);
    lyd_free_siblings(new_start_data);
    lyd_free_siblings(old_run_data);
    lyd_free_siblings(new_run_data);
    free(start_data_json);
    free(run_data_json);
    ly_ctx_destroy(old_ctx, NULL);
    if (err_info) {
        sr_errinfo_new(&err_info, SR_ERR_OPERATION_FAILED, NULL, "Failed to update data for the new context.");
    }
    return err_info;
}

/**
 * @brief Finalize applying scheduled module removal. Meaning remove its data files
 * and module file in case it is not imported by other modules.
 *
 * @param[in] sr_mod Sysrepo module to remove. Will be freed.
 * @param[in] new_ctx Context with the new modules.
 * @param[in] update Whether this function is called from module update or module removal.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_finalize_module_remove(struct lyd_node *sr_mod, const struct ly_ctx *new_ctx, int update)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    const char *mod_name, *mod_rev;
    struct lyd_node *child;
    uint32_t idx;
    LY_ARRAY_COUNT_TYPE u;

    child = LYD_CHILD(sr_mod);
    assert(!strcmp(child->schema->name, "name"));
    mod_name = SR_LY_TERM_VALUE(child);
    if (child->next && !strcmp(child->next->schema->name, "revision")) {
        mod_rev = SR_LY_TERM_VALUE(child->next);
    } else {
        mod_rev = NULL;
    }

    /* remove data files */
    if (!update && (err_info = sr_remove_data_files(mod_name))) {
        return err_info;
    }

    /* check whether it is imported by other modules */
    idx = ly_ctx_internal_module_count(new_ctx);
    while ((ly_mod = ly_ctx_get_module_iter(new_ctx, &idx))) {
        LY_ARRAY_FOR(ly_mod->parsed->imports, u) {
            if (!strcmp(ly_mod->parsed->imports[u].module->name, mod_name)) {
                break;
            }
        }
        if (u < LY_ARRAY_COUNT(ly_mod->parsed->imports)) {
            break;
        }
    }
    if (!ly_mod) {
        /* no module imports the removed one, remove the YANG as well */
        if ((err_info = sr_remove_module_file(mod_name, mod_rev))) {
            return err_info;
        }
    }

    if (!update) {
        SR_LOG_INF("Module \"%s\" was removed.", mod_name);
    }

    /* remove module list instance */
    lyd_free_tree(sr_mod);
    return NULL;
}

/**
 * @brief Finalize applying scheduled module update.
 *
 * @param[in] sr_mod Sysrepo module to update. Will be freed.
 * @param[in] new_ctx Context with the updated module loaded.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_finalize_module_update(struct lyd_node *sr_mod, const struct ly_ctx *new_ctx)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    struct lyd_node *sr_mods;

    sr_mods = LYD_PARENT(sr_mod);

    /* find the updated module in the new context */
    assert(!strcmp(LYD_CHILD(sr_mod)->schema->name, "name"));
    ly_mod = ly_ctx_get_module_implemented(new_ctx, SR_LY_CHILD_VALUE(sr_mod));
    assert(ly_mod);

    /* remove module */
    if ((err_info = sr_lydmods_sched_finalize_module_remove(sr_mod, new_ctx, 1))) {
        return err_info;
    }

    /* re-add it (only the data files are kept) */
    if ((err_info = sr_lydmods_add_module_with_imps_r(sr_mods, ly_mod, 0))) {
        return err_info;
    }

    SR_LOG_INF("Module \"%s\" was updated to revision %s.", ly_mod->name, ly_mod->revision);
    return NULL;
}

/**
 * @brief Finalize applying scheduled module feature changes.
 *
 * @param[in] sr_mod Sysrepo module with feature changes.
 * @param[in] new_ctx Context with new modules used for printing them.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_finalize_module_change_features(struct lyd_node *sr_mod, const struct ly_ctx *new_ctx)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    const char *feat_name;
    struct lyd_node *next, *node;
    struct lyd_node_inner *inner;
    struct ly_set *set;
    int enable;
    char *xpath;
    LY_ERR lyrc;

    assert(!strcmp(LYD_CHILD(sr_mod)->schema->name, "name"));
    ly_mod = ly_ctx_get_module_implemented(new_ctx, SR_LY_CHILD_VALUE(sr_mod));
    assert(ly_mod);

    LY_LIST_FOR_SAFE(LYD_CHILD(sr_mod)->next, next, node) {
        if (!strcmp(node->schema->name, "changed-feature")) {
            /*
             * changed feature
             */
            inner = (struct lyd_node_inner *)node;
            assert(!strcmp(inner->child->schema->name, "name"));
            assert(!strcmp(inner->child->next->schema->name, "change"));

            feat_name = SR_LY_TERM_VALUE(inner->child);
            enable = !strcmp(SR_LY_TERM_VALUE(inner->child->next), "enable") ? 1 : 0;
            lyd_free_tree(node);

            /* update internal sysrepo data tree */
            if (enable) {
                if (lyd_new_path(sr_mod, NULL, "enabled-feature", (void *)feat_name, 0, NULL)) {
                    sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mod));
                    return err_info;
                }
            } else {
                if (asprintf(&xpath, "enabled-feature[.='%s']", feat_name) == -1) {
                    SR_ERRINFO_MEM(&err_info);
                    return err_info;
                }
                lyrc = lyd_find_xpath(sr_mod, xpath, &set);
                free(xpath);
                if (lyrc) {
                    sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(sr_mod));
                    return err_info;
                }
                assert(set->count == 1);
                lyd_free_tree(set->dnodes[0]);
                ly_set_free(set, NULL);
            }

            SR_LOG_INF("Module \"%s\" feature \"%s\" was %s.", ly_mod->name, feat_name, enable ? "enabled" : "disabled");
        }
    }

    return NULL;
}

/**
 * @brief Finalize applying scheduled module installation. That consists of updating
 * sysrepo module data tree and storing updated YANG module files.
 *
 * @param[in] sr_mod Sysrepo module to install. Will be freed.
 * @param[in] new_ctx Context with new modules used for printing them.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_sched_finalize_module_install(struct lyd_node *sr_mod, const struct ly_ctx *new_ctx)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    struct lyd_node *sr_mods, *node;
    LY_ARRAY_COUNT_TYPE u;

    LY_LIST_FOR(sr_mod->next, node) {
        if (strcmp(node->schema->name, "installed-module")) {
            continue;
        }

        assert(!strcmp(LYD_CHILD(node)->schema->name, "name"));
        ly_mod = ly_ctx_get_module_implemented(new_ctx, SR_LY_CHILD_VALUE(node));
        assert(ly_mod);

        LY_ARRAY_FOR(ly_mod->parsed->imports, u) {
            if (ly_mod->parsed->imports[u].module->implemented
                    && !strcmp(ly_mod->parsed->imports[u].module->name, SR_LY_CHILD_VALUE(sr_mod))) {
                /* we will install this module as a dependency of a module installed later */
                SR_LOG_INF("Module \"%s\" will be installed as \"%s\" module dependency.",
                        SR_LY_CHILD_VALUE(sr_mod), ly_mod->name);
                lyd_free_tree(sr_mod);
                return NULL;
            }
        }
    }

    sr_mods = LYD_PARENT(sr_mod);

    /*
     * installed module, store new YANG, install all of its implemented dependencies
     */
    assert(!strcmp(LYD_CHILD(sr_mod)->schema->name, "name"));
    ly_mod = ly_ctx_get_module_implemented(new_ctx, SR_LY_CHILD_VALUE(sr_mod));
    assert(ly_mod);
    lyd_free_tree(sr_mod);

    if ((err_info = sr_lydmods_add_module_with_imps_r(sr_mods, ly_mod, 2))) {
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_lydmods_sched_apply(struct lyd_node *sr_mods, struct ly_ctx *new_ctx, int *change, int *fail)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *next, *next2, *sr_mod, *node;
    const struct lys_module *ly_mod;

    assert(sr_mods && new_ctx && change);

    SR_LOG_INFMSG("Applying scheduled changes.");
    *change = 0;
    *fail = 0;

    /*
     * 1) create the new context, LY sysrepo data are not modified
     */

    /* load updated modules into new context */
    if ((err_info = sr_lydmods_sched_ctx_update_modules(sr_mods, new_ctx, change, fail)) || *fail) {
        goto cleanup;
    }

    /* load all remaining non-updated non-removed modules into new context */
    if ((err_info = sr_lydmods_ctx_load_modules(sr_mods, new_ctx, 0, 0, change))) {
        goto cleanup;
    }

    /* change features */
    if ((err_info = sr_lydmods_sched_ctx_change_features(sr_mods, new_ctx, change, fail)) || *fail) {
        goto cleanup;
    }

    /* install modules */
    if ((err_info = sr_lydmods_sched_ctx_install_modules(sr_mods, new_ctx, change, fail)) || *fail) {
        goto cleanup;
    }

    if (*change) {
        /* check that removed modules can really be removed */
        if ((err_info = sr_lydmods_sched_check_removed_modules(sr_mods, new_ctx, fail)) || *fail) {
            goto cleanup;
        }

        /* check that persistent module data can be loaded with updated modules */
        if ((err_info = sr_lydmods_sched_update_data(sr_mods, new_ctx, fail)) || *fail) {
            goto cleanup;
        }

        /*
         * 2) update LY sysrepo data, dependencies are created from scratch
         */
        LY_LIST_FOR_SAFE(LYD_CHILD(sr_mods), next, sr_mod) {
            if (!strcmp(sr_mod->schema->name, "module")) {
                assert(!strcmp(LYD_CHILD(sr_mod)->schema->name, "name"));
                LY_LIST_FOR_SAFE(LYD_CHILD(sr_mod)->next, next2, node) {
                    if (!strcmp(node->schema->name, "removed")) {
                        if ((err_info = sr_lydmods_sched_finalize_module_remove(sr_mod, new_ctx, 0))) {
                            goto cleanup;
                        }
                        /* sr_mod was freed */
                        break;
                    } else if (!strcmp(node->schema->name, "updated-yang")) {
                        if ((err_info = sr_lydmods_sched_finalize_module_update(sr_mod, new_ctx))) {
                            goto cleanup;
                        }
                        /* sr_mod was freed */
                        break;
                    } else if (!strcmp(node->schema->name, "changed-feature")) {
                        if ((err_info = sr_lydmods_sched_finalize_module_change_features(sr_mod, new_ctx))) {
                            goto cleanup;
                        }
                        /* sr_mod children were freed, iteration cannot continue */
                        break;
                    } else if (!strcmp(node->schema->name, "data-deps")
                            || !strcmp(node->schema->name, "op-deps")
                            || !strcmp(node->schema->name, "inverse-data-deps")) {
                        /* remove all stored dependencies of all the modules */
                        lyd_free_tree(node);
                    }
                }
            } else {
                assert(!strcmp(sr_mod->schema->name, "installed-module"));
                if ((err_info = sr_lydmods_sched_finalize_module_install(sr_mod, new_ctx))) {
                    goto cleanup;
                }
            }
        }

        /* now add (rebuild) all dependencies of all the modules */
        LY_LIST_FOR(LYD_CHILD(sr_mods), sr_mod) {
            ly_mod = ly_ctx_get_module_implemented(new_ctx, SR_LY_CHILD_VALUE(sr_mod));
            assert(ly_mod);
            if ((err_info = sr_lydmods_add_all_deps(sr_mod, ly_mod))) {
                goto cleanup;
            }
        }
    }

    /* success */

cleanup:
    if (!err_info) {
        if (*fail) {
            SR_LOG_WRNMSG("Failed to apply some changes, leaving all changes scheduled.");
            *change = 0;
        } else if (*change) {
            SR_LOG_INFMSG("Scheduled changes applied.");
        } else {
            SR_LOG_INFMSG("No scheduled changes.");
        }
    }
    return err_info;
}

sr_error_info_t *
sr_lydmods_deferred_add_module(struct ly_ctx *ly_ctx, const struct lys_module *ly_mod, const char **features, int feat_count)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL, *inst_mod;
    struct ly_set *set = NULL;
    char *path = NULL, *yang_str = NULL;
    int i;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* check that the module is not already marked for installation */
    if (asprintf(&path, "installed-module[name=\"%s\"]", ly_mod->name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (set->count == 1) {
        sr_errinfo_new(&err_info, SR_ERR_EXISTS, NULL, "Module \"%s\" already scheduled for installation.", ly_mod->name);
        goto cleanup;
    }

    /* store all info for installation */
    if (lyd_new_path(sr_mods, NULL, path, NULL, 0, &inst_mod)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    if (ly_mod->revision && lyd_new_term(inst_mod, NULL, "revision", ly_mod->revision, NULL)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    for (i = 0; i < feat_count; ++i) {
        if (lyd_new_term(inst_mod, NULL, "enabled-feature", features[i], NULL)) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            goto cleanup;
        }
    }

    /* print the module into memory */
    if (lys_print_mem(&yang_str, ly_mod, LYS_OUT_YANG, 0)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        goto cleanup;
    }

    if (lyd_new_term(inst_mod, NULL, "module-yang", yang_str, NULL)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

    SR_LOG_INF("Module \"%s\" scheduled for installation.", ly_mod->name);

cleanup:
    free(path);
    free(yang_str);
    ly_set_free(set, NULL);
    lyd_free_all(sr_mods);
    return err_info;
}

sr_error_info_t *
sr_lydmods_unsched_add_module(struct ly_ctx *ly_ctx, const char *module_name)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;
    struct ly_set *set = NULL;
    char *path = NULL;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* check that the module is scheduled for installation */
    if (asprintf(&path, "installed-module[name=\"%s\"]", module_name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (!set->count) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Module \"%s\" not scheduled for installation.", module_name);
        goto cleanup;
    }

    /* unschedule installation */
    lyd_free_tree(set->dnodes[0]);

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

    SR_LOG_INF("Module \"%s\" installation unscheduled.", module_name);

cleanup:
    free(path);
    ly_set_free(set, NULL);
    lyd_free_all(sr_mods);
    return err_info;
}

sr_error_info_t *
sr_lydmods_ctx_load_installed_module_all(const struct lyd_node *sr_mods, struct ly_ctx *ly_ctx, const char *module_name,
        const struct lys_module **ly_mod_p)
{
    sr_error_info_t *err_info = NULL;
    struct ly_set *set = NULL;
    const struct lys_module *ly_mod;
    uint32_t i;

    *ly_mod_p = NULL;

    /* find all scheduled modules */
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, "installed-module/module-yang", &set), err_info, cleanup);

    /* load all the modules, it must succeed */
    for (i = 0; i < set->count; ++i) {
        if (lys_parse_mem(ly_ctx, SR_LY_TERM_VALUE(set->dnodes[i]), LYS_IN_YANG, &ly_mod)) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            SR_ERRINFO_INT(&err_info);
            goto cleanup;
        }

        /* just enable all features */
        if ((err_info = sr_lydmods_ctx_load_module(LYD_PARENT(set->dnodes[i]), ly_ctx, NULL))) {
            goto cleanup;
        }

        if (!strcmp(ly_mod->name, module_name)) {
            /* the required mdule was found */
            *ly_mod_p = ly_mod;
        }
    }

    if (!*ly_mod_p) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Module \"%s\" not scheduled for installation.", module_name);
        goto cleanup;
    }

    /* success */

cleanup:
    ly_set_free(set, NULL);
    return err_info;
}

sr_error_info_t *
sr_lydmods_deferred_add_module_data(struct lyd_node *sr_mods, const char *module_name, const struct lyd_node *data)
{
    sr_error_info_t *err_info = NULL;
    struct ly_set *set = NULL;
    struct lyd_node *node;
    char *path = NULL, *data_json = NULL;

    /* find the module */
    if (asprintf(&path, "installed-module[name=\"%s\"]", module_name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (!set->count) {
        sr_errinfo_new(&err_info, SR_ERR_EXISTS, NULL, "Module \"%s\" not scheduled for installation.", module_name);
        goto cleanup;
    }

    /* remove any previously set data */
    LY_LIST_FOR(LYD_CHILD(set->dnodes[0]), node) {
        if (!strcmp(node->schema->name, "data")) {
            lyd_free_tree(node);
            break;
        }
    }

    /* print into buffer */
    if (lyd_print_mem(&data_json, data, LYD_JSON, LYD_PRINT_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, LYD_NODE_CTX(data));
        goto cleanup;
    }

    /* add into module */
    if (lyd_new_term(set->dnodes[0], NULL, "data", data_json, NULL)) {
        goto cleanup;
    }

    /* success */

cleanup:
    free(path);
    free(data_json);
    ly_set_free(set, NULL);
    return err_info;
}

sr_error_info_t *
sr_lydmods_deferred_del_module(struct ly_ctx *ly_ctx, const char *mod_name)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;
    struct ly_set *set = NULL;
    char *path = NULL;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* check that the module is not already marked for deletion */
    if (asprintf(&path, "module[name=\"%s\"]/removed", mod_name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (set->count == 1) {
        sr_errinfo_new(&err_info, SR_ERR_EXISTS, NULL, "Module \"%s\" already scheduled for deletion.", mod_name);
        goto cleanup;
    }

    /* mark for deletion */
    if (lyd_new_path(sr_mods, NULL, path, NULL, 0, NULL)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

    SR_LOG_INF("Module \"%s\" scheduled for deletion.", mod_name);

cleanup:
    free(path);
    ly_set_free(set, NULL);
    lyd_free_all(sr_mods);
    return err_info;
}

/**
 * @brief Unchedule module (with any implemented dependencies) deletion from internal sysrepo data.
 *
 * @param[in] main_shm_add Main SHM mapping address.
 * @param[in] sr_mods Internal sysrepo data to modify.
 * @param[in] ly_mod Module whose removal to unschedule.
 * @param[in] first Whether this is the first module or just a dependency.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_unsched_del_module_r(struct lyd_node *sr_mods, const struct lys_module *ly_mod, int first)
{
    sr_error_info_t *err_info = NULL;
    struct ly_set *set = NULL;
    char *path = NULL;
    LY_ARRAY_COUNT_TYPE u;

    /* check whether the module is marked for deletion */
    if (asprintf(&path, "module[name=\"%s\"]/removed", ly_mod->name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (!set->count) {
        if (first) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Module \"%s\" not scheduled for deletion.", ly_mod->name);
            goto cleanup;
        }
    } else {
        assert(set->count == 1);
        lyd_free_tree(set->dnodes[0]);
        SR_LOG_INF("Module \"%s\" deletion unscheduled.", ly_mod->name);
    }
    first = 0;

    /* recursively check all imported implemented modules */
    LY_ARRAY_FOR(ly_mod->parsed->imports, u) {
        if (ly_mod->parsed->imports[u].module->implemented) {
            if ((err_info = sr_lydmods_unsched_del_module_r(sr_mods, ly_mod->parsed->imports[u].module, 0))) {
                goto cleanup;
            }
        }
    }

cleanup:
    free(path);
    ly_set_free(set, NULL);
    return err_info;
}

sr_error_info_t *
sr_lydmods_unsched_del_module_with_imps(struct ly_ctx *ly_ctx, const struct lys_module *ly_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* try to unschedule deletion */
    if ((err_info = sr_lydmods_unsched_del_module_r(sr_mods, ly_mod, 1))) {
        goto cleanup;
    }

    /* store the updated persistent data tree */
    err_info = sr_lydmods_print(&sr_mods);

cleanup:
    lyd_free_all(sr_mods);
    return err_info;
}

sr_error_info_t *
sr_lydmods_deferred_upd_module(struct ly_ctx *ly_ctx, const struct lys_module *ly_upd_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;
    struct ly_set *set = NULL;
    char *path = NULL, *yang_str = NULL;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* check that the module is not already marked for update */
    if (asprintf(&path, "module[name=\"%s\"]/updated-yang", ly_upd_mod->name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (set->count == 1) {
        sr_errinfo_new(&err_info, SR_ERR_EXISTS, NULL, "Module \"%s\" already scheduled for an update.", ly_upd_mod->name);
        goto cleanup;
    }

    /* print the module into memory */
    if (lys_print_mem(&yang_str, ly_upd_mod, LYS_OUT_YANG, 0)) {
        sr_errinfo_new_ly(&err_info, ly_upd_mod->ctx);
        goto cleanup;
    }

    /* mark for update */
    if (lyd_new_path(sr_mods, NULL, path, yang_str, 0, NULL)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

    SR_LOG_INF("Module \"%s\" scheduled for an update.", ly_upd_mod->name);

cleanup:
    free(path);
    free(yang_str);
    ly_set_free(set, NULL);
    lyd_free_all(sr_mods);
    return err_info;
}

sr_error_info_t *
sr_lydmods_unsched_upd_module(struct ly_ctx *ly_ctx, const char *mod_name)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;
    struct ly_set *set = NULL;
    char *path = NULL;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* check whether the module is marked for update */
    if (asprintf(&path, "module[name=\"%s\"]/updated-yang", mod_name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (!set->count) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Module \"%s\" not scheduled for an update.", mod_name);
        goto cleanup;
    }

    assert(set->count == 1);
    /* free the "updated-yang" node */
    lyd_free_tree(set->dnodes[0]);

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

    SR_LOG_INF("Module \"%s\" update unscheduled.", mod_name);

cleanup:
    free(path);
    ly_set_free(set, NULL);
    lyd_free_all(sr_mods);
    return err_info;
}

sr_error_info_t *
sr_lydmods_deferred_change_feature(struct ly_ctx *ly_ctx, const struct lys_module *ly_mod, const char *feat_name,
        int to_enable, int is_enabled)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL;
    struct lyd_node *node;
    struct ly_set *set = NULL;
    char *path = NULL;

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    /* check that the feature is not already marked for change */
    if (asprintf(&path, "module[name=\"%s\"]/changed-feature[name=\"%s\"]/change",
            ly_mod->name, feat_name) == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(lyd_find_xpath(sr_mods, path, &set), err_info, cleanup);
    if (set->count == 1) {
        node = set->dnodes[0];
        if ((to_enable && !strcmp(SR_LY_TERM_VALUE(node), "enable"))
                || (!to_enable && !strcmp(SR_LY_TERM_VALUE(node), "disable"))) {
            sr_errinfo_new(&err_info, SR_ERR_EXISTS, NULL, "Module \"%s\" feature \"%s\" already scheduled to be %s.",
                    ly_mod->name, feat_name, to_enable ? "enabled" : "disabled");
            goto cleanup;
        }

        /* unschedule the feature change */
        lyd_free_tree(LYD_PARENT(node));
        SR_LOG_INF("Module \"%s\" feature \"%s\" %s unscheduled.", ly_mod->name, feat_name,
                to_enable ? "disabling" : "enabling");
    } else {
        if ((to_enable && is_enabled) || (!to_enable && !is_enabled)) {
            sr_errinfo_new(&err_info, SR_ERR_EXISTS, NULL, "Module \"%s\" feature \"%s\" is already %s.",
                    ly_mod->name, feat_name, to_enable ? "enabled" : "disabled");
            goto cleanup;
        }

        /* schedule the feature change */
        if (lyd_new_path(sr_mods, NULL, path, to_enable ? "enable" : "disable", 0, NULL)) {
            sr_errinfo_new_ly(&err_info, ly_ctx);
            goto cleanup;
        }

        SR_LOG_INF("Module \"%s\" feature \"%s\" %s scheduled.", ly_mod->name, feat_name,
                to_enable ? "enabling" : "disabling");
    }

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

cleanup:
    free(path);
    ly_set_free(set, NULL);
    lyd_free_all(sr_mods);
    return err_info;
}

/**
 * @brief Update replay support of a module.
 *
 * @param[in,out] sr_mod Module to update.
 * @param[in] replay_support Whether replay should be enabled or disabled.
 * @param[in] s_replay Schema node of replay support.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_lydmods_update_replay_support_module(struct lyd_node *sr_mod, int replay_support, const struct lysc_node *s_replay)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_replay;
    char buf[21];
    time_t from_ts, to_ts;

    lyd_find_sibling_val(lyd_node_children(sr_mod, 0), s_replay, NULL, 0, &sr_replay);
    if (!replay_support && sr_replay) {
        /* remove replay support */
        lyd_free_tree(sr_replay);
    } else if (replay_support && !sr_replay) {
        /* find earliest stored notification or use current time */
        if ((err_info = sr_replay_find_file(SR_LY_CHILD_VALUE(sr_mod), 1, 0, &from_ts, &to_ts))) {
            return err_info;
        }
        if (!from_ts) {
            from_ts = time(NULL);
        }
        sprintf(buf, "%ld", (long int)from_ts);

        /* add replay support */
        SR_CHECK_LY_RET(lyd_new_term(sr_mod, NULL, "replay-support", buf, NULL), LYD_NODE_CTX(sr_mod), err_info);
    }

    return NULL;
}

sr_error_info_t *
sr_lydmods_update_replay_support(struct ly_ctx *ly_ctx, const char *mod_name, int replay_support)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_mods = NULL, *sr_mod;
    char *pred = NULL;
    const struct lysc_node *s_mod, *s_replay;

    /* find schema nodes */
    s_mod = ly_ctx_get_node(ly_ctx, NULL, "/sysrepo:sysrepo-modules/module", 0);
    assert(s_mod);
    s_replay = lys_find_child(s_mod, s_mod->module, "replay-support", 0, 0, 0);
    assert(s_replay);

    /* parse current module information */
    if ((err_info = sr_lydmods_parse(ly_ctx, &sr_mods))) {
        goto cleanup;
    }

    if (mod_name) {
        if (asprintf(&pred, "[name=\"%s\"]", mod_name) == -1) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }

        /* we expect the module to exist */
        lyd_find_sibling_val(lyd_node_children(sr_mods, 0), s_mod, pred, strlen(pred), &sr_mod);
        assert(sr_mod);

        /* set replay support */
        if ((err_info = sr_lydmods_update_replay_support_module(sr_mod, replay_support, s_replay))) {
            goto cleanup;
        }
    } else {
        LY_LIST_FOR(lyd_node_children(sr_mods, 0), sr_mod) {
            if (sr_mod->schema != s_mod) {
                continue;
            }

            /* set replay support */
            if ((err_info = sr_lydmods_update_replay_support_module(sr_mod, replay_support, s_replay))) {
                goto cleanup;
            }
        }
    }

    /* store the updated persistent data tree */
    if ((err_info = sr_lydmods_print(&sr_mods))) {
        goto cleanup;
    }

    /* success */

cleanup:
    free(pred);
    lyd_free_all(sr_mods);
    return err_info;
}


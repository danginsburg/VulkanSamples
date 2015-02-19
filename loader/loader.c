/*
 * XGL
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Chia-I Wu <olv@lunarg.com>
 *   Jon Ashburn <jon@lunarg.com>
 *   Courtney Goeltzenleuchter <courtney@lunarg.com>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include <sys/types.h>
#if defined(WIN32)
#include "dirent_on_windows.h"
#else // WIN32
#include <dirent.h>
#endif // WIN32
#include "loader_platform.h"
#include "table_ops.h"
#include "loader.h"
#include "xglIcd.h"
// The following is #included again to catch certain OS-specific functions
// being used:
#include "loader_platform.h"

struct loader_instance {
    struct loader_icd *icds;
    struct loader_instance *next;
};

struct loader_layers {
    loader_platform_dl_handle lib_handle;
    char name[256];
};

struct layer_name_pair {
    char *layer_name;
    const char *lib_name;
};

struct loader_icd {
    const struct loader_scanned_icds *scanned_icds;

    XGL_LAYER_DISPATCH_TABLE *loader_dispatch;
    uint32_t layer_count[XGL_MAX_PHYSICAL_GPUS];
    struct loader_layers layer_libs[XGL_MAX_PHYSICAL_GPUS][MAX_LAYER_LIBRARIES];
    XGL_BASE_LAYER_OBJECT *wrappedGpus[XGL_MAX_PHYSICAL_GPUS];
    uint32_t gpu_count;
    XGL_BASE_LAYER_OBJECT *gpus;

    struct loader_icd *next;
};


struct loader_msg_callback {
    XGL_DBG_MSG_CALLBACK_FUNCTION func;
    void *data;

    struct loader_msg_callback *next;
};

struct loader_scanned_icds {
    loader_platform_dl_handle handle;
    xglGetProcAddrType GetProcAddr;
    xglCreateInstanceType CreateInstance;
    xglDestroyInstanceType DestroyInstance;
    xglEnumerateGpusType EnumerateGpus;
    XGL_INSTANCE instance;
    struct loader_scanned_icds *next;
};

// Note: Since the following is a static structure, all members are initialized
// to zero.
static struct {
    struct loader_instance *instances;
    bool icds_scanned;
    struct loader_scanned_icds *scanned_icd_list;
    bool layer_scanned;
    char *layer_dirs;
    unsigned int scanned_layer_count;
    char *scanned_layer_names[MAX_LAYER_LIBRARIES];
    struct loader_msg_callback *msg_callbacks;

    bool debug_echo_enable;
    bool break_on_error;
    bool break_on_warning;
} loader;


#if defined(WIN32)
// For ICD developers, look in the registry, and look for an environment
// variable for a path(s) where to find the ICD(s):
static char *loader_get_registry_and_env(const char *env_var,
                                         const char *registry_value)
{
    char *env_str = getenv(env_var);
    size_t env_len = (env_str == NULL) ? 0 : strlen(env_str);
#define INITIAL_STR_LEN 1024
    char *registry_str = malloc(INITIAL_STR_LEN);
    DWORD registry_len = INITIAL_STR_LEN;
    DWORD registry_value_type;
    LONG  registry_return_value;
    char *rtn_str = NULL;
    size_t rtn_len;

    registry_return_value = RegGetValue(HKEY_LOCAL_MACHINE, "Software\\XGL",
                                        registry_value,
                                        (RRF_RT_REG_SZ | RRF_ZEROONFAILURE),
                                        &registry_value_type,
                                        (PVOID) registry_str,
                                        &registry_len);

    if (registry_return_value == ERROR_MORE_DATA) {
        registry_str = realloc(registry_str, registry_len);
        registry_return_value = RegGetValue(HKEY_LOCAL_MACHINE, "Software\\XGL",
                                            registry_value,
                                            (RRF_RT_REG_SZ | RRF_ZEROONFAILURE),
                                            &registry_value_type,
                                            (PVOID) registry_str,
                                            &registry_len);
    }

    rtn_len = env_len + registry_len + 1;
    if (rtn_len <= 2) {
        // We found neither the desired registry value, nor the environment
        // variable; return NULL:
        return NULL;
    } else {
        // We found something, and so we need to allocate memory for the string
        // to return:
        rtn_str = malloc(rtn_len);
    }

    if (registry_return_value != ERROR_SUCCESS) {
        // We didn't find the desired registry value, and so we must have found
        // only the environment variable:
        _snprintf(rtn_str, rtn_len, "%s", env_str);
    } else if (env_str != NULL) {
        // We found both the desired registry value and the environment
        // variable, so concatenate them both:
        _snprintf(rtn_str, rtn_len, "%s;%s", registry_str, env_str);
    } else {
        // We must have only found the desired registry value:
        _snprintf(rtn_str, rtn_len, "%s", registry_str);
    }

    free(registry_str);

    return(rtn_str);
}
#endif // WIN32


static XGL_RESULT loader_msg_callback_add(XGL_DBG_MSG_CALLBACK_FUNCTION func,
                                          void *data)
{
    struct loader_msg_callback *cb;

    cb = malloc(sizeof(*cb));
    if (!cb)
        return XGL_ERROR_OUT_OF_MEMORY;

    cb->func = func;
    cb->data = data;

    cb->next = loader.msg_callbacks;
    loader.msg_callbacks = cb;

    return XGL_SUCCESS;
}

static XGL_RESULT loader_msg_callback_remove(XGL_DBG_MSG_CALLBACK_FUNCTION func)
{
    struct loader_msg_callback *cb = loader.msg_callbacks;

    /*
     * Find the first match (last registered).
     *
     * XXX What if the same callback function is registered more than once?
     */
    while (cb) {
        if (cb->func == func) {
            break;
        }

        cb = cb->next;
    }

    if (!cb)
        return XGL_ERROR_INVALID_POINTER;

    free(cb);

    return XGL_SUCCESS;
}

static void loader_msg_callback_clear(void)
{
    struct loader_msg_callback *cb = loader.msg_callbacks;

    while (cb) {
        struct loader_msg_callback *next = cb->next;
        free(cb);
        cb = next;
    }

    loader.msg_callbacks = NULL;
}

static void loader_log(XGL_DBG_MSG_TYPE msg_type, int32_t msg_code,
                       const char *format, ...)
{
    const struct loader_msg_callback *cb = loader.msg_callbacks;
    char msg[256];
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsnprintf(msg, sizeof(msg), format, ap);
    if ((ret >= (int) sizeof(msg)) || ret < 0) {
        msg[sizeof(msg) - 1] = '\0';
    }
    va_end(ap);

    if (loader.debug_echo_enable || !cb) {
        fputs(msg, stderr);
        fputc('\n', stderr);
    }

    while (cb) {
        cb->func(msg_type, XGL_VALIDATION_LEVEL_0, XGL_NULL_HANDLE, 0,
                msg_code, msg, cb->data);
        cb = cb->next;
    }

    switch (msg_type) {
    case XGL_DBG_MSG_ERROR:
        if (loader.break_on_error) {
            exit(1);
        }
        /* fall through */
    case XGL_DBG_MSG_WARNING:
        if (loader.break_on_warning) {
            exit(1);
        }
        break;
    default:
        break;
    }
}

static void
loader_icd_destroy(struct loader_icd *icd)
{
    loader_platform_close_library(icd->scanned_icds->handle);
    free(icd);
}

static struct loader_icd *
loader_icd_create(const struct loader_scanned_icds *scanned)
{
    struct loader_icd *icd;

    icd = malloc(sizeof(*icd));
    if (!icd)
        return NULL;

    memset(icd, 0, sizeof(*icd));

    icd->scanned_icds = scanned;

    return icd;
}

static XGL_RESULT loader_icd_register_msg_callbacks(const struct loader_icd *icd)
{
    const struct loader_msg_callback *cb = loader.msg_callbacks;
    XGL_RESULT res;

    while (cb) {
        for (uint32_t i = 0; i < icd->gpu_count; i++) {
            res = (icd->loader_dispatch + i)->DbgRegisterMsgCallback(cb->func, cb->data);
            if (res != XGL_SUCCESS) {
                break;
            }
        }
        cb = cb->next;
    }

    /* roll back on errors */
    if (cb) {
        const struct loader_msg_callback *tmp = loader.msg_callbacks;

        while (tmp != cb) {
            for (uint32_t i = 0; i < icd->gpu_count; i++) {
                (icd->loader_dispatch + i)->DbgUnregisterMsgCallback(cb->func);
            }
            tmp = tmp->next;
        }

        return res;
    }

    return XGL_SUCCESS;
}

static XGL_RESULT loader_icd_set_global_options(const struct loader_icd *icd)
{
#define SETB(icd, opt, val) do {                                \
    if (val) {                                                  \
        for (uint32_t i = 0; i < icd->gpu_count; i++) {         \
            const XGL_RESULT res =                              \
                (icd->loader_dispatch + i)->DbgSetGlobalOption(opt, sizeof(val), &val); \
            if (res != XGL_SUCCESS)                             \
                return res;                                     \
        }                                                       \
    }                                                           \
} while (0)
    SETB(icd, XGL_DBG_OPTION_DEBUG_ECHO_ENABLE, loader.debug_echo_enable);
    SETB(icd, XGL_DBG_OPTION_BREAK_ON_ERROR, loader.break_on_error);
    SETB(icd, XGL_DBG_OPTION_BREAK_ON_WARNING, loader.break_on_warning);
#undef SETB

return XGL_SUCCESS;
}

static struct loader_icd *loader_icd_add(struct loader_instance *ptr_inst,
                                     const struct loader_scanned_icds *scanned)
{
    struct loader_icd *icd;

    icd = loader_icd_create(scanned);
    if (!icd)
        return NULL;

    /* prepend to the list */
    icd->next = ptr_inst->icds;
    ptr_inst->icds = icd;

    return icd;
}

static void loader_scanned_icd_add(const char *filename)
{
    loader_platform_dl_handle handle;
    void *fp_gpa, *fp_enumerate, *fp_create_inst, *fp_destroy_inst;
    struct loader_scanned_icds *new_node;

    // Used to call: dlopen(filename, RTLD_LAZY);
    handle = loader_platform_open_library(filename);
    if (!handle) {
        loader_log(XGL_DBG_MSG_WARNING, 0, loader_platform_open_library_error(filename));
        return;
    }

#define LOOKUP(func_ptr, func) do {                            \
    func_ptr = (xgl ##func## Type) loader_platform_get_proc_address(handle, "xgl" #func); \
    if (!func_ptr) {                                           \
        loader_log(XGL_DBG_MSG_WARNING, 0, loader_platform_get_proc_address_error("xgl" #func)); \
        return;                                                \
    }                                                          \
} while (0)

    LOOKUP(fp_gpa, GetProcAddr);
    LOOKUP(fp_create_inst, CreateInstance);
    LOOKUP(fp_destroy_inst, DestroyInstance);
    LOOKUP(fp_enumerate, EnumerateGpus);
#undef LOOKUP

    new_node = (struct loader_scanned_icds *) malloc(sizeof(struct loader_scanned_icds));
    if (!new_node) {
        loader_log(XGL_DBG_MSG_WARNING, 0, "Out of memory can't add icd");
        return;
    }

    new_node->handle = handle;
    new_node->GetProcAddr = fp_gpa;
    new_node->CreateInstance = fp_create_inst;
    new_node->DestroyInstance = fp_destroy_inst;
    new_node->EnumerateGpus = fp_enumerate;
    new_node->next = loader.scanned_icd_list;
    loader.scanned_icd_list = new_node;
}


/**
 * Try to \c loader_icd_scan XGL driver(s).
 *
 * This function scans the default system path or path
 * specified by the \c LIBXGL_DRIVERS_PATH environment variable in
 * order to find loadable XGL ICDs with the name of libXGL_*.
 *
 * \returns
 * void; but side effect is to set loader_icd_scanned to true
 */
static void loader_icd_scan(void)
{
    const char *p, *next;
    char *libPaths = NULL;
    DIR *sysdir;
    struct dirent *dent;
    char icd_library[1024];
    char path[1024];
    uint32_t len;
#if defined(WIN32)
    bool must_free_libPaths;
    libPaths = loader_get_registry_and_env(DRIVER_PATH_ENV,
                                           DRIVER_PATH_REGISTRY_VALUE);
    if (libPaths != NULL) {
        must_free_libPaths = true;
    } else {
        must_free_libPaths = false;
        libPaths = DEFAULT_XGL_DRIVERS_PATH;
    }
#else  // WIN32
    if (geteuid() == getuid()) {
        /* Don't allow setuid apps to use the DRIVER_PATH_ENV env var: */
        libPaths = getenv(DRIVER_PATH_ENV);
    }
    if (libPaths == NULL) {
        libPaths = DEFAULT_XGL_DRIVERS_PATH;
    }
#endif // WIN32

    for (p = libPaths; *p; p = next) {
       next = strchr(p, PATH_SEPERATOR);
       if (next == NULL) {
          len = (uint32_t) strlen(p);
          next = p + len;
       }
       else {
          len = (uint32_t) (next - p);
          sprintf(path, "%.*s", (len > sizeof(path) - 1) ? (int) sizeof(path) - 1 : len, p);
          p = path;
          next++;
       }

       // TODO/TBD: Do we want to do this on Windows, or just let Windows take
       // care of its own search path (which it apparently has)?
       sysdir = opendir(p);
       if (sysdir) {
          dent = readdir(sysdir);
          while (dent) {
             /* Look for ICDs starting with XGL_DRIVER_LIBRARY_PREFIX and
              * ending with XGL_LIBRARY_SUFFIX
              */
              if (!strncmp(dent->d_name,
                          XGL_DRIVER_LIBRARY_PREFIX,
                          XGL_DRIVER_LIBRARY_PREFIX_LEN)) {
                 uint32_t nlen = (uint32_t) strlen(dent->d_name);
                 const char *suf = dent->d_name + nlen - XGL_LIBRARY_SUFFIX_LEN;
                 if ((nlen > XGL_LIBRARY_SUFFIX_LEN) &&
                     !strncmp(suf,
                              XGL_LIBRARY_SUFFIX,
                              XGL_LIBRARY_SUFFIX_LEN)) {
                    snprintf(icd_library, 1024, "%s" DIRECTORY_SYMBOL "%s", p,dent->d_name);
                    loader_scanned_icd_add(icd_library);
                 }
              }

             dent = readdir(sysdir);
          }
          closedir(sysdir);
       }
    }

#if defined(WIN32)
    // Free any allocated memory:
    if (must_free_libPaths) {
        free(libPaths);
    }
#endif // WIN32

    // Note that we've scanned for ICDs:
    loader.icds_scanned = true;
}


static void layer_lib_scan(void)
{
    const char *p, *next;
    char *libPaths = NULL;
    DIR *curdir;
    struct dirent *dent;
    size_t len, i;
    char temp_str[1024];

#if defined(WIN32)
    bool must_free_libPaths;
    libPaths = loader_get_registry_and_env(LAYERS_PATH_ENV,
                                           LAYERS_PATH_REGISTRY_VALUE);
    if (libPaths != NULL) {
        must_free_libPaths = true;
    } else {
        must_free_libPaths = false;
        libPaths = DEFAULT_XGL_LAYERS_PATH;
    }
#else  // WIN32
    if (geteuid() == getuid()) {
        /* Don't allow setuid apps to use the DRIVER_PATH_ENV env var: */
        libPaths = getenv(LAYERS_PATH_ENV);
    }
    if (libPaths == NULL) {
        libPaths = DEFAULT_XGL_LAYERS_PATH;
    }
#endif // WIN32

    if (libPaths == NULL) {
        // Have no paths to search:
        return;
    }
    len = strlen(libPaths);
    loader.layer_dirs = malloc(len+1);
    if (loader.layer_dirs == NULL) {
        free(libPaths);
        return;
    }
    // Alloc passed, so we know there is enough space to hold the string, don't
    // need strncpy
    strcpy(loader.layer_dirs, libPaths);
#if defined(WIN32)
    // Free any allocated memory:
    if (must_free_libPaths) {
        free(libPaths);
        must_free_libPaths = false;
    }
#endif // WIN32
    libPaths = loader.layer_dirs;

    /* cleanup any previously scanned libraries */
    for (i = 0; i < loader.scanned_layer_count; i++) {
        if (loader.scanned_layer_names[i] != NULL)
            free(loader.scanned_layer_names[i]);
        loader.scanned_layer_names[i] = NULL;
    }
    loader.scanned_layer_count = 0;

    for (p = libPaths; *p; p = next) {
       next = strchr(p, PATH_SEPERATOR);
       if (next == NULL) {
          len = (uint32_t) strlen(p);
          next = p + len;
       }
       else {
          len = (uint32_t) (next - p);
          *(char *) next = '\0';
          next++;
       }

       curdir = opendir(p);
       if (curdir) {
          dent = readdir(curdir);
          while (dent) {
             /* Look for layers starting with XGL_LAYER_LIBRARY_PREFIX and
              * ending with XGL_LIBRARY_SUFFIX
              */
              if (!strncmp(dent->d_name,
                          XGL_LAYER_LIBRARY_PREFIX,
                          XGL_LAYER_LIBRARY_PREFIX_LEN)) {
                 uint32_t nlen = (uint32_t) strlen(dent->d_name);
                 const char *suf = dent->d_name + nlen - XGL_LIBRARY_SUFFIX_LEN;
                 if ((nlen > XGL_LIBRARY_SUFFIX_LEN) &&
                     !strncmp(suf,
                              XGL_LIBRARY_SUFFIX,
                              XGL_LIBRARY_SUFFIX_LEN)) {
                     loader_platform_dl_handle handle;
                     snprintf(temp_str, sizeof(temp_str), "%s" DIRECTORY_SYMBOL "%s",p,dent->d_name);
                     // Used to call: dlopen(temp_str, RTLD_LAZY)
                     if ((handle = loader_platform_open_library(temp_str)) == NULL) {
                         dent = readdir(curdir);
                         continue;
                     }
                     if (loader.scanned_layer_count == MAX_LAYER_LIBRARIES) {
                         loader_log(XGL_DBG_MSG_ERROR, 0, "%s ignored: max layer libraries exceed", temp_str);
                         break;
                     }
                     if ((loader.scanned_layer_names[loader.scanned_layer_count] = malloc(strlen(temp_str) + 1)) == NULL) {
                         loader_log(XGL_DBG_MSG_ERROR, 0, "%s ignored: out of memory", temp_str);
                         break;
                     }
                     strcpy(loader.scanned_layer_names[loader.scanned_layer_count], temp_str);
                     loader.scanned_layer_count++;
                     loader_platform_close_library(handle);
                 }
             }

             dent = readdir(curdir);
          }
          closedir(curdir);
       }
    }

    loader.layer_scanned = true;
}

static void loader_init_dispatch_table(XGL_LAYER_DISPATCH_TABLE *tab, xglGetProcAddrType fpGPA, XGL_PHYSICAL_GPU gpu)
{
    loader_initialize_dispatch_table(tab, fpGPA, gpu);

    if (tab->EnumerateLayers == NULL)
        tab->EnumerateLayers = xglEnumerateLayers;
}

static struct loader_icd * loader_get_icd(const XGL_BASE_LAYER_OBJECT *gpu, uint32_t *gpu_index)
{
    for (struct loader_instance *inst = loader.instances; inst; inst = inst->next) {
        for (struct loader_icd *icd = inst->icds; icd; icd = icd->next) {
            for (uint32_t i = 0; i < icd->gpu_count; i++)
                if ((icd->gpus + i) == gpu || (icd->gpus +i)->baseObject ==
                                                            gpu->baseObject) {
                    *gpu_index = i;
                    return icd;
                }
        }
    }
    return NULL;
}

static bool loader_layers_activated(const struct loader_icd *icd, const uint32_t gpu_index)
{
    if (icd->layer_count[gpu_index])
        return true;
    else
        return false;
}

static void loader_init_layer_libs(struct loader_icd *icd, uint32_t gpu_index, struct layer_name_pair * pLayerNames, uint32_t count)
{
    if (!icd)
        return;

    struct loader_layers *obj;
    bool foundLib;
    for (uint32_t i = 0; i < count; i++) {
        foundLib = false;
        for (uint32_t j = 0; j < icd->layer_count[gpu_index]; j++) {
            if (icd->layer_libs[gpu_index][j].lib_handle && !strcmp(icd->layer_libs[gpu_index][j].name, (char *) pLayerNames[i].layer_name)) {
                foundLib = true;
                break;
            }
        }
        if (!foundLib) {
            obj = &(icd->layer_libs[gpu_index][i]);
            strncpy(obj->name, (char *) pLayerNames[i].layer_name, sizeof(obj->name) - 1);
            obj->name[sizeof(obj->name) - 1] = '\0';
            // Used to call: dlopen(pLayerNames[i].lib_name, RTLD_LAZY | RTLD_DEEPBIND)
            if ((obj->lib_handle = loader_platform_open_library(pLayerNames[i].lib_name)) == NULL) {
                loader_log(XGL_DBG_MSG_ERROR, 0, loader_platform_open_library_error(pLayerNames[i].lib_name));
                continue;
            } else {
                loader_log(XGL_DBG_MSG_UNKNOWN, 0, "Inserting layer %s from library %s", pLayerNames[i].layer_name, pLayerNames[i].lib_name);
            }
            free(pLayerNames[i].layer_name);
            icd->layer_count[gpu_index]++;
        }
    }
}

static bool find_layer_name(struct loader_icd *icd, uint32_t gpu_index, const char * layer_name, const char **lib_name)
{
    loader_platform_dl_handle handle;
    xglEnumerateLayersType fpEnumerateLayers;
    char layer_buf[16][256];
    char * layers[16];

    for (int i = 0; i < 16; i++)
         layers[i] = &layer_buf[i][0];

    for (unsigned int j = 0; j < loader.scanned_layer_count; j++) {
        *lib_name = loader.scanned_layer_names[j];
        // Used to call: dlopen(*lib_name, RTLD_LAZY)
        if ((handle = loader_platform_open_library(*lib_name)) == NULL)
            continue;
        if ((fpEnumerateLayers = (xglEnumerateLayersType) loader_platform_get_proc_address(handle, "xglEnumerateLayers")) == NULL) {
            char * lib_str = malloc(strlen(*lib_name) + 1 + strlen(layer_name));
            //use default layer name
            snprintf(lib_str, strlen(*lib_name) + strlen(layer_name),
                     XGL_DRIVER_LIBRARY_PREFIX "%s" XGL_LIBRARY_SUFFIX,
                     layer_name);
            loader_platform_close_library(handle);
            if (!strcmp(*lib_name, lib_str)) {
                free(lib_str);
                return true;
            }
            else {
                free(lib_str);
                continue;
            }
        }
        else {
            size_t cnt;
            fpEnumerateLayers(NULL, 16, 256, &cnt, layers, (char *) icd->gpus + gpu_index);
            for (unsigned int i = 0; i < cnt; i++) {
                if (!strcmp((char *) layers[i], layer_name)) {
                    loader_platform_close_library(handle);
                    return true;
                }
            }
        }

        loader_platform_close_library(handle);
    }

    return false;
}

static uint32_t loader_get_layer_env(struct loader_icd *icd, uint32_t gpu_index, struct layer_name_pair *pLayerNames)
{
    char *layerEnv;
    uint32_t len, count = 0;
    char *p, *pOrig, *next, *name;

#if defined(WIN32)
    layerEnv = loader_get_registry_and_env(LAYER_NAMES_ENV,
                                           LAYER_NAMES_REGISTRY_VALUE);
#else  // WIN32
    layerEnv = getenv(LAYER_NAMES_ENV);
#endif // WIN32
    if (layerEnv == NULL) {
        return 0;
    }
    p = malloc(strlen(layerEnv) + 1);
    if (p == NULL) {
#if defined(WIN32)
        free(layerEnv);
#endif // WIN32
        return 0;
    }
    strcpy(p, layerEnv);
#if defined(WIN32)
    free(layerEnv);
#endif // WIN32
    pOrig = p;

    while (p && *p && count < MAX_LAYER_LIBRARIES) {
        const char *lib_name = NULL;
        next = strchr(p, PATH_SEPERATOR);
        if (next == NULL) {
            len = (uint32_t) strlen(p);
            next = p + len;
        }
        else {
            len = (uint32_t) (next - p);
            *(char *) next = '\0';
            next++;
        }
        name = basename(p);
        if (!find_layer_name(icd, gpu_index, name, &lib_name)) {
            p = next;
            continue;
        }

        len = (uint32_t) strlen(name);
        pLayerNames[count].layer_name = malloc(len + 1);
        if (!pLayerNames[count].layer_name) {
            free(pOrig);
            return count;
        }
        strncpy((char *) pLayerNames[count].layer_name, name, len);
        pLayerNames[count].layer_name[len] = '\0';
        pLayerNames[count].lib_name = lib_name;
        count++;
        p = next;

    };

    free(pOrig);
    return count;
}

static uint32_t loader_get_layer_libs(struct loader_icd *icd, uint32_t gpu_index, const XGL_DEVICE_CREATE_INFO* pCreateInfo, struct layer_name_pair **ppLayerNames)
{
    static struct layer_name_pair layerNames[MAX_LAYER_LIBRARIES];
    int env_layer_count = 0;

    *ppLayerNames =  &layerNames[0];
    /* Load any layers specified in the environment first */
    env_layer_count = loader_get_layer_env(icd, gpu_index, layerNames);

    const XGL_LAYER_CREATE_INFO *pCi =
        (const XGL_LAYER_CREATE_INFO *) pCreateInfo->pNext;

    while (pCi) {
        if (pCi->sType == XGL_STRUCTURE_TYPE_LAYER_CREATE_INFO) {
            const char *name;
            uint32_t len;
            for (uint32_t i = env_layer_count; i < (env_layer_count + pCi->layerCount); i++) {
                const char * lib_name = NULL;
                name = *(pCi->ppActiveLayerNames + i);
                if (!find_layer_name(icd, gpu_index, name, &lib_name))
                    return loader_get_layer_env(icd, gpu_index, layerNames);
                len = (uint32_t) strlen(name);
                layerNames[i].layer_name = malloc(len + 1);
                if (!layerNames[i].layer_name)
                    return i;
                strncpy((char *) layerNames[i].layer_name, name, len);
                layerNames[i].layer_name[len] = '\0';
                layerNames[i].lib_name = lib_name;
            }
            return pCi->layerCount + loader_get_layer_env(icd, gpu_index, layerNames);
        }
        pCi = pCi->pNext;
    }
    return loader_get_layer_env(icd, gpu_index, layerNames);
}

static void loader_deactivate_layer(const struct loader_instance *instance)
{
    struct loader_icd *icd;
    struct loader_layers *libs;

    for (icd = instance->icds; icd; icd = icd->next) {
        if (icd->gpus)
            free(icd->gpus);
        icd->gpus = NULL;
        if (icd->loader_dispatch)
            free(icd->loader_dispatch);
        icd->loader_dispatch = NULL;
        for (uint32_t j = 0; j < icd->gpu_count; j++) {
            if (icd->layer_count[j] > 0) {
                for (uint32_t i = 0; i < icd->layer_count[j]; i++) {
                    libs = &(icd->layer_libs[j][i]);
                    if (libs->lib_handle)
                        loader_platform_close_library(libs->lib_handle);
                    libs->lib_handle = NULL;
                }
                if (icd->wrappedGpus[j])
                    free(icd->wrappedGpus[j]);
            }
            icd->layer_count[j] = 0;
        }
        icd->gpu_count = 0;
    }
}

extern uint32_t loader_activate_layers(XGL_PHYSICAL_GPU gpu, const XGL_DEVICE_CREATE_INFO* pCreateInfo)
{
    uint32_t gpu_index;
    uint32_t count;
    struct layer_name_pair *pLayerNames;
    struct loader_icd *icd = loader_get_icd((const XGL_BASE_LAYER_OBJECT *) gpu, &gpu_index);

    if (!icd)
        return 0;
    assert(gpu_index < XGL_MAX_PHYSICAL_GPUS);

    /* activate any layer libraries */
    if (!loader_layers_activated(icd, gpu_index)) {
        XGL_BASE_LAYER_OBJECT *gpuObj = (XGL_BASE_LAYER_OBJECT *) gpu;
        XGL_BASE_LAYER_OBJECT *nextGpuObj, *baseObj = gpuObj->baseObject;
        xglGetProcAddrType nextGPA = xglGetProcAddr;

        count = loader_get_layer_libs(icd, gpu_index, pCreateInfo, &pLayerNames);
        if (!count)
            return 0;
        loader_init_layer_libs(icd, gpu_index, pLayerNames, count);

        icd->wrappedGpus[gpu_index] = malloc(sizeof(XGL_BASE_LAYER_OBJECT) * icd->layer_count[gpu_index]);
        if (! icd->wrappedGpus[gpu_index])
                loader_log(XGL_DBG_MSG_ERROR, 0, "Failed to malloc Gpu objects for layer");
        for (int32_t i = icd->layer_count[gpu_index] - 1; i >= 0; i--) {
            nextGpuObj = (icd->wrappedGpus[gpu_index] + i);
            nextGpuObj->pGPA = nextGPA;
            nextGpuObj->baseObject = baseObj;
            nextGpuObj->nextObject = gpuObj;
            gpuObj = nextGpuObj;

            char funcStr[256];
            snprintf(funcStr, 256, "%sGetProcAddr",icd->layer_libs[gpu_index][i].name);
            if ((nextGPA = (xglGetProcAddrType) loader_platform_get_proc_address(icd->layer_libs[gpu_index][i].lib_handle, funcStr)) == NULL)
                nextGPA = (xglGetProcAddrType) loader_platform_get_proc_address(icd->layer_libs[gpu_index][i].lib_handle, "xglGetProcAddr");
            if (!nextGPA) {
                loader_log(XGL_DBG_MSG_ERROR, 0, "Failed to find xglGetProcAddr in layer %s", icd->layer_libs[gpu_index][i].name);
                continue;
            }

            if (i == 0) {
                loader_init_dispatch_table(icd->loader_dispatch + gpu_index, nextGPA, gpuObj);
                //Insert the new wrapped objects into the list with loader object at head
                ((XGL_BASE_LAYER_OBJECT *) gpu)->nextObject = gpuObj;
                ((XGL_BASE_LAYER_OBJECT *) gpu)->pGPA = nextGPA;
                gpuObj = icd->wrappedGpus[gpu_index] + icd->layer_count[gpu_index] - 1;
                gpuObj->nextObject = baseObj;
                gpuObj->pGPA = icd->scanned_icds->GetProcAddr;
            }

        }
    }
    else {
        //make sure requested Layers matches currently activated Layers
        count = loader_get_layer_libs(icd, gpu_index, pCreateInfo, &pLayerNames);
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(icd->layer_libs[gpu_index][i].name, pLayerNames[i].layer_name)) {
                loader_log(XGL_DBG_MSG_ERROR, 0, "Layers activated != Layers requested");
                break;
            }
        }
        if (count != icd->layer_count[gpu_index]) {
            loader_log(XGL_DBG_MSG_ERROR, 0, "Number of Layers activated != number requested");
        }
    }
    return icd->layer_count[gpu_index];
}

LOADER_EXPORT XGL_RESULT XGLAPI xglCreateInstance(
        const XGL_APPLICATION_INFO*                 pAppInfo,
        const XGL_ALLOC_CALLBACKS*                  pAllocCb,
        XGL_INSTANCE*                               pInstance)
{
    static LOADER_PLATFORM_THREAD_ONCE_DECLARATION(once_icd);
    static LOADER_PLATFORM_THREAD_ONCE_DECLARATION(once_layer);
    struct loader_instance *ptr_instance = NULL;
    struct loader_scanned_icds *scanned_icds;
    struct loader_icd *icd;
    XGL_RESULT res;

    /* Scan/discover all ICD libraries in a single-threaded manner */
    loader_platform_thread_once(&once_icd, loader_icd_scan);

    /* get layer libraries in a single-threaded manner */
    loader_platform_thread_once(&once_layer, layer_lib_scan);

    ptr_instance = (struct loader_instance*) malloc(sizeof(struct loader_instance));
    if (ptr_instance == NULL) {
        return XGL_ERROR_OUT_OF_MEMORY;
    }
    memset(ptr_instance, 0, sizeof(struct loader_instance));

    ptr_instance->next = loader.instances;
    loader.instances = ptr_instance;

    scanned_icds = loader.scanned_icd_list;
    while (scanned_icds) {
        icd = loader_icd_add(ptr_instance, scanned_icds);
        if (icd) {
            res = scanned_icds->CreateInstance(pAppInfo, pAllocCb,
                                           &(scanned_icds->instance));
            if (res != XGL_SUCCESS)
            {
                ptr_instance->icds = ptr_instance->icds->next;
                loader_icd_destroy(icd);
                scanned_icds->instance = NULL;
                loader_log(XGL_DBG_MSG_WARNING, 0,
                        "ICD ignored: failed to CreateInstance on device");
            }
        }
        scanned_icds = scanned_icds->next;
    }

    if (ptr_instance->icds == NULL) {
        return XGL_ERROR_INCOMPATIBLE_DRIVER;
    }

    *pInstance = (XGL_INSTANCE) ptr_instance;
    return XGL_SUCCESS;
}

LOADER_EXPORT XGL_RESULT XGLAPI xglDestroyInstance(
        XGL_INSTANCE                                instance)
{
    struct loader_instance *ptr_instance = (struct loader_instance *) instance;
    struct loader_scanned_icds *scanned_icds;
    XGL_RESULT res;

    // Remove this instance from the list of instances:
    struct loader_instance *prev = NULL;
    struct loader_instance *next = loader.instances;
    while (next != NULL) {
        if (next == ptr_instance) {
            // Remove this instance from the list:
            if (prev)
                prev->next = next->next;
            else
                loader.instances = next->next;
            break;
        }
        prev = next;
        next = next->next;
    }
    if (next  == NULL) {
        // This must be an invalid instance handle or empty list
        return XGL_ERROR_INVALID_HANDLE;
    }

    // cleanup any prior layer initializations
    loader_deactivate_layer(ptr_instance);

    scanned_icds = loader.scanned_icd_list;
    while (scanned_icds) {
        if (scanned_icds->instance)
            res = scanned_icds->DestroyInstance(scanned_icds->instance);
        if (res != XGL_SUCCESS)
            loader_log(XGL_DBG_MSG_WARNING, 0,
                        "ICD ignored: failed to DestroyInstance on device");
        scanned_icds->instance = NULL;
        scanned_icds = scanned_icds->next;
    }

    free(ptr_instance);

    return XGL_SUCCESS;
}

LOADER_EXPORT XGL_RESULT XGLAPI xglEnumerateGpus(

        XGL_INSTANCE                                instance,
        uint32_t                                    maxGpus,
        uint32_t*                                   pGpuCount,
        XGL_PHYSICAL_GPU*                           pGpus)
{
    struct loader_instance *ptr_instance = (struct loader_instance *) instance;
    struct loader_icd *icd;
    uint32_t count = 0;
    XGL_RESULT res;

    //in spirit of XGL don't error check on the instance parameter
    icd = ptr_instance->icds;
    while (icd) {
        XGL_PHYSICAL_GPU gpus[XGL_MAX_PHYSICAL_GPUS];
        XGL_BASE_LAYER_OBJECT * wrapped_gpus;
        xglGetProcAddrType get_proc_addr = icd->scanned_icds->GetProcAddr;
        uint32_t n, max = maxGpus - count;

        if (max > XGL_MAX_PHYSICAL_GPUS) {
            max = XGL_MAX_PHYSICAL_GPUS;
        }

        res = icd->scanned_icds->EnumerateGpus(icd->scanned_icds->instance,
                                               max, &n,
                                               gpus);
        if (res == XGL_SUCCESS && n) {
            wrapped_gpus = (XGL_BASE_LAYER_OBJECT*) malloc(n *
                                        sizeof(XGL_BASE_LAYER_OBJECT));
            icd->gpus = wrapped_gpus;
            icd->gpu_count = n;
            icd->loader_dispatch = (XGL_LAYER_DISPATCH_TABLE *) malloc(n *
                                        sizeof(XGL_LAYER_DISPATCH_TABLE));
            for (unsigned int i = 0; i < n; i++) {
                (wrapped_gpus + i)->baseObject = gpus[i];
                (wrapped_gpus + i)->pGPA = get_proc_addr;
                (wrapped_gpus + i)->nextObject = gpus[i];
                memcpy(pGpus + count, &wrapped_gpus, sizeof(*pGpus));
                loader_init_dispatch_table(icd->loader_dispatch + i,
                                           get_proc_addr, gpus[i]);

                /* Verify ICD compatibility */
                if (!valid_loader_magic_value(gpus[i])) {
                    loader_log(XGL_DBG_MSG_WARNING, 0,
                            "Loader: Incompatible ICD, first dword must be initialized to ICD_LOADER_MAGIC. See loader/README.md for details.\n");
                    assert(0);
                }

                const XGL_LAYER_DISPATCH_TABLE **disp;
                disp = (const XGL_LAYER_DISPATCH_TABLE **) gpus[i];
                *disp = icd->loader_dispatch + i;
            }

            if (loader_icd_set_global_options(icd) != XGL_SUCCESS ||
                loader_icd_register_msg_callbacks(icd) != XGL_SUCCESS) {
                loader_log(XGL_DBG_MSG_WARNING, 0,
                        "ICD ignored: failed to migrate settings");
                loader_icd_destroy(icd);
            }
            count += n;

            if (count >= maxGpus) {
                break;
            }
        }

        icd = icd->next;
    }

    /* we have nothing to log anymore */
    loader_msg_callback_clear();

    *pGpuCount = count;

    return (count > 0) ? XGL_SUCCESS : res;
}

LOADER_EXPORT void * XGLAPI xglGetProcAddr(XGL_PHYSICAL_GPU gpu, const char * pName)
{
    if (gpu == NULL) {
        return NULL;
    }
    XGL_BASE_LAYER_OBJECT* gpuw = (XGL_BASE_LAYER_OBJECT *) gpu;
    XGL_LAYER_DISPATCH_TABLE * disp_table = * (XGL_LAYER_DISPATCH_TABLE **) gpuw->baseObject;
    void *addr;

    if (disp_table == NULL)
        return NULL;

    addr = loader_lookup_dispatch_table(disp_table, pName);
    if (addr)
        return addr;
    else  {
        if (disp_table->GetProcAddr == NULL)
            return NULL;
        return disp_table->GetProcAddr(gpuw->nextObject, pName);
    }
}

LOADER_EXPORT XGL_RESULT XGLAPI xglEnumerateLayers(XGL_PHYSICAL_GPU gpu, size_t maxLayerCount, size_t maxStringSize, size_t* pOutLayerCount, char* const* pOutLayers, void* pReserved)
{
    uint32_t gpu_index;
    size_t count = 0;
    char *lib_name;
    struct loader_icd *icd = loader_get_icd((const XGL_BASE_LAYER_OBJECT *) gpu, &gpu_index);
    loader_platform_dl_handle handle;
    xglEnumerateLayersType fpEnumerateLayers;
    char layer_buf[16][256];
    char * layers[16];

    if (pOutLayerCount == NULL || pOutLayers == NULL)
        return XGL_ERROR_INVALID_POINTER;

    if (!icd)
        return XGL_ERROR_UNAVAILABLE;

    for (int i = 0; i < 16; i++)
         layers[i] = &layer_buf[i][0];

    for (unsigned int j = 0; j < loader.scanned_layer_count && count < maxLayerCount; j++) {
        lib_name = loader.scanned_layer_names[j];
        // Used to call: dlopen(*lib_name, RTLD_LAZY)
        if ((handle = loader_platform_open_library(lib_name)) == NULL)
            continue;
        if ((fpEnumerateLayers = loader_platform_get_proc_address(handle, "xglEnumerateLayers")) == NULL) {
            //use default layer name based on library name XGL_LAYER_LIBRARY_PREFIX<name>.XGL_LIBRARY_SUFFIX
            char *pEnd, *cpyStr;
            size_t siz;
            loader_platform_close_library(handle);
            lib_name = basename(lib_name);
            pEnd = strrchr(lib_name, '.');
            siz = (int) (pEnd - lib_name - strlen(XGL_LAYER_LIBRARY_PREFIX) + 1);
            if (pEnd == NULL || siz <= 0)
                continue;
            cpyStr = malloc(siz);
            if (cpyStr == NULL) {
                free(cpyStr);
                continue;
            }
            strncpy(cpyStr, lib_name + strlen(XGL_LAYER_LIBRARY_PREFIX), siz);
            cpyStr[siz - 1] = '\0';
            if (siz > maxStringSize)
                siz = (int) maxStringSize;
            strncpy((char *) (pOutLayers[count]), cpyStr, siz);
            pOutLayers[count][siz - 1] = '\0';
            count++;
            free(cpyStr);
        }
        else {
            size_t cnt;
            uint32_t n;
            XGL_RESULT res;
            n = (uint32_t) ((maxStringSize < 256) ? maxStringSize : 256);
            res = fpEnumerateLayers(NULL, 16, n, &cnt, layers, (char *) icd->gpus + gpu_index);
            loader_platform_close_library(handle);
            if (res != XGL_SUCCESS)
                continue;
            if (cnt + count > maxLayerCount)
                cnt = maxLayerCount - count;
            for (uint32_t i = (uint32_t) count; i < cnt + count; i++) {
                strncpy((char *) (pOutLayers[i]), (char *) layers[i - count], n);
                if (n > 0)
                    pOutLayers[i - count][n - 1] = '\0';
            }
            count += cnt;
        }
    }

    *pOutLayerCount = count;

    return XGL_SUCCESS;
}

LOADER_EXPORT XGL_RESULT XGLAPI xglDbgRegisterMsgCallback(XGL_DBG_MSG_CALLBACK_FUNCTION pfnMsgCallback, void* pUserData)
{
    const struct loader_icd *icd;
    struct loader_instance *inst;
    XGL_RESULT res;
    uint32_t gpu_idx;

    if (!loader.icds_scanned) {
        return loader_msg_callback_add(pfnMsgCallback, pUserData);
    }

    for (inst = loader.instances; inst; inst = inst->next) {
        for (icd = inst->icds; icd; icd = icd->next) {
            for (uint32_t i = 0; i < icd->gpu_count; i++) {
                res = (icd->loader_dispatch + i)->DbgRegisterMsgCallback(
                                                   pfnMsgCallback, pUserData);
                if (res != XGL_SUCCESS) {
                    gpu_idx = i;
                    break;
                }
            }
            if (res != XGL_SUCCESS)
                break;
        }
        if (res != XGL_SUCCESS)
            break;
    }

    /* roll back on errors */
    if (icd) {
        for (struct loader_instance *tmp_inst = loader.instances;
                        tmp_inst != inst; tmp_inst = tmp_inst->next) {
            for (const struct loader_icd * tmp = tmp_inst->icds; tmp != icd;
                                                      tmp = tmp->next) {
                for (uint32_t i = 0; i < icd->gpu_count; i++)
                    (tmp->loader_dispatch + i)->DbgUnregisterMsgCallback(pfnMsgCallback);
            }
        }
        /* and gpus on current icd */
        for (uint32_t i = 0; i < gpu_idx; i++)
            (icd->loader_dispatch + i)->DbgUnregisterMsgCallback(pfnMsgCallback);

        return res;
    }

    return XGL_SUCCESS;
}

LOADER_EXPORT XGL_RESULT XGLAPI xglDbgUnregisterMsgCallback(XGL_DBG_MSG_CALLBACK_FUNCTION pfnMsgCallback)
{
    XGL_RESULT res = XGL_SUCCESS;

    if (!loader.icds_scanned) {
        return loader_msg_callback_remove(pfnMsgCallback);
    }

    for (struct loader_instance *inst = loader.instances; inst;
                                                          inst = inst->next) {
        for (const struct loader_icd * icd = inst->icds; icd;
                                                            icd = icd->next) {
            for (uint32_t i = 0; i < icd->gpu_count; i++) {
                XGL_RESULT r;
                r = (icd->loader_dispatch + i)->DbgUnregisterMsgCallback(pfnMsgCallback);
                if (r != XGL_SUCCESS) {
                    res = r;
                }
            }
        }
    }
    return res;
}

LOADER_EXPORT XGL_RESULT XGLAPI xglDbgSetGlobalOption(XGL_DBG_GLOBAL_OPTION dbgOption, size_t dataSize, const void* pData)
{
    XGL_RESULT res = XGL_SUCCESS;

    if (!loader.icds_scanned) {
        if (dataSize == 0)
            return XGL_ERROR_INVALID_VALUE;

        switch (dbgOption) {
        case XGL_DBG_OPTION_DEBUG_ECHO_ENABLE:
            loader.debug_echo_enable = *((const bool *) pData);
            break;
        case XGL_DBG_OPTION_BREAK_ON_ERROR:
            loader.break_on_error = *((const bool *) pData);
            break;
        case XGL_DBG_OPTION_BREAK_ON_WARNING:
            loader.break_on_warning = *((const bool *) pData);
            break;
        default:
            res = XGL_ERROR_INVALID_VALUE;
            break;
        }

        return res;
    }

    for (struct loader_instance *inst = loader.instances; inst;
                                                          inst = inst->next) {
        for (const struct loader_icd * icd = inst->icds; icd;
                                                          icd = icd->next) {
            for (uint32_t i = 0; i < icd->gpu_count; i++) {
                XGL_RESULT r;
                r = (icd->loader_dispatch + i)->DbgSetGlobalOption(dbgOption,
                                                           dataSize, pData);
                /* unfortunately we cannot roll back */
                if (r != XGL_SUCCESS) {
                   res = r;
                }
            }
        }
    }

    return res;
}

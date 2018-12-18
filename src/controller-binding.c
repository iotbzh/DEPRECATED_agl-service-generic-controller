/*
* Copyright (C) 2018 "IoT.bzh"
* Author Fulup Ar Foll <fulup@iot.bzh>
* Author Romain Forlot <romain@iot.bzh>
* Author Fulup Ar Foll <fulup@iot.bzh>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "controller-binding.h"

/*
 * Controller's sections definition. A section map a JSON section key to a
 * callback in charge of loading and processing the JSON object. Default defined
 * callbacks available:
 * - PluginConfig: to load controller C or LUA plugins
 * - OnloadConfig: Controller's actions to take at when loading
 * - ControlConfig: declare controller's action which will be add as API's verbs
 * - EventConfig: map event received to a controller's action
 */
static CtlSectionT ctrlSections[] = {
    { .key = "plugins", .loadCB = PluginConfig },
    { .key = "controls", .loadCB = ControlConfig },
    { .key = "events", .loadCB = EventConfig },
    { .key = "onload", .loadCB = OnloadConfig },
    { .key = NULL }
};

/**
 * @brief A simple API's verb that count how many it has been called.
 *
 * @param request AFB request with the JSON arguments if the request got some.
 */
static void ctrlapi_ping(afb_req_t request)
{
    static int count = 0;

    count++;
    AFB_ReqNotice(request, "Controller:ping count=%d", count);
    AFB_ReqSuccess(request, json_object_new_int(count), NULL);

    return;
}

/**
 * @brief Authenticate session to raise Level Of Assurance of the session
 *
 * @param request AFB request with the JSON arguments if the request got some.
 */
void ctrlapi_auth(afb_req_t request)
{
    AFB_ReqSetLOA(request, 1);
    AFB_ReqSuccess(request, NULL, NULL);
}

static afb_verb_t CtrlApiVerbs[] = {
    /* VERB'S NAME         FUNCTION TO CALL         SHORT DESCRIPTION */
    { .verb = "ping-global", .callback = ctrlapi_ping, .info = "ping test for API" },
    { .verb = "auth", .callback = ctrlapi_auth, .info = "Authenticate session to raise Level Of Assurance of the session" },
    { .verb = NULL } /* marker for end of the array */
};

static int CtrlLoadStaticVerbs(afb_api_t api, afb_verb_t* verbs)
{
    int errcount = 0;

    for (int idx = 0; verbs[idx].verb; idx++) {
        errcount += afb_api_add_verb(
            api, CtrlApiVerbs[idx].verb, NULL, CtrlApiVerbs[idx].callback,
            (void*)&CtrlApiVerbs[idx], CtrlApiVerbs[idx].auth, 0, 0);
    }

    return errcount;
};

/**
 * @brief Created API init function. Usually here where the controller is
 * finalize its configuration, as its plugins intialized.
 *
 * @param api a created API handle.
 * @return int 0 if ok, other if not.
 */
static int CtrlInitOneApi(afb_api_t api)
{
	CtlConfigT *ctrlConfig;

	if(!api)
		return -1;

	// Retrieve section config from api handle
	ctrlConfig = (CtlConfigT *) afb_api_get_userdata(api);
	if(!ctrlConfig)
		return -2;

    return CtlConfigExec(api, ctrlConfig);
}

/**
 * @brief Created API pre-init function. Here we finalizing the API declaration
 * and setting up the 'init' function, 'on_event' function, the API's verbs etc.
 *
 * @param cbdata the userdata opaque pointer provided by the afb_api_new_api
 * function.
 * @param api the created API handle to setting up
 *
 * @return int 0 if OK, other if not.
 */
static int CtrlLoadOneApi(void* cbdata, afb_api_t api)
{
    CtlConfigT* ctrlConfig = (CtlConfigT*)cbdata;

    // save closure as api's data context
    afb_api_set_userdata(api, ctrlConfig);

    // add some static controls verbs
    int err = CtrlLoadStaticVerbs(api, CtrlApiVerbs);
    if (err) {
        AFB_API_ERROR(api, "CtrlLoadSection fail to register static API verbs");
        return ERROR;
    }

    // load controller's sections for the corresponding for this API
    err = CtlLoadSections(api, ctrlConfig, ctrlSections);

    // declare an event manager for this API
    afb_api_on_event(api, CtrlDispatchApiEvent);

	// declare an init function for this API
	afb_api_on_init(api, CtrlInitOneApi);

    afb_api_seal(api);
    return err;
}

/**
 * @brief Binding entry point for the binder, it's here that APIs could be
 * created and corresponding to the pre-init step of a binding.
 *
 * @param root_api the root API handle given by the binder to create its APIs.
 * @return int 0 if ok, other values if not
 */
int afbBindingEntry(afb_api_t root_api)
{
    AFB_API_NOTICE(root_api, "Controller in afbBindingEntry");

    size_t len = 0, bindingRootDirLen = 0;
    json_object *settings = afb_api_settings(root_api),
                *bpath = NULL;
    const char *envDirList = NULL,
               *configPath = NULL,
               *bindingRootDir = NULL;
    char *dirList,
         *ctlapp_RootDir, *path;

    if(json_object_object_get_ex(settings, "binding-path", &bpath)) {
        ctlapp_RootDir = strdupa(json_object_get_string(bpath));
        path = rindex(ctlapp_RootDir, '/');
        if(strlen(path) < 3)
            return ERROR;
        *++path = '.';
        *++path = '.';
        *++path = '\0';
    }
    else {
        ctlapp_RootDir = alloca(1);
        strcpy(ctlapp_RootDir, "");
    }

    /* Grab environment variable with prefix CONTROL_PREFIX for CONFIG_PATH
     * which give CTLAPP_CONFIG_PATH for this binding. */
    envDirList = getEnvDirList(CONTROL_PREFIX, "CONFIG_PATH");

    /* Get the binding root dir, this is another way if this isn't available
     * from the API's settings. */
    bindingRootDir = GetBindingDirPath(root_api);
    bindingRootDirLen = strlen(bindingRootDir);

    if(envDirList) {
        len = strlen(CONTROL_CONFIG_PATH) + strlen(envDirList) + strlen(ctlapp_RootDir) + bindingRootDirLen + 3;
        dirList = malloc(len + 1);
        snprintf(dirList, len +1, "%s:%s:%s:%s", envDirList, ctlapp_RootDir, bindingRootDir, CONTROL_CONFIG_PATH);
    }
    else {
        len = strlen(CONTROL_CONFIG_PATH) + strlen(ctlapp_RootDir) + bindingRootDirLen + 2;
        dirList = malloc(len + 1);
        snprintf(dirList, len + 1, "%s:%s:%s", bindingRootDir, ctlapp_RootDir, CONTROL_CONFIG_PATH);
    }

    /* Search for the JSON controller configuration file in the freshly composed
     * directory list with no prefix so searching here for file corresponding to
     * the binder process middle name. IE if you specify :
     * "afb-demon --name afb-MyBinder [...]"
     * then this will search for MyBinder*.json */
    configPath = CtlConfigSearch(root_api, dirList, "");
    if (!configPath) {
        AFB_API_ERROR(root_api, "CtlPreInit: No %s* config found in %s ", GetBinderName(), dirList);
        free(dirList);
        return ERROR;
    }

    /* load the JSON configuration file and process the metadata JSON section */
    CtlConfigT* ctrlConfig = CtlLoadMetaData(root_api, configPath);
    if (!ctrlConfig) {
        AFB_API_ERROR(root_api,
            "No valid control config file in:\n-- %s", configPath);
        free(dirList);
        return ERROR;
    }

    if (!ctrlConfig->api) {
        AFB_API_ERROR(root_api,
            "API Missing from metadata in:\n-- %s", configPath);
        free(dirList);
        return ERROR;
    }

    AFB_API_NOTICE(root_api, "Controller API='%s' info='%s'", ctrlConfig->api,
        ctrlConfig->info);

    /* Create one API and initializing it through the function CtrlLoadOneApi
     * given the ctrlConfig struct as an userdata opaque pointer. */
    if (! afb_api_new_api(root_api, ctrlConfig->api, ctrlConfig->info, 1, CtrlLoadOneApi, ctrlConfig)) {
        AFB_API_ERROR(root_api, "API creation failed");
        free(dirList);
        return ERROR;
    }

    free(dirList);
    return 0;
}

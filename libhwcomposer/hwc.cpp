/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
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
#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)
#include <fcntl.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <EGL/egl.h>
#include <utils/Trace.h>
#include <sys/ioctl.h>
#include <overlay.h>
#include <overlayRotator.h>
#include <overlayWriteback.h>
#include <mdp_version.h>
#include "hwc_utils.h"
#include "hwc_fbupdate.h"
#include "hwc_mdpcomp.h"
#include "hwc_dump_layers.h"
#include "external.h"
#include "hwc_copybit.h"
#include "hwc_ad.h"
#include "profiler.h"
#include "hwc_virtual.h"

using namespace qhwc;
using namespace overlay;

#define VSYNC_DEBUG 0
#define BLANK_DEBUG 1

static int hwc_device_open(const struct hw_module_t* module,
                           const char* name,
                           struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

static void reset_panel(struct hwc_composer_device_1* dev);

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 2,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Qualcomm Hardware Composer Module",
        author: "CodeAurora Forum",
        methods: &hwc_module_methods,
        dso: 0,
        reserved: {0},
    }
};

/* In case of non-hybrid WFD session, we are fooling SF by piggybacking on
 * HDMI display ID for virtual. This helper is needed to differentiate their
 * paths in HAL.
 * TODO: Not needed once we have WFD client working on top of Google API's */

static int getDpyforExternalDisplay(hwc_context_t *ctx, int dpy) {
    if(dpy == HWC_DISPLAY_EXTERNAL && ctx->mVirtualonExtActive)
        return HWC_DISPLAY_VIRTUAL;
    return dpy;
}

/*
 * Save callback functions registered to HWC
 */
static void hwc_registerProcs(struct hwc_composer_device_1* dev,
                              hwc_procs_t const* procs)
{
    ALOGI("%s", __FUNCTION__);
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    if(!ctx) {
        ALOGE("%s: Invalid context", __FUNCTION__);
        return;
    }
    ctx->proc = procs;

    // Now that we have the functions needed, kick off
    // the uevent & vsync threads
    init_uevent_thread(ctx);
    init_vsync_thread(ctx);
}

//Helper
static void reset(hwc_context_t *ctx, int numDisplays,
                  hwc_display_contents_1_t** displays) {

    ctx->numActiveDisplays = 0;
    ctx->isPaddingRound = false;

    for(int i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t *list = displays[i];
        // XXX:SurfaceFlinger no longer guarantees that this
        // value is reset on every prepare. However, for the layer
        // cache we need to reset it.
        // We can probably rethink that later on
        if (LIKELY(list && list->numHwLayers > 0)) {
            for(size_t j = 0; j < list->numHwLayers; j++) {
                if(list->hwLayers[j].compositionType != HWC_FRAMEBUFFER_TARGET)
                    list->hwLayers[j].compositionType = HWC_FRAMEBUFFER;
            }

            /* For display devices like SSD and screenrecord, we cannot
             * rely on isActive and connected attributes of dpyAttr to
             * determine if the displaydevice is active. Hence in case if
             * the layer-list is non-null and numHwLayers > 0, we assume
             * the display device to be active.
             */
            ctx->numActiveDisplays += 1;

            if((ctx->mPrevHwLayerCount[i] == 1) and (list->numHwLayers > 1)) {
                /* If the previous cycle for dpy 'i' has 0 AppLayers and the
                 * current cycle has atleast 1 AppLayer, padding round needs
                 * to be invoked on current cycle to free up the resources.
                 */
                ctx->isPaddingRound = true;
            }
            ctx->mPrevHwLayerCount[i] = (int)list->numHwLayers;
        } else {
            ctx->mPrevHwLayerCount[i] = 0;
        }

        if(ctx->mFBUpdate[i])
            ctx->mFBUpdate[i]->reset();
        if(ctx->mCopyBit[i])
            ctx->mCopyBit[i]->reset();
        if(ctx->mLayerRotMap[i])
            ctx->mLayerRotMap[i]->reset();
    }

    ctx->mAD->reset();
    MDPComp::reset();
    if(ctx->mHWCVirtual)
        ctx->mHWCVirtual->destroy(ctx, numDisplays, displays);
}

bool isEqual(float f1, float f2) {
        return ((int)(f1*100) == (int)(f2*100)) ? true : false;
}

static void scaleDisplayFrame(hwc_context_t *ctx, int dpy,
                            hwc_display_contents_1_t *list) {
    uint32_t origXres = ctx->dpyAttr[dpy].xres_orig;
    uint32_t origYres = ctx->dpyAttr[dpy].yres_orig;
    uint32_t fakeXres = ctx->dpyAttr[dpy].xres;
    uint32_t fakeYres = ctx->dpyAttr[dpy].yres;
    float xresRatio = (float)origXres / (float)fakeXres;
    float yresRatio = (float)origYres / (float)fakeYres;
    for (size_t i = 0; i < list->numHwLayers; i++) {
        hwc_layer_1_t *layer = &list->hwLayers[i];
        hwc_rect_t& displayFrame = layer->displayFrame;
        hwc_rect_t sourceCrop = integerizeSourceCrop(layer->sourceCropf);
        uint32_t layerWidth = displayFrame.right - displayFrame.left;
        uint32_t layerHeight = displayFrame.bottom - displayFrame.top;
        uint32_t sourceWidth = sourceCrop.right - sourceCrop.left;
        uint32_t sourceHeight = sourceCrop.bottom - sourceCrop.top;

        if (isEqual(((float)layerWidth / (float)sourceWidth), xresRatio) &&
                isEqual(((float)layerHeight / (float)sourceHeight), yresRatio))
            break;

        displayFrame.left = (int)(xresRatio * (float)displayFrame.left);
        displayFrame.top = (int)(yresRatio * (float)displayFrame.top);
        displayFrame.right = (int)((float)displayFrame.left +
                                   (float)layerWidth * xresRatio);
        displayFrame.bottom = (int)((float)displayFrame.top +
                                    (float)layerHeight * yresRatio);
    }
}

static int hwc_prepare_primary(hwc_composer_device_1 *dev,
        hwc_display_contents_1_t *list) {
    ATRACE_CALL();
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    const int dpy = HWC_DISPLAY_PRIMARY;
    bool fbComp = false;
    if (LIKELY(list && list->numHwLayers > 1) &&
            ctx->dpyAttr[dpy].isActive) {

        if (ctx->dpyAttr[dpy].customFBSize)
            scaleDisplayFrame(ctx, dpy, list);

        reset_layer_prop(ctx, dpy, (int)list->numHwLayers - 1);
        setListStats(ctx, list, dpy);

        fbComp = (ctx->mMDPComp[dpy]->prepare(ctx, list) < 0);

        if (fbComp) {
            const int fbZ = 0;
            ctx->mFBUpdate[dpy]->prepareAndValidate(ctx, list, fbZ);
        }

        if (ctx->mMDP.version < qdutils::MDP_V4_0) {
            if(ctx->mCopyBit[dpy])
                ctx->mCopyBit[dpy]->prepare(ctx, list, dpy);
        }
        setGPUHint(ctx, list);
    }
    return 0;
}

static int hwc_prepare_external(hwc_composer_device_1 *dev,
        hwc_display_contents_1_t *list) {
    ATRACE_CALL();
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    const int dpy = HWC_DISPLAY_EXTERNAL;

    if (LIKELY(list && list->numHwLayers > 1) &&
            ctx->dpyAttr[dpy].isActive &&
            ctx->dpyAttr[dpy].connected) {
        reset_layer_prop(ctx, dpy, (int)list->numHwLayers - 1);
        if(!ctx->dpyAttr[dpy].isPause) {
            ctx->dpyAttr[dpy].isConfiguring = false;
            setListStats(ctx, list, dpy);
            if(ctx->mMDPComp[dpy]->prepare(ctx, list) < 0) {
                const int fbZ = 0;
                ctx->mFBUpdate[dpy]->prepareAndValidate(ctx, list, fbZ);
            }
        } else {
            /* External Display is in Pause state.
             * Mark all application layers as OVERLAY so that
             * GPU will not compose.
             */
            for(size_t i = 0 ;i < (size_t)(list->numHwLayers - 1); i++) {
                hwc_layer_1_t *layer = &list->hwLayers[i];
                layer->compositionType = HWC_OVERLAY;
            }
        }
    }
    return 0;
}

static int hwc_prepare(hwc_composer_device_1 *dev, size_t numDisplays,
                       hwc_display_contents_1_t** displays)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    if (ctx->mPanelResetStatus) {
        ALOGW("%s: panel is in bad state. reset the panel", __FUNCTION__);
        reset_panel(dev);
    }

    //Will be unlocked at the end of set
    ctx->mDrawLock.lock();
    reset(ctx, (int)numDisplays, displays);

    ctx->mOverlay->configBegin();
    ctx->mRotMgr->configBegin();
    overlay::Writeback::configBegin();

    for (int32_t i = ((int32_t)numDisplays-1); i >=0 ; i--) {
        hwc_display_contents_1_t *list = displays[i];
        int dpy = getDpyforExternalDisplay(ctx, i);
        switch(dpy) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_prepare_primary(dev, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
                ret = hwc_prepare_external(dev, list);
                break;
            case HWC_DISPLAY_VIRTUAL:
                if(ctx->mHWCVirtual)
                    ret = ctx->mHWCVirtual->prepare(dev, list);
                break;
            default:
                ret = -EINVAL;
        }
    }

    ctx->mOverlay->configDone();
    ctx->mRotMgr->configDone();
    overlay::Writeback::configDone();

    return ret;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev, int dpy,
                             int event, int enable)
{
    ATRACE_CALL();
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    switch(event) {
        case HWC_EVENT_VSYNC:
            if (ctx->vstate.enable == enable)
                break;
            ret = hwc_vsync_control(ctx, dpy, enable);
            if(ret == 0)
                ctx->vstate.enable = !!enable;
            ALOGD_IF (VSYNC_DEBUG, "VSYNC state changed to %s",
                      (enable)?"ENABLED":"DISABLED");
            break;
#ifdef QCOM_BSP
        case  HWC_EVENT_ORIENTATION:
            if(dpy == HWC_DISPLAY_PRIMARY) {
                Locker::Autolock _l(ctx->mDrawLock);
                // store the primary display orientation
                ctx->deviceOrientation = enable;
            }
            break;
#endif
        default:
            ret = -EINVAL;
    }
    return ret;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int dpy, int blank)
{
    ATRACE_CALL();
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    Locker::Autolock _l(ctx->mDrawLock);
    int ret = 0, value = 0;

    /* In case of non-hybrid WFD session, we are fooling SF by
     * piggybacking on HDMI display ID for virtual.
     * TODO: Not needed once we have WFD client working on top
     * of Google API's.
     */
    dpy = getDpyforExternalDisplay(ctx,dpy);

    ALOGD_IF(BLANK_DEBUG, "%s: %s display: %d", __FUNCTION__,
          blank==1 ? "Blanking":"Unblanking", dpy);
    if(blank) {
        // free up all the overlay pipes in use
        // when we get a blank for either display
        // makes sure that all pipes are freed
        ctx->mOverlay->configBegin();
        ctx->mOverlay->configDone();
        ctx->mRotMgr->clear();
        // If VDS is connected, do not clear WB object as it
        // will end up detaching IOMMU. This is required
        // to send black frame to WFD sink on power suspend.
        // Note: With this change, we keep the WriteBack object
        // alive on power suspend for AD use case.
    }
    switch(dpy) {
    case HWC_DISPLAY_PRIMARY:
        value = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        if(ioctl(ctx->dpyAttr[dpy].fd, FBIOBLANK, value) < 0 ) {
            ALOGE("%s: Failed to handle blank event(%d) for Primary!!",
                  __FUNCTION__, blank );
            return -1;
        }

        if(!blank) {
            // Enable HPD here, as during bootup unblank is called
            // when SF is completely initialized
            ctx->mExtDisplay->setHPD(1);
        }

        ctx->dpyAttr[dpy].isActive = !blank;

        if(ctx->mVirtualonExtActive) {
            /* if mVirtualonExtActive is true, display hal will
             * receive unblank calls for non-hybrid WFD solution
             * since we piggyback on HDMI.
             * TODO: Not needed once we have WFD client working on top
             of Google API's */
            break;
        }
    case HWC_DISPLAY_VIRTUAL:
        /* There are two ways to reach this block of code.

         * Display hal has received unblank call on HWC_DISPLAY_EXTERNAL
         and ctx->mVirtualonExtActive is true. In this case, non-hybrid
         WFD is active. If so, getDpyforExternalDisplay will return dpy
         as HWC_DISPLAY_VIRTUAL.

         * Display hal has received unblank call on HWC_DISPLAY_PRIMARY
         and since SF is not aware of VIRTUAL DISPLAY being handle by HWC,
         it wont send blank / unblank events for it. We piggyback on
         PRIMARY DISPLAY events to release mdp pipes and
         activate/deactivate VIRTUAL DISPLAY.

         * TODO: This separate case statement is not needed once we have
         WFD client working on top of Google API's.

         */

        if(ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].connected) {
            if(blank and (!ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].isPause)) {
                int dpy = HWC_DISPLAY_VIRTUAL;
                if(!Overlay::displayCommit(ctx->dpyAttr[dpy].fd)) {
                    ALOGE("%s: display commit fail for virtual!", __FUNCTION__);
                    ret = -1;
                }
            }
            ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].isActive = !blank;
        }
        break;
    case HWC_DISPLAY_EXTERNAL:
        if(blank) {
            if(!Overlay::displayCommit(ctx->dpyAttr[dpy].fd)) {
                ALOGE("%s: display commit fail for external!", __FUNCTION__);
                ret = -1;
            }
        }
        ctx->dpyAttr[dpy].isActive = !blank;
        break;
    default:
        return -EINVAL;
    }

    ALOGD_IF(BLANK_DEBUG, "%s: Done %s display: %d", __FUNCTION__,
          blank ? "blanking":"unblanking", dpy);
    return ret;
}

static void reset_panel(struct hwc_composer_device_1* dev)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    if (!ctx->dpyAttr[HWC_DISPLAY_PRIMARY].isActive) {
        ALOGD ("%s : Display OFF - Skip BLANK & UNBLANK", __FUNCTION__);
        ctx->mPanelResetStatus = false;
        return;
    }

    ALOGD("%s: calling BLANK DISPLAY", __FUNCTION__);
    ret = hwc_blank(dev, HWC_DISPLAY_PRIMARY, 1);
    if (ret < 0) {
        ALOGE("%s: FBIOBLANK failed to BLANK:  %s", __FUNCTION__,
                                                            strerror(errno));
    }

    ALOGD("%s: calling UNBLANK DISPLAY and enabling vsync", __FUNCTION__);
    ret = hwc_blank(dev, HWC_DISPLAY_PRIMARY, 0);
    if (ret < 0) {
        ALOGE("%s: FBIOBLANK failed to UNBLANK : %s", __FUNCTION__,
                                                            strerror(errno));
    }
    hwc_vsync_control(ctx, HWC_DISPLAY_PRIMARY, 1);

    ctx->mPanelResetStatus = false;
}


static int hwc_query(struct hwc_composer_device_1* dev,
                     int param, int* value)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    int supported = HWC_DISPLAY_PRIMARY_BIT;

    switch (param) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // Not supported for now
        value[0] = 0;
        break;
    case HWC_DISPLAY_TYPES_SUPPORTED:
        if(ctx->mMDP.hasOverlay) {
            supported |= HWC_DISPLAY_VIRTUAL_BIT;
            if(!(qdutils::MDPVersion::getInstance().is8x26() ||
                        qdutils::MDPVersion::getInstance().is8x16()))
                supported |= HWC_DISPLAY_EXTERNAL_BIT;
        }
        value[0] = supported;
        break;
    case HWC_FORMAT_RB_SWAP:
        value[0] = 1;
        break;
    case HWC_COLOR_FILL:
        value[0] = 1;
        break;
    default:
        return -EINVAL;
    }
    return 0;

}


static int hwc_set_primary(hwc_context_t *ctx, hwc_display_contents_1_t* list) {
    ATRACE_CALL();
    int ret = 0;
    const int dpy = HWC_DISPLAY_PRIMARY;
    if (LIKELY(list) && ctx->dpyAttr[dpy].isActive) {
        size_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        int fd = -1; //FenceFD from the Copybit(valid in async mode)
        bool copybitDone = false;
        if(ctx->mCopyBit[dpy])
            copybitDone = ctx->mCopyBit[dpy]->draw(ctx, list, dpy, &fd);
        if(list->numHwLayers > 1)
            hwc_sync(ctx, list, dpy, fd);

        // Dump the layers for primary
        if(ctx->mHwcDebug[dpy])
            ctx->mHwcDebug[dpy]->dumpLayers(list);

        if (!ctx->mMDPComp[dpy]->draw(ctx, list)) {
            ALOGE("%s: MDPComp draw failed", __FUNCTION__);
            ret = -1;
        }

        //TODO We dont check for SKIP flag on this layer because we need PAN
        //always. Last layer is always FB
        private_handle_t *hnd = (private_handle_t *)fbLayer->handle;
        if(copybitDone && ctx->mMDP.version >= qdutils::MDP_V4_0) {
            hnd = ctx->mCopyBit[dpy]->getCurrentRenderBuffer();
        }

        if(hnd) {
            if (!ctx->mFBUpdate[dpy]->draw(ctx, hnd)) {
                ALOGE("%s: FBUpdate draw failed", __FUNCTION__);
                ret = -1;
            }
        }

        if(!Overlay::displayCommit(ctx->dpyAttr[dpy].fd,
                                            ctx->listStats[dpy].roi)) {
            ALOGE("%s: display commit fail for %d dpy!", __FUNCTION__, dpy);
            ret = -1;
        }

    }

    closeAcquireFds(list);
    return ret;
}

static int hwc_set_external(hwc_context_t *ctx,
                            hwc_display_contents_1_t* list)
{
    ATRACE_CALL();
    int ret = 0;

    const int dpy = HWC_DISPLAY_EXTERNAL;


    if (LIKELY(list) && ctx->dpyAttr[dpy].isActive &&
        ctx->dpyAttr[dpy].connected &&
        !ctx->dpyAttr[dpy].isPause) {
        size_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        int fd = -1; //FenceFD from the Copybit(valid in async mode)
        bool copybitDone = false;
        if(ctx->mCopyBit[dpy])
            copybitDone = ctx->mCopyBit[dpy]->draw(ctx, list, dpy, &fd);

        if(list->numHwLayers > 1)
            hwc_sync(ctx, list, dpy, fd);

        // Dump the layers for external
        if(ctx->mHwcDebug[dpy])
            ctx->mHwcDebug[dpy]->dumpLayers(list);

        if (!ctx->mMDPComp[dpy]->draw(ctx, list)) {
            ALOGE("%s: MDPComp draw failed", __FUNCTION__);
            ret = -1;
        }

        int extOnlyLayerIndex =
                ctx->listStats[dpy].extOnlyLayerIndex;

        private_handle_t *hnd = (private_handle_t *)fbLayer->handle;
        if(extOnlyLayerIndex!= -1) {
            hwc_layer_1_t *extLayer = &list->hwLayers[extOnlyLayerIndex];
            hnd = (private_handle_t *)extLayer->handle;
        } else if(copybitDone) {
            hnd = ctx->mCopyBit[dpy]->getCurrentRenderBuffer();
        }

        if(hnd && !isYuvBuffer(hnd)) {
            if (!ctx->mFBUpdate[dpy]->draw(ctx, hnd)) {
                ALOGE("%s: FBUpdate::draw fail!", __FUNCTION__);
                ret = -1;
            }
        }

        if(!Overlay::displayCommit(ctx->dpyAttr[dpy].fd)) {
            ALOGE("%s: display commit fail for %d dpy!", __FUNCTION__, dpy);
            ret = -1;
        }
    }

    closeAcquireFds(list);
    return ret;
}

static int hwc_set(hwc_composer_device_1 *dev,
                   size_t numDisplays,
                   hwc_display_contents_1_t** displays)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    for (int i = 0; i < (int)numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        int dpy = getDpyforExternalDisplay(ctx, i);
        switch(dpy) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_set_primary(ctx, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
                ret = hwc_set_external(ctx, list);
                break;
            case HWC_DISPLAY_VIRTUAL:
                if(ctx->mHWCVirtual)
                    ret = ctx->mHWCVirtual->set(ctx, list);
                break;
            default:
                ret = -EINVAL;
        }
    }
    // This is only indicative of how many times SurfaceFlinger posts
    // frames to the display.
    CALC_FPS();
    MDPComp::resetIdleFallBack();
    ctx->mVideoTransFlag = false;
    if(ctx->mRotMgr->getNumActiveSessions() == 0)
        Overlay::setDMAMode(Overlay::DMA_LINE_MODE);
    //Was locked at the beginning of prepare
    ctx->mDrawLock.unlock();
    return ret;
}

int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
        uint32_t* configs, size_t* numConfigs) {
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    disp = getDpyforExternalDisplay(ctx, disp);
    //in 1.1 there is no way to choose a config, report as config id # 0
    //This config is passed to getDisplayAttributes. Ignore for now.
    switch(disp) {
        case HWC_DISPLAY_PRIMARY:
            if(*numConfigs > 0) {
                configs[0] = 0;
                *numConfigs = 1;
            }
            ret = 0; //NO_ERROR
            break;
        case HWC_DISPLAY_EXTERNAL:
        case HWC_DISPLAY_VIRTUAL:
            ret = -1; //Not connected
            if(ctx->dpyAttr[disp].connected) {
                ret = 0; //NO_ERROR
                if(*numConfigs > 0) {
                    configs[0] = 0;
                    *numConfigs = 1;
                }
            }
            break;
    }
    return ret;
}

int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
        uint32_t /*config*/, const uint32_t* attributes, int32_t* values) {

    hwc_context_t* ctx = (hwc_context_t*)(dev);
    disp = getDpyforExternalDisplay(ctx, disp);
    //If hotpluggable displays(i.e, HDMI, WFD) are inactive return error
    if( (disp != HWC_DISPLAY_PRIMARY) && !ctx->dpyAttr[disp].connected) {
        return -1;
    }

    //From HWComposer
    static const uint32_t DISPLAY_ATTRIBUTES[] = {
        HWC_DISPLAY_VSYNC_PERIOD,
        HWC_DISPLAY_WIDTH,
        HWC_DISPLAY_HEIGHT,
        HWC_DISPLAY_DPI_X,
        HWC_DISPLAY_DPI_Y,
        HWC_DISPLAY_NO_ATTRIBUTE,
    };

    const size_t NUM_DISPLAY_ATTRIBUTES = (sizeof(DISPLAY_ATTRIBUTES) /
            sizeof(DISPLAY_ATTRIBUTES)[0]);

    for (size_t i = 0; i < NUM_DISPLAY_ATTRIBUTES - 1; i++) {
        switch (attributes[i]) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            values[i] = ctx->dpyAttr[disp].vsync_period;
            break;
        case HWC_DISPLAY_WIDTH:
            values[i] = ctx->dpyAttr[disp].xres;
            ALOGD("%s disp = %d, width = %d",__FUNCTION__, disp,
                    ctx->dpyAttr[disp].xres);
            break;
        case HWC_DISPLAY_HEIGHT:
            values[i] = ctx->dpyAttr[disp].yres;
            ALOGD("%s disp = %d, height = %d",__FUNCTION__, disp,
                    ctx->dpyAttr[disp].yres);
            break;
        case HWC_DISPLAY_DPI_X:
            values[i] = (int32_t) (ctx->dpyAttr[disp].xdpi*1000.0);
            break;
        case HWC_DISPLAY_DPI_Y:
            values[i] = (int32_t) (ctx->dpyAttr[disp].ydpi*1000.0);
            break;
        default:
            ALOGE("Unknown display attribute %d",
                    attributes[i]);
            return -EINVAL;
        }
    }
    return 0;
}

void hwc_dump(struct hwc_composer_device_1* dev, char *buff, int buff_len)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    Locker::Autolock _l(ctx->mDrawLock);
    android::String8 aBuf("");
    dumpsys_log(aBuf, "Qualcomm HWC state:\n");
    dumpsys_log(aBuf, "  MDPVersion=%d\n", ctx->mMDP.version);
    dumpsys_log(aBuf, "  DisplayPanel=%c\n", ctx->mMDP.panel);
    for(int dpy = 0; dpy < HWC_NUM_DISPLAY_TYPES; dpy++) {
        if(ctx->mMDPComp[dpy])
            ctx->mMDPComp[dpy]->dump(aBuf);
    }
    char ovDump[2048] = {'\0'};
    ctx->mOverlay->getDump(ovDump, 2048);
    dumpsys_log(aBuf, ovDump);
    ovDump[0] = '\0';
    ctx->mRotMgr->getDump(ovDump, 1024);
    dumpsys_log(aBuf, ovDump);
    ovDump[0] = '\0';
    if(Writeback::getDump(ovDump, 1024)) {
        dumpsys_log(aBuf, ovDump);
        ovDump[0] = '\0';
    }
    strlcpy(buff, aBuf.string(), buff_len);
}

static int hwc_device_close(struct hw_device_t *dev)
{
    if(!dev) {
        ALOGE("%s: NULL device pointer", __FUNCTION__);
        return -1;
    }
    closeContext((hwc_context_t*)dev);
    free(dev);

    return 0;
}

static int hwc_device_open(const struct hw_module_t* module, const char* name,
                           struct hw_device_t** device)
{
    int status = -EINVAL;

    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));
        if(dev == NULL)
            return status;
        memset(dev, 0, sizeof(*dev));

        //Initialize hwc context
        initContext(dev);

        //Setup HWC methods
        dev->device.common.tag          = HARDWARE_DEVICE_TAG;
        dev->device.common.version      = HWC_DEVICE_API_VERSION_1_3;
        dev->device.common.module       = const_cast<hw_module_t*>(module);
        dev->device.common.close        = hwc_device_close;
        dev->device.prepare             = hwc_prepare;
        dev->device.set                 = hwc_set;
        dev->device.eventControl        = hwc_eventControl;
        dev->device.blank               = hwc_blank;
        dev->device.query               = hwc_query;
        dev->device.registerProcs       = hwc_registerProcs;
        dev->device.dump                = hwc_dump;
        dev->device.getDisplayConfigs   = hwc_getDisplayConfigs;
        dev->device.getDisplayAttributes = hwc_getDisplayAttributes;
        *device = &dev->device.common;
        status = 0;
    }
    return status;
}

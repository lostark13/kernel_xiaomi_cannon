/*
 * Copyright (c) 2013-2019 The Linux Foundation. All rights reserved.
 * Not a contribution.
 *
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "AudioPolicyManagerCustom"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

// A device mask for all audio output devices that are considered "remote" when evaluating
// active output devices in isStreamActiveRemotely()
#define APM_AUDIO_OUT_DEVICE_REMOTE_ALL  AUDIO_DEVICE_OUT_REMOTE_SUBMIX
// A device mask for all audio input and output devices where matching inputs/outputs on device
// type alone is not enough: the address must match too
#define APM_AUDIO_DEVICE_MATCH_ADDRESS_ALL (AUDIO_DEVICE_IN_REMOTE_SUBMIX | \
                                            AUDIO_DEVICE_OUT_REMOTE_SUBMIX)
#define SAMPLE_RATE_8000 8000
#include <inttypes.h>
#include <math.h>

#include <cutils/properties.h>
#include <utils/Log.h>
#include <hardware/audio.h>
#include <hardware/audio_effect.h>
#include <media/AudioParameter.h>
#include <soundtrigger/SoundTrigger.h>
#include "AudioPolicyManager.h"
#include <policy.h>

namespace android {
/*audio policy: workaround for truncated touch sounds*/
//FIXME: workaround for truncated touch sounds
// to be removed when the problem is handled by system UI
#define TOUCH_SOUND_FIXED_DELAY_MS 100

sp<APMConfigHelper> AudioPolicyManagerCustom::mApmConfigs = new APMConfigHelper();

audio_output_flags_t AudioPolicyManagerCustom::getFallBackPath()
{
    audio_output_flags_t flag = AUDIO_OUTPUT_FLAG_FAST;
    const char *fallback_path = mApmConfigs->getVoiceConcFallbackPath().c_str();

    if (strlen(fallback_path) > 0) {
        if (!strncmp(fallback_path, "deep-buffer", 11)) {
            flag = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
        }
        else if (!strncmp(fallback_path, "fast", 4)) {
            flag = AUDIO_OUTPUT_FLAG_FAST;
        }
        else {
            ALOGD("voice_conc:not a recognised path(%s) in prop vendor.voice.conc.fallbackpath",
                 fallback_path);
        }
    }
    else {
        ALOGD("voice_conc:prop vendor.voice.conc.fallbackpath not set");
    }

    ALOGD("voice_conc:picked up flag(0x%x) from prop vendor.voice.conc.fallbackpath",
        flag);

    return flag;
}

void AudioPolicyManagerCustom::moveGlobalEffect()
{
    audio_io_handle_t dstOutput = getOutputForEffect();
    if (hasPrimaryOutput() && dstOutput != mPrimaryOutput->mIoHandle)
        mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX,
                                   mPrimaryOutput->mIoHandle, dstOutput);
}

// ----------------------------------------------------------------------------
// AudioPolicyInterface implementation
// ----------------------------------------------------------------------------
extern "C" AudioPolicyInterface* createAudioPolicyManager(
         AudioPolicyClientInterface *clientInterface)
{
     return new AudioPolicyManagerCustom(clientInterface);
}

extern "C" void destroyAudioPolicyManager(AudioPolicyInterface *interface)
{
     delete interface;
}

status_t AudioPolicyManagerCustom::setDeviceConnectionStateInt(audio_devices_t device,
                                                         audio_policy_dev_state_t state,
                                                         const char *device_address,
                                                         const char *device_name)
{
    ALOGD("setDeviceConnectionStateInt() device: 0x%X, state %d, address %s name %s",
            device, state, device_address, device_name);

    // connect/disconnect only 1 device at a time
    if (!audio_is_output_device(device) && !audio_is_input_device(device)) return BAD_VALUE;

    sp<DeviceDescriptor> devDesc =
            mHwModules.getDeviceDescriptor(device, device_address, device_name);

    // handle output devices
    if (audio_is_output_device(device)) {
        SortedVector <audio_io_handle_t> outputs;

        ssize_t index = mAvailableOutputDevices.indexOf(devDesc);

        // save a copy of the opened output descriptors before any output is opened or closed
        // by checkOutputsForDevice(). This will be needed by checkOutputForAllStrategies()
        mPreviousOutputs = mOutputs;
        switch (state)
        {
        // handle output device connection
        case AUDIO_POLICY_DEVICE_STATE_AVAILABLE: {
            if (index >= 0) {
                if (mApmConfigs->isHDMISpkEnabled() &&
                        (popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                   if (!strncmp(device_address, "hdmi_spkr", 9)) {
                        mHdmiAudioDisabled = false;
                    } else {
                        mHdmiAudioEvent = true;
                    }
                }
                ALOGW("setDeviceConnectionState() device already connected: %x", device);
                return INVALID_OPERATION;
            }
            ALOGV("setDeviceConnectionState() connecting device %x", device);

            // register new device as available
            index = mAvailableOutputDevices.add(devDesc);
            if (mApmConfigs->isHDMISpkEnabled() &&
                    (popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                if (!strncmp(device_address, "hdmi_spkr", 9)) {
                    mHdmiAudioDisabled = false;
                } else {
                    mHdmiAudioEvent = true;
                }
                if (mHdmiAudioDisabled || !mHdmiAudioEvent) {
                    mAvailableOutputDevices.remove(devDesc);
                    ALOGW("HDMI sink not connected, do not route audio to HDMI out");
                    return INVALID_OPERATION;
                }
            }
            if (index >= 0) {
                sp<HwModule> module = mHwModules.getModuleForDevice(device);
                if (module == 0) {
                    ALOGD("setDeviceConnectionState() could not find HW module for device %08x",
                          device);
                    mAvailableOutputDevices.remove(devDesc);
                    return INVALID_OPERATION;
                }
                mAvailableOutputDevices[index]->attach(module);
            } else {
                return NO_MEMORY;
            }

            // Before checking outputs, broadcast connect event to allow HAL to retrieve dynamic
            // parameters on newly connected devices (instead of opening the outputs...)
            broadcastDeviceConnectionState(device, state, devDesc->address());

            if (checkOutputsForDevice(devDesc, state, outputs, devDesc->address()) != NO_ERROR) {
                mAvailableOutputDevices.remove(devDesc);

                broadcastDeviceConnectionState(device, AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                               devDesc->address());
                return INVALID_OPERATION;
            }
            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
            if (device == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
                chkDpConnAndAllowedForVoice();
            }

            // outputs should never be empty here
            ALOG_ASSERT(outputs.size() != 0, "setDeviceConnectionState():"
                    "checkOutputsForDevice() returned no outputs but status OK");
            ALOGV("setDeviceConnectionState() checkOutputsForDevice() returned %zu outputs",
                  outputs.size());

            } break;
        // handle output device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
                if (mApmConfigs->isHDMISpkEnabled() &&
                        (popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                    if (!strncmp(device_address, "hdmi_spkr", 9)) {
                        mHdmiAudioDisabled = true;
                    } else {
                        mHdmiAudioEvent = false;
                    }
                }
                ALOGW("setDeviceConnectionState() device not connected: %x", device);
                return INVALID_OPERATION;
            }

            ALOGV("setDeviceConnectionState() disconnecting output device %x", device);

            // Send Disconnect to HALs
            broadcastDeviceConnectionState(device, state, devDesc->address());

            // remove device from available output devices
            mAvailableOutputDevices.remove(devDesc);
            if (mApmConfigs->isHDMISpkEnabled() &&
                    (popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                if (!strncmp(device_address, "hdmi_spkr", 9)) {
                    mHdmiAudioDisabled = true;
                } else {
                    mHdmiAudioEvent = false;
                }
            }
            checkOutputsForDevice(devDesc, state, outputs, devDesc->address());

            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
            if (device == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
                mEngine->setDpConnAndAllowedForVoice(false);
            }
            } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        // checkA2dpSuspend must run before checkOutputForAllStrategies so that A2DP
        // output is suspended before any tracks are moved to it
        checkA2dpSuspend();

        if (!outputs.isEmpty()) {
            for (size_t i = 0; i < outputs.size(); i++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(outputs[i]);
                // close voip output before track invalidation to allow creation of
                // new voip stream from restoreTrack
                if ((desc->mFlags == (AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_VOIP_RX)) != 0) {
                    closeOutput(outputs[i]);
                    outputs.remove(outputs[i]);
                }
            }
        }

        checkOutputForAllStrategies();
        // outputs must be closed after checkOutputForAllStrategies() is executed
        if (!outputs.isEmpty()) {
            for (size_t i = 0; i < outputs.size(); i++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(outputs[i]);
                // close unused outputs after device disconnection or direct outputs that have been
                // opened by checkOutputsForDevice() to query dynamic parameters
                if ((state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) ||
                        (((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) &&
                         (desc->mDirectOpenCount == 0))) {
                    closeOutput(outputs[i]);
                }
            }
            // check again after closing A2DP output to reset mA2dpSuspended if needed
            checkA2dpSuspend();
        }

        // handle FM device connection state to trigger FM AFE loopback
        if (mApmConfigs->isFMPowerOptEnabled() &&
               device == AUDIO_DEVICE_OUT_FM && hasPrimaryOutput()) {
           audio_devices_t newDevice = AUDIO_DEVICE_NONE;
           if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
               /*
                 when mPrimaryOutput->start() is called for FM it would check if isActive() is true
                 or not as streamActiveCount=0 so isActive() would return false and curActiveCount will be
                 1 and then the streamActiveCount will be increased by 1 for FM case.Updating curActiveCount
                 is important as in case of adding other tracks when FM is still active isActive()
                 will always be true as streamActiveCount will always be > 0,Hence curActiveCount will never
                 update for them. However ,when fm stops and the track stops too streamActiveCount will be 0
                 isActive will false,it will check if curActiveCount < 1 as curActiveCount was never
                 updated so LOG_FATAL will cause the AudioServer to die.Hence this start() call will
                 ensure that curActiveCount is updated at least once when FM starts prior to other
                 tracks and on calling of stop() LOG_FATAL is not called.
               */
               mPrimaryOutput->start();
               for (const std::pair<sp<TrackClientDescriptor>, size_t>& client_pair : mPrimaryOutput->getActiveClients()) {
                    if (client_pair.first->stream() == AUDIO_STREAM_MUSIC) {
                        mPrimaryOutput->changeStreamActiveCount(client_pair.first, 1);
                        break;
                    }
               }
               newDevice = (audio_devices_t)(getNewOutputDevice(mPrimaryOutput, false)|AUDIO_DEVICE_OUT_FM);
               mFMIsActive = true;
               mPrimaryOutput->mDevice = newDevice & ~AUDIO_DEVICE_OUT_FM;
           } else {
               newDevice = (audio_devices_t)(getNewOutputDevice(mPrimaryOutput, false));
               mFMIsActive = false;
               for (const std::pair<sp<TrackClientDescriptor>, size_t>& client_pair : mPrimaryOutput->getActiveClients()) {
                    if (client_pair.first->stream() == AUDIO_STREAM_MUSIC) {
                        mPrimaryOutput->changeStreamActiveCount(client_pair.first, -1);
                        break;
                    }
               }
               /*
                 mPrimaryOutput->stop() is called as because of calling of start()
                 in FM case curActiveCount is getting updated and hence stop() is
                 called so that curActiveCount gets decremented and if any tracks
                 are added after FM stops they may get curActiveCount=0 ,ouptput
                 curActiveCount can be properly updated
               */
               mPrimaryOutput->stop();
           }
           AudioParameter param = AudioParameter();
           param.addInt(String8("handle_fm"), (int)newDevice);
           mpClientInterface->setParameters(mPrimaryOutput->mIoHandle, param.toString());
        }

        updateDevicesAndOutputs();
        if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL && hasPrimaryOutput()) {
            audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
            updateCallRouting(newDevice);
        }

        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) || (desc != mPrimaryOutput)) {
                audio_devices_t newDevice = getNewOutputDevice(desc, true /*fromCache*/);
                // do not force device change on duplicated output because if device is 0, it will
                // also force a device 0 for the two outputs it is duplicated to which may override
                // a valid device selection on those outputs.
                bool force = !desc->isDuplicated()
                        && (!device_distinguishes_on_address(device)
                                // always force when disconnecting (a non-duplicated device)
                                || (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE));
                setOutputDevice(desc, newDevice, force, 0);
            }
        }

        if (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
            cleanUpForDevice(devDesc);
        }

        mpClientInterface->onAudioPortListUpdate();
        return NO_ERROR;
    }  // end if is output device

    // handle input devices
    if (audio_is_input_device(device)) {
        SortedVector <audio_io_handle_t> inputs;

        ssize_t index = mAvailableInputDevices.indexOf(devDesc);
        switch (state)
        {
        // handle input device connection
        case AUDIO_POLICY_DEVICE_STATE_AVAILABLE: {
            if (index >= 0) {
                ALOGW("setDeviceConnectionState() device already connected: %d", device);
                return INVALID_OPERATION;
            }
            sp<HwModule> module = mHwModules.getModuleForDevice(device);
            if (module == NULL) {
                ALOGW("setDeviceConnectionState(): could not find HW module for device %08x",
                      device);
                return INVALID_OPERATION;
            }

            // Before checking intputs, broadcast connect event to allow HAL to retrieve dynamic
            // parameters on newly connected devices (instead of opening the inputs...)
            broadcastDeviceConnectionState(device, state, devDesc->address());

            if (checkInputsForDevice(devDesc, state, inputs, devDesc->address()) != NO_ERROR) {
                broadcastDeviceConnectionState(device, AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                               devDesc->address());
                return INVALID_OPERATION;
            }

            index = mAvailableInputDevices.add(devDesc);
            if (index >= 0) {
                mAvailableInputDevices[index]->attach(module);
            } else {
                return NO_MEMORY;
            }

            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
        } break;

        // handle input device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
                ALOGW("setDeviceConnectionState() device not connected: %d", device);
                return INVALID_OPERATION;
            }

            ALOGV("setDeviceConnectionState() disconnecting input device %x", device);

            // Set Disconnect to HALs
            broadcastDeviceConnectionState(device, state, devDesc->address());

            checkInputsForDevice(devDesc, state, inputs, devDesc->address());
            mAvailableInputDevices.remove(devDesc);

            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
        } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        closeAllInputs();
        /*audio policy: fix call volume over USB*/
        // As the input device list can impact the output device selection, update
        // getDeviceForStrategy() cache
        updateDevicesAndOutputs();

        if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL && hasPrimaryOutput()) {
            audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
            updateCallRouting(newDevice);
        }

        if (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
            cleanUpForDevice(devDesc);
        }

        mpClientInterface->onAudioPortListUpdate();
        return NO_ERROR;
    } // end if is input device

    ALOGW("setDeviceConnectionState() invalid device: %x", device);
    return BAD_VALUE;
}

void AudioPolicyManagerCustom::chkDpConnAndAllowedForVoice()
{
    String8 value;
    bool connAndAllowed = false;
    String8 valueStr = mpClientInterface->getParameters((audio_io_handle_t)0,
                                                        String8("dp_for_voice"));

    AudioParameter result = AudioParameter(valueStr);
    if (result.get(String8("dp_for_voice"), value) == NO_ERROR) {
        connAndAllowed = value.contains("true");
    }
    mEngine->setDpConnAndAllowedForVoice(connAndAllowed);
}

bool AudioPolicyManagerCustom::isInvalidationOfMusicStreamNeeded(routing_strategy strategy)
{
    if (strategy == STRATEGY_MEDIA) {
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> newOutputDesc = mOutputs.valueAt(i);
            if (newOutputDesc->mFormat == AUDIO_FORMAT_DSD)
                return false;
        }
    }
    return true;
}

void AudioPolicyManagerCustom::checkOutputForStrategy(routing_strategy strategy)
{
    audio_devices_t oldDevice = getDeviceForStrategy(strategy, true /*fromCache*/);
    audio_devices_t newDevice = getDeviceForStrategy(strategy, false /*fromCache*/);
    SortedVector<audio_io_handle_t> srcOutputs = getOutputsForDevice(oldDevice, mOutputs);
    SortedVector<audio_io_handle_t> dstOutputs = getOutputsForDevice(newDevice, mOutputs);

    // also take into account external policy-related changes: add all outputs which are
    // associated with policies in the "before" and "after" output vectors
    ALOGV("checkOutputForStrategy(): policy related outputs");
    for (size_t i = 0 ; i < mPreviousOutputs.size() ; i++) {
        const sp<SwAudioOutputDescriptor> desc = mPreviousOutputs.valueAt(i);
        if (desc != 0 && desc->mPolicyMix != NULL) {
            srcOutputs.add(desc->mIoHandle);
            ALOGV(" previous outputs: adding %d", desc->mIoHandle);
        }
    }
    for (size_t i = 0 ; i < mOutputs.size() ; i++) {
        const sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if (desc != 0 && desc->mPolicyMix != NULL) {
            dstOutputs.add(desc->mIoHandle);
            ALOGV(" new outputs: adding %d", desc->mIoHandle);
        }
    }

    if ((srcOutputs != dstOutputs) && isInvalidationOfMusicStreamNeeded(strategy)) {
        AudioPolicyManager::checkOutputForStrategy(strategy);
    }
}

// This function checks for the parameters which can be offloaded.
// This can be enhanced depending on the capability of the DSP and policy
// of the system.
bool AudioPolicyManagerCustom::isOffloadSupported(const audio_offload_info_t& offloadInfo)
{
    ALOGV("isOffloadSupported: SR=%u, CM=0x%x, Format=0x%x, StreamType=%d,"
     " BitRate=%u, duration=%" PRId64 " us, has_video=%d",
     offloadInfo.sample_rate, offloadInfo.channel_mask,
     offloadInfo.format,
     offloadInfo.stream_type, offloadInfo.bit_rate, offloadInfo.duration_us,
     offloadInfo.has_video);

     if (mMasterMono) {
        return false; // no offloading if mono is set.
     }

    if (mApmConfigs->isVoiceConcEnabled()) {
        if (mApmConfigs->isVoicePlayConcDisabled() && isInCall()) {
            ALOGD("\n copl: blocking  compress offload on call mode\n");
            return false;
        }
    }
    if (mApmConfigs->isVoiceDSDConcDisabled() &&
        isInCall() &&  (offloadInfo.format == AUDIO_FORMAT_DSD)) {
        ALOGD("blocking DSD compress offload on call mode");
        return false;
    }
    if (mApmConfigs->isRecPlayConcEnabled()) {
        if (mApmConfigs->isRecPlayConcDisabled() &&
             ((true == mIsInputRequestOnProgress) || (mInputs.activeInputsCountOnDevices() > 0))) {
            ALOGD("copl: blocking  compress offload for record concurrency");
            return false;
        }
    }
    // Check if stream type is music, then only allow offload as of now.
    if (offloadInfo.stream_type != AUDIO_STREAM_MUSIC)
    {
        ALOGV("isOffloadSupported: stream_type != MUSIC, returning false");
        return false;
    }

    // Check if offload has been disabled
    bool offloadDisabled = property_get_bool("audio.offload.disable", false);
    if (offloadDisabled) {
        ALOGI("offload disabled by audio.offload.disable=%d", offloadDisabled);
        return false;
    }

    //check if it's multi-channel AAC (includes sub formats) and FLAC format
    if ((popcount(offloadInfo.channel_mask) > 2) &&
        (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC) ||
        ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_VORBIS))) {
        ALOGD("offload disabled for multi-channel AAC,FLAC and VORBIS format");
        return false;
    }

    if (mApmConfigs->isExtnFormatsEnabled()) {
        //check if it's multi-channel FLAC/ALAC/WMA format with sample rate > 48k
        if ((popcount(offloadInfo.channel_mask) > 2) &&
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_FLAC) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_ALAC) && (offloadInfo.sample_rate > 48000)) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA) && (offloadInfo.sample_rate > 48000)) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA_PRO) && (offloadInfo.sample_rate > 48000)) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC_ADTS))) {
                ALOGD("offload disabled for multi-channel FLAC/ALAC/WMA/AAC_ADTS clips with sample rate > 48kHz");
            return false;
        }

        // check against wma std bit rate restriction
        if ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA) {
            int32_t sr_id = -1;
            uint32_t min_bitrate, max_bitrate;
            for (int i = 0; i < WMA_STD_NUM_FREQ; i++) {
                if (offloadInfo.sample_rate == wmaStdSampleRateTbl[i]) {
                    sr_id = i;
                    break;
                }
            }
            if ((sr_id < 0) || (popcount(offloadInfo.channel_mask) > 2)
                    || (popcount(offloadInfo.channel_mask) <= 0)) {
                ALOGE("invalid sample rate or channel count");
                return false;
            }

            min_bitrate = wmaStdMinAvgByteRateTbl[sr_id][popcount(offloadInfo.channel_mask) - 1];
            max_bitrate = wmaStdMaxAvgByteRateTbl[sr_id][popcount(offloadInfo.channel_mask) - 1];
            if ((offloadInfo.bit_rate > max_bitrate) || (offloadInfo.bit_rate < min_bitrate)) {
                ALOGD("offload disabled for WMA clips with unsupported bit rate");
                ALOGD("bit_rate %d, max_bitrate %d, min_bitrate %d", offloadInfo.bit_rate, max_bitrate, min_bitrate);
                return false;
            }
        }

        // Safely choose the min bitrate as threshold and leave the restriction to NT decoder as we can't distinguish wma pro and wma lossless here.
        if ((((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA_PRO) && (offloadInfo.bit_rate > MAX_BITRATE_WMA_PRO)) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA_PRO) && (offloadInfo.bit_rate > MAX_BITRATE_WMA_LOSSLESS))) {
            ALOGD("offload disabled for WMA_PRO/WMA_LOSSLESS clips with bit rate over maximum supported value");
            return false;
        }
    }

    //TODO: enable audio offloading with video when ready
    if (offloadInfo.has_video && !mApmConfigs->isAudioOffloadVideoEnabled()) {
        ALOGV("isOffloadSupported: has_video == true, returning false");
        return false;
    }

    if (offloadInfo.has_video && offloadInfo.is_streaming &&
            !mApmConfigs->isAVStreamingOffloadEnabled()) {
        ALOGW("offload disabled by vendor.audio.av.streaming.offload.enable %d",
               mApmConfigs->isAVStreamingOffloadEnabled());
        return false;
    }

    //If duration is less than minimum value defined in property, return false
    if (mApmConfigs->getAudioOffloadMinDuration() > 0) {
        if (offloadInfo.duration_us < (mApmConfigs->getAudioOffloadMinDuration() * 1000000 )) {
            ALOGV("Offload denied by duration < audio.offload.min.duration.secs(=%u)", mApmConfigs->getAudioOffloadMinDuration());
            return false;
        }
    } else if (offloadInfo.duration_us < OFFLOAD_DEFAULT_MIN_DURATION_SECS * 1000000) {
        ALOGV("Offload denied by duration < default min(=%u)", OFFLOAD_DEFAULT_MIN_DURATION_SECS);
        //duration checks only valid for MP3/AAC/ formats,
        //do not check duration for other audio formats, e.g. AAC/AC3 and amrwb+ formats
        if ((offloadInfo.format == AUDIO_FORMAT_MP3) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_FLAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_VORBIS))
            return false;

        if (mApmConfigs->isExtnFormatsEnabled()) {
            if (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA) ||
                ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA_PRO) ||
                ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_ALAC) ||
                ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_APE) ||
                ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_DSD) ||
                ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC_ADTS))
                return false;
        }
    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    if (mEffects.isNonOffloadableEffectEnabled()) {
        return false;
    }

    // See if there is a profile to support this.
    // AUDIO_DEVICE_NONE
    sp<IOProfile> profile = getProfileForOutput(AUDIO_DEVICE_NONE /*ignore device */,
                                            offloadInfo.sample_rate,
                                            offloadInfo.format,
                                            offloadInfo.channel_mask,
                                            AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD,
                                            true /*directOnly*/);
    ALOGV("isOffloadSupported() profile %sfound", profile != 0 ? "" : "NOT ");
    return (profile != 0);
}

void AudioPolicyManagerCustom::setPhoneState(audio_mode_t state)
{
    ALOGD("setPhoneState() state %d", state);
    // store previous phone state for management of sonification strategy below
    audio_devices_t newDevice = AUDIO_DEVICE_NONE;
    int oldState = mEngine->getPhoneState();

    if (mEngine->setPhoneState(state) != NO_ERROR) {
        ALOGW("setPhoneState() invalid or same state %d", state);
        return;
    }
    /// Opens: can these line be executed after the switch of volume curves???
    if (isStateInCall(oldState)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);

        // force reevaluating accessibility routing when call stops
        mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }

    /**
     * Switching to or from incall state or switching between telephony and VoIP lead to force
     * routing command.
     */
    bool force = ((is_state_in_call(oldState) != is_state_in_call(state))
                  || (is_state_in_call(state) && (state != oldState)));

    // check for device and output changes triggered by new phone state
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();

    sp<SwAudioOutputDescriptor> hwOutputDesc = mPrimaryOutput;
    if (mApmConfigs->isVoiceConcEnabled()) {
        bool prop_playback_enabled = mApmConfigs->isVoicePlayConcDisabled();
        bool prop_rec_enabled = mApmConfigs->isVoiceRecConcDisabled();
        bool prop_voip_enabled = mApmConfigs->isVoiceVOIPConcDisabled();

        if ((AUDIO_MODE_IN_CALL != oldState) && (AUDIO_MODE_IN_CALL == state)) {
            ALOGD("voice_conc:Entering to call mode oldState :: %d state::%d ",
                oldState, state);
            mvoice_call_state = state;
            if (prop_rec_enabled) {
                //Close all active inputs
                Vector<sp <AudioInputDescriptor> > activeInputs = mInputs.getActiveInputs();
                if (activeInputs.size() != 0) {
                   for (size_t i = 0; i <  activeInputs.size(); i++) {
                       sp<AudioInputDescriptor> activeInput = activeInputs[i];
                       switch(activeInput->inputSource()) {
                           case AUDIO_SOURCE_VOICE_UPLINK:
                           case AUDIO_SOURCE_VOICE_DOWNLINK:
                           case AUDIO_SOURCE_VOICE_CALL:
                               ALOGD("voice_conc:FOUND active input during call active: %d",
                                       activeInput->inputSource());
                           break;

                           case  AUDIO_SOURCE_VOICE_COMMUNICATION:
                                if (prop_voip_enabled) {
                                    ALOGD("voice_conc:CLOSING VoIP input source on call setup :%d ",
                                            activeInput->inputSource());
                                    RecordClientVector activeClients = activeInput->clientsList(true /*activeOnly*/);
                                    for (const auto& activeClient : activeClients) {
                                        closeClient(activeClient->portId());
                                    }
                                }
                           break;

                           default:
                               ALOGD("voice_conc:CLOSING input on call setup  for inputSource: %d",
                                       activeInput->inputSource());
                               RecordClientVector activeClients = activeInput->clientsList(true /*activeOnly*/);
                               for (const auto& activeClient : activeClients) {
                                   closeClient(activeClient->portId());
                               }
                           break;
                       }
                   }
               }
            } else if (prop_voip_enabled) {
                Vector<sp <AudioInputDescriptor> > activeInputs = mInputs.getActiveInputs();
                if (activeInputs.size() != 0) {
                    for (size_t i = 0; i <  activeInputs.size(); i++) {
                        sp<AudioInputDescriptor> activeInput = activeInputs[i];
                        if (AUDIO_SOURCE_VOICE_COMMUNICATION == activeInput->inputSource()) {
                            ALOGD("voice_conc:CLOSING VoIP on call setup : %d",activeInput->inputSource());
                            RecordClientVector activeClients = activeInput->clientsList(true /*activeOnly*/);
                            for (const auto& activeClient : activeClients) {
                                closeClient(activeClient->portId());
                            }
                        }
                    }
                }
            }
            if (prop_playback_enabled) {
                // Move tracks associated to this strategy from previous output to new output
                for (int i = AUDIO_STREAM_SYSTEM; i < AUDIO_STREAM_FOR_POLICY_CNT; i++) {
                    ALOGV("voice_conc:Invalidate on call mode for stream :: %d ", i);
                    if (AUDIO_OUTPUT_FLAG_DEEP_BUFFER == mFallBackflag) {
                        if ((AUDIO_STREAM_MUSIC == i) ||
                            (AUDIO_STREAM_VOICE_CALL == i) ) {
                            ALOGD("voice_conc:Invalidate stream type %d", i);
                            mpClientInterface->invalidateStream((audio_stream_type_t)i);
                        }
                    } else if (AUDIO_OUTPUT_FLAG_FAST == mFallBackflag) {
                        ALOGD("voice_conc:Invalidate stream type %d", i);
                        mpClientInterface->invalidateStream((audio_stream_type_t)i);
                    }
                }
            }

            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
                if ( (outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
                   ALOGD("voice_conc:ouput desc / profile is NULL");
                   continue;
                }

                bool isFastFallBackNeeded =
                   ((AUDIO_OUTPUT_FLAG_DEEP_BUFFER | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT_PCM) & outputDesc->mProfile->getFlags());

                if ((AUDIO_OUTPUT_FLAG_FAST == mFallBackflag) && isFastFallBackNeeded) {
                    if (((!outputDesc->isDuplicated() && outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_PRIMARY))
                                && prop_playback_enabled) {
                        ALOGD("voice_conc:calling suspendOutput on call mode for primary output");
                        mpClientInterface->suspendOutput(mOutputs.keyAt(i));
                    } //Close compress all sessions
                    else if ((outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                                    &&  prop_playback_enabled) {
                        ALOGD("voice_conc:calling closeOutput on call mode for COMPRESS output");
                        closeOutput(mOutputs.keyAt(i));
                    }
                    else if ((outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_VOIP_RX)
                                    && prop_voip_enabled) {
                        ALOGD("voice_conc:calling closeOutput on call mode for DIRECT  output");
                        closeOutput(mOutputs.keyAt(i));
                    }
                } else if (AUDIO_OUTPUT_FLAG_DEEP_BUFFER == mFallBackflag) {
                    if (outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_VOIP_RX) {
                        if (prop_voip_enabled) {
                            ALOGD("voice_conc:calling closeOutput on call mode for DIRECT  output");
                            closeOutput(mOutputs.keyAt(i));
                        }
                    }
                    else if (prop_playback_enabled
                               && (outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_DIRECT)) {
                        ALOGD("voice_conc:calling closeOutput on call mode for COMPRESS output");
                        closeOutput(mOutputs.keyAt(i));
                    }
                }
            }
        }

        if ((AUDIO_MODE_IN_CALL == oldState || AUDIO_MODE_IN_COMMUNICATION == oldState) &&
           (AUDIO_MODE_NORMAL == state) && prop_playback_enabled && mvoice_call_state) {
            ALOGD("voice_conc:EXITING from call mode oldState :: %d state::%d \n",oldState, state);
            mvoice_call_state = 0;
            if (AUDIO_OUTPUT_FLAG_FAST == mFallBackflag) {
                //restore PCM (deep-buffer) output after call termination
                for (size_t i = 0; i < mOutputs.size(); i++) {
                    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
                    if ( (outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
                       ALOGD("voice_conc:ouput desc / profile is NULL");
                       continue;
                    }
                    if (!outputDesc->isDuplicated() && outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_PRIMARY) {
                        ALOGD("voice_conc:calling restoreOutput after call mode for primary output");
                        mpClientInterface->restoreOutput(mOutputs.keyAt(i));
                    }
               }
            }
           //call invalidate tracks so that any open streams can fall back to deep buffer/compress path from ULL
            for (int i = AUDIO_STREAM_SYSTEM; i < AUDIO_STREAM_FOR_POLICY_CNT; i++) {
                ALOGV("voice_conc:Invalidate on call mode for stream :: %d ", i);
                if (AUDIO_OUTPUT_FLAG_DEEP_BUFFER == mFallBackflag) {
                    if ((AUDIO_STREAM_MUSIC == i) ||
                        (AUDIO_STREAM_VOICE_CALL == i) ) {
                        mpClientInterface->invalidateStream((audio_stream_type_t)i);
                    }
                } else if (AUDIO_OUTPUT_FLAG_FAST == mFallBackflag) {
                    mpClientInterface->invalidateStream((audio_stream_type_t)i);
                }
            }
        }
    }

    sp<SwAudioOutputDescriptor> outputDesc = NULL;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        outputDesc = mOutputs.valueAt(i);
        if ((outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
            ALOGD("voice_conc:ouput desc / profile is NULL");
            continue;
        }

        if (mApmConfigs->isVoiceDSDConcDisabled() &&
            (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
            (outputDesc->mFormat == AUDIO_FORMAT_DSD)) {
            ALOGD("voice_conc:calling closeOutput on call mode for DSD COMPRESS output");
            closeOutput(mOutputs.keyAt(i));
            // call invalidate for music, so that DSD compress will fallback to deep-buffer.
            mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);
        }

    }

    if (mApmConfigs->isRecPlayConcEnabled()) {
        if (mApmConfigs->isRecPlayConcDisabled()) {
            if (AUDIO_MODE_IN_COMMUNICATION == mEngine->getPhoneState()) {
                ALOGD("phone state changed to MODE_IN_COMM invlaidating music and voice streams");
                // call invalidate for voice streams, so that it can use deepbuffer with VoIP out device from HAL
                mpClientInterface->invalidateStream(AUDIO_STREAM_VOICE_CALL);
                // call invalidate for music, so that compress will fallback to deep-buffer with VoIP out device
                mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);

                // close compress output to make sure session will be closed before timeout(60sec)
                for (size_t i = 0; i < mOutputs.size(); i++) {

                    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
                    if ((outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
                       ALOGD("ouput desc / profile is NULL");
                       continue;
                    }

                    if (outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                        ALOGD("calling closeOutput on call mode for COMPRESS output");
                        closeOutput(mOutputs.keyAt(i));
                    }
                }
            } else if ((oldState == AUDIO_MODE_IN_COMMUNICATION) &&
                        (mEngine->getPhoneState() == AUDIO_MODE_NORMAL)) {
                // call invalidate for music so that music can fallback to compress
                mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);
            }
        }
    }
    mPrevPhoneState = oldState;
    int delayMs = 0;
    if (isStateInCall(state)) {
        nsecs_t sysTime = systemTime();
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            // mute media and sonification strategies and delay device switch by the largest
            // latency of any output where either strategy is active.
            // This avoid sending the ring tone or music tail into the earpiece or headset.
            if ((isStrategyActive(desc, STRATEGY_MEDIA,
                                  SONIFICATION_HEADSET_MUSIC_DELAY,
                                  sysTime) ||
                 isStrategyActive(desc, STRATEGY_SONIFICATION,
                                  SONIFICATION_HEADSET_MUSIC_DELAY,
                                  sysTime)) &&
                    (delayMs < (int)desc->latency()*2)) {
                delayMs = desc->latency()*2;
            }
            setStrategyMute(STRATEGY_MEDIA, true, desc);
            setStrategyMute(STRATEGY_MEDIA, false, desc, MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_MEDIA, true /*fromCache*/));
            setStrategyMute(STRATEGY_SONIFICATION, true, desc);
            setStrategyMute(STRATEGY_SONIFICATION, false, desc, MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_SONIFICATION, true /*fromCache*/));
        }
    }

    if (hasPrimaryOutput()) {
        // Note that despite the fact that getNewOutputDevice() is called on the primary output,
        // the device returned is not necessarily reachable via this output
        audio_devices_t rxDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
        // force routing command to audio hardware when ending call
        // even if no device change is needed
        if (isStateInCall(oldState) && rxDevice == AUDIO_DEVICE_NONE) {
            rxDevice = mPrimaryOutput->device();
        }

        if (state == AUDIO_MODE_IN_CALL) {
            updateCallRouting(rxDevice, delayMs);
        } else if (oldState == AUDIO_MODE_IN_CALL) {
            if (mCallRxPatch != 0) {
                mpClientInterface->releaseAudioPatch(mCallRxPatch->mAfPatchHandle, 0);
                mCallRxPatch.clear();
            }
            if (mCallTxPatch != 0) {
                mpClientInterface->releaseAudioPatch(mCallTxPatch->mAfPatchHandle, 0);
                mCallTxPatch.clear();
            }
            setOutputDevice(mPrimaryOutput, rxDevice, force, 0);
        } else {
            setOutputDevice(mPrimaryOutput, rxDevice, force, 0);
        }
    }
    //update device for all non-primary outputs
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_io_handle_t output = mOutputs.keyAt(i);
        if (output != mPrimaryOutput->mIoHandle) {
            newDevice = getNewOutputDevice(mOutputs.valueFor(output), false /*fromCache*/);
            setOutputDevice(mOutputs.valueFor(output), newDevice, (newDevice != AUDIO_DEVICE_NONE));
        }
    }
    if (isStateInCall(state)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);

       // force reevaluating accessibility routing when call starts
       mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }

    // Flag that ringtone volume must be limited to music volume until we exit MODE_RINGTONE
    if (state == AUDIO_MODE_RINGTONE &&
        isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY)) {
        mLimitRingtoneVolume = true;
    } else {
        mLimitRingtoneVolume = false;
    }
}

void AudioPolicyManagerCustom::setForceUse(audio_policy_force_use_t usage,
                                         audio_policy_forced_cfg_t config)
{
    ALOGD("setForceUse() usage %d, config %d, mPhoneState %d", usage, config, mEngine->getPhoneState());
    if (config == mEngine->getForceUse(usage)) {
        return;
    }

    if (mEngine->setForceUse(usage, config) != NO_ERROR) {
        ALOGW("setForceUse() could not set force cfg %d for usage %d", config, usage);
        return;
    }
    bool forceVolumeReeval = (usage == AUDIO_POLICY_FORCE_FOR_COMMUNICATION) ||
            (usage == AUDIO_POLICY_FORCE_FOR_DOCK) ||
            (usage == AUDIO_POLICY_FORCE_FOR_SYSTEM);

    // check for device and output changes triggered by new force usage
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();

    /*audio policy: workaround for truncated touch sounds*/
    //FIXME: workaround for truncated touch sounds
    // to be removed when the problem is handled by system UI
    uint32_t delayMs = 0;
    uint32_t waitMs = 0;
    if (usage == AUDIO_POLICY_FORCE_FOR_COMMUNICATION) {
        delayMs = TOUCH_SOUND_FIXED_DELAY_MS;
    }
    if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL && hasPrimaryOutput()) {
        audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, true /*fromCache*/);
        if (forceVolumeReeval && (newDevice != AUDIO_DEVICE_NONE)) {
            applyStreamVolumes(mPrimaryOutput, newDevice, delayMs, true);
        }
        waitMs = updateCallRouting(newDevice, delayMs);
    }
    // Use reverse loop to make sure any low latency usecases (generally tones)
    // are not routed before non LL usecases (generally music).
    // We can safely assume that LL output would always have lower index,
    // and use this work-around to avoid routing of output with music stream
    // from the context of short lived LL output.
    // Note: in case output's share backend(HAL sharing is implicit) all outputs
    //       gets routing update while processing first output itself.
    for (size_t i = mOutputs.size(); i > 0; i--) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i-1);
        audio_devices_t newDevice = getNewOutputDevice(outputDesc, true /*fromCache*/);
        if ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) || (outputDesc != mPrimaryOutput)) {
            waitMs = setOutputDevice(outputDesc, newDevice, (newDevice != AUDIO_DEVICE_NONE),
                                     delayMs);

            if (forceVolumeReeval && (newDevice != AUDIO_DEVICE_NONE)) {
                applyStreamVolumes(outputDesc, newDevice, waitMs, true);
            }
         }
    }

    Vector<sp <AudioInputDescriptor> > activeInputs = mInputs.getActiveInputs();
    for (size_t i = 0; i <  activeInputs.size(); i++) {
        sp<AudioInputDescriptor> activeDesc = activeInputs[i];
        // Skip for hotword recording as the input device switch
        // is handled within sound trigger HAL
        if (activeDesc->isSoundTrigger() &&
            activeDesc->source() == AUDIO_SOURCE_HOTWORD) {
            continue;
        }
        audio_devices_t newDevice = getNewInputDevice(activeDesc);
        // Force new input selection if the new device can not be reached via current input
        if (activeDesc->mProfile->getSupportedDevices().types() &
                (newDevice & ~AUDIO_DEVICE_BIT_IN)) {
            setInputDevice(activeDesc->mIoHandle, newDevice);
        } else {
            closeInput(activeDesc->mIoHandle);
        }
    }
}

status_t AudioPolicyManagerCustom::stopSource(const sp<SwAudioOutputDescriptor>& outputDesc,
                                              const sp<TrackClientDescriptor>& client)
{
    audio_stream_type_t stream = client->stream();

    if (stream < 0 || stream >= AUDIO_STREAM_CNT) {
        ALOGW("stopSource() invalid stream %d", stream);
        return INVALID_OPERATION;
    }
    // always handle stream stop, check which stream type is stopping
    handleEventForBeacon(stream == AUDIO_STREAM_TTS ? STOPPING_BEACON : STOPPING_OUTPUT);

    if (outputDesc->streamActiveCount(stream) > 0) {
        if (outputDesc->streamActiveCount(stream) == 1) {
            // Automatically disable the remote submix input when output is stopped on a
            // re routing mix of type MIX_TYPE_RECORDERS
            if (audio_is_remote_submix_device(outputDesc->mDevice) &&
                outputDesc->mPolicyMix != NULL &&
                outputDesc->mPolicyMix->mMixType == MIX_TYPE_RECORDERS) {
                setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                                            AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                            outputDesc->mPolicyMix->mDeviceAddress,
                                            "remote-submix");
            }
        }
        bool forceDeviceUpdate = false;
        if (client->hasPreferredDevice(true)) {
            checkStrategyRoute(getStrategy(stream), AUDIO_IO_HANDLE_NONE);
            forceDeviceUpdate = true;
        }
        // decrement usage count of this stream on the output
        outputDesc->changeStreamActiveCount(client, -1);
        client->setActive(false);

        // store time at which the stream was stopped - see isStreamActive()
        if (outputDesc->streamActiveCount(stream) == 0 || forceDeviceUpdate) {
            outputDesc->mStopTime[stream] = systemTime();
            audio_devices_t prevDevice = outputDesc->device();
            audio_devices_t newDevice = getNewOutputDevice(outputDesc, false /*fromCache*/);
            // delay the device switch by twice the latency because stopOutput() is executed when
            // the track stop() command is received and at that time the audio track buffer can
            // still contain data that needs to be drained. The latency only covers the audio HAL
            // and kernel buffers. Also the latency does not always include additional delay in the
            // audio path (audio DSP, CODEC ...)
            setOutputDevice(outputDesc, newDevice, false, outputDesc->latency()*2);

            // force restoring the device selection on other active outputs if it differs from the
            // one being selected for this output
            for (size_t i = 0; i < mOutputs.size(); i++) {
                audio_io_handle_t curOutput = mOutputs.keyAt(i);
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
                if (desc != outputDesc &&
                        desc->isActive() &&
                        outputDesc->sharesHwModuleWith(desc) &&
                        (newDevice != desc->device())) {
                        audio_devices_t dev = getNewOutputDevice(mOutputs.valueFor(curOutput), false /*fromCache*/);
                        bool force = prevDevice != dev;
                        uint32_t delayMs;
                        if (dev == prevDevice) {
                            delayMs = 0;
                        } else {
                            delayMs = outputDesc->latency()*2;
                        }
                        setOutputDevice(desc,
                                    dev,
                                    force,
                                    delayMs);
                    /*audio policy: fix media volume after ringtone*/
                    // re-apply device specific volume if not done by setOutputDevice()
                     if (!force) {
                         applyStreamVolumes(desc, dev, delayMs);
                     }
                }
            }
            // update the outputs if stopping one with a stream that can affect notification routing
            handleNotificationRoutingForStream(stream);
        }
        if (stream == AUDIO_STREAM_MUSIC) {
            selectOutputForMusicEffects();
        }
        return NO_ERROR;
    } else {
        ALOGW("stopOutput() refcount is already 0");
        return INVALID_OPERATION;
    }
}

status_t AudioPolicyManagerCustom::startSource(const sp<SwAudioOutputDescriptor>& outputDesc,
                                               const sp<TrackClientDescriptor>& client,
                                               uint32_t *delayMs)
{
    // cannot start playback of STREAM_TTS if any other output is being used
    uint32_t beaconMuteLatency = 0;
    audio_stream_type_t stream = client->stream();

    if (stream < 0 || stream >= AUDIO_STREAM_CNT) {
        ALOGW("startSource() invalid stream %d", stream);
        return INVALID_OPERATION;
    }

    *delayMs = 0;
    if (stream == AUDIO_STREAM_TTS) {
        ALOGV("\t found BEACON stream");
        if (!mTtsOutputAvailable && mOutputs.isAnyOutputActive(AUDIO_STREAM_TTS /*streamToIgnore*/)) {
            return INVALID_OPERATION;
        } else {
            beaconMuteLatency = handleEventForBeacon(STARTING_BEACON);
        }
    } else {
        // some playback other than beacon starts
        beaconMuteLatency = handleEventForBeacon(STARTING_OUTPUT);
    }

    // force device change if the output is inactive and no audio patch is already present.
    // check active before incrementing usage count
    bool force = !outputDesc->isActive() &&
            (outputDesc->getPatchHandle() == AUDIO_PATCH_HANDLE_NONE);

    audio_devices_t device = AUDIO_DEVICE_NONE;
    AudioMix *policyMix = NULL;
    const char *address = NULL;
    if (outputDesc->mPolicyMix != NULL) {
        policyMix = outputDesc->mPolicyMix;
        address = policyMix->mDeviceAddress.string();
        if ((policyMix->mRouteFlags & MIX_ROUTE_FLAG_RENDER) == MIX_ROUTE_FLAG_RENDER) {
            device = policyMix->mDeviceType;
        } else {
            device = AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
        }
    }

    // increment usage count for this stream on the requested output:
    // NOTE that the usage count is the same for duplicated output and hardware output which is
    // necessary for a correct control of hardware output routing by startOutput() and stopOutput()
    outputDesc->changeStreamActiveCount(client, 1);

    if (stream == AUDIO_STREAM_MUSIC) {
        selectOutputForMusicEffects();
    }

    if (outputDesc->streamActiveCount(stream) == 1 || device != AUDIO_DEVICE_NONE) {
        // starting an output being rerouted?
        if (device == AUDIO_DEVICE_NONE) {
            device = getNewOutputDevice(outputDesc, false /*fromCache*/);
        }
        routing_strategy strategy = getStrategy(stream);
        bool shouldWait = (strategy == STRATEGY_SONIFICATION) ||
                            (strategy == STRATEGY_SONIFICATION_RESPECTFUL) ||
                            (beaconMuteLatency > 0);
        uint32_t waitMs = beaconMuteLatency;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc != outputDesc) {
                // force a device change if any other output is:
                // - managed by the same hw module
                // - has a current device selection that differs from selected device.
                // - supports currently selected device
                // - has an active audio patch
                // In this case, the audio HAL must receive the new device selection so that it can
                // change the device currently selected by the other active output.
                if (outputDesc->sharesHwModuleWith(desc) &&
                        desc->device() != device &&
                        desc->supportedDevices() & device &&
                        desc->getPatchHandle() != AUDIO_PATCH_HANDLE_NONE) {
                    force = true;
                }
                // wait for audio on other active outputs to be presented when starting
                // a notification so that audio focus effect can propagate, or that a mute/unmute
                // event occurred for beacon
                uint32_t latency = desc->latency();
                if (shouldWait && desc->isActive(latency * 2) && (waitMs < latency)) {
                    waitMs = latency;
                }
            }
        }
        uint32_t muteWaitMs = setOutputDevice(outputDesc, device, force, 0, NULL, address);

        // apply volume rules for current stream and device if necessary
        checkAndSetVolume(stream,
                          mVolumeCurves->getVolumeIndex(stream, device),
                          outputDesc,
                          device);

        // update the outputs if starting an output with a stream that can affect notification
        // routing
        handleNotificationRoutingForStream(stream);

        // force reevaluating accessibility routing when ringtone or alarm starts
        if (strategy == STRATEGY_SONIFICATION) {
            mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
        }
        if (waitMs > muteWaitMs) {
            *delayMs = waitMs - muteWaitMs;
        }

    }
    return NO_ERROR;
}

void AudioPolicyManagerCustom::handleNotificationRoutingForStream(audio_stream_type_t stream) {
    switch(stream) {
    case AUDIO_STREAM_MUSIC:
        checkOutputForStrategy(STRATEGY_SONIFICATION_RESPECTFUL);
        updateDevicesAndOutputs();
        break;
    default:
        break;
    }
}

status_t AudioPolicyManagerCustom::checkAndSetVolume(audio_stream_type_t stream,
                                                   int index,
                                                   const sp<AudioOutputDescriptor>& outputDesc,
                                                   audio_devices_t device,
                                                   int delayMs,
                                                   bool force)
{
    if (stream < 0 || stream >= AUDIO_STREAM_CNT) {
        ALOGW("checkAndSetVolume() invalid stream %d", stream);
        return INVALID_OPERATION;
    }
    // do not change actual stream volume if the stream is muted
    if (outputDesc->mMuteCount[stream] != 0) {
        ALOGVV("checkAndSetVolume() stream %d muted count %d",
              stream, outputDesc->mMuteCount[stream]);
        return NO_ERROR;
    }
    audio_policy_forced_cfg_t forceUseForComm =
            mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION);
    // do not change in call volume if bluetooth is connected and vice versa
    if ((stream == AUDIO_STREAM_VOICE_CALL && forceUseForComm == AUDIO_POLICY_FORCE_BT_SCO) ||
        (stream == AUDIO_STREAM_BLUETOOTH_SCO && forceUseForComm != AUDIO_POLICY_FORCE_BT_SCO)) {
        ALOGV("checkAndSetVolume() cannot set stream %d volume with force use = %d for comm",
             stream, forceUseForComm);
        return INVALID_OPERATION;
    }

    if (device == AUDIO_DEVICE_NONE) {
        device = outputDesc->device();
    }

    float volumeDb = computeVolume(stream, index, device);
    if (outputDesc->isFixedVolume(device)) {
        volumeDb = 0.0f;
    }

    outputDesc->setVolume(volumeDb, stream, device, delayMs, force);

    if (stream == AUDIO_STREAM_VOICE_CALL ||
        stream == AUDIO_STREAM_BLUETOOTH_SCO) {
        float voiceVolume;
        // Force voice volume to max for bluetooth SCO as volume is managed by the headset
        if (stream == AUDIO_STREAM_VOICE_CALL) {
            voiceVolume = (float)index/(float)mVolumeCurves->getVolumeIndexMax(stream);
        } else {
            voiceVolume = 1.0;
        }

        if (voiceVolume != mLastVoiceVolume) {
            mpClientInterface->setVoiceVolume(voiceVolume, delayMs);
            mLastVoiceVolume = voiceVolume;
        }
    } else if (mApmConfigs->isFMPowerOptEnabled() &&
               stream == AUDIO_STREAM_MUSIC && hasPrimaryOutput() &&
               outputDesc == mPrimaryOutput && mFMIsActive) {
        /* Avoid unnecessary set_parameter calls as it puts the primary
           outputs FastMixer in HOT_IDLE leading to breaks in audio */
        if (volumeDb != mPrevFMVolumeDb) {
            mPrevFMVolumeDb = volumeDb;
            AudioParameter param = AudioParameter();
            param.addFloat(String8("fm_volume"), Volume::DbToAmpl(volumeDb));
            //Double delayMs to avoid sound burst while device switch.
            mpClientInterface->setParameters(mPrimaryOutput->mIoHandle, param.toString(), delayMs*2);
        }
    }

    return NO_ERROR;
}

bool AudioPolicyManagerCustom::isDirectOutput(audio_io_handle_t output) {
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_io_handle_t curOutput = mOutputs.keyAt(i);
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if ((curOutput == output) && (desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            return true;
        }
    }
    return false;
}

bool static tryForDirectPCM(audio_output_flags_t flags)
{
    bool trackDirectPCM = false;  // Output request for track created by other apps

    if (flags == AUDIO_OUTPUT_FLAG_NONE) {
        if (AudioPolicyManagerCustom::mApmConfigs != NULL)
            trackDirectPCM = AudioPolicyManagerCustom::mApmConfigs->isAudioTrackOffloadEnabled();
    }
    return trackDirectPCM;
}

status_t AudioPolicyManagerCustom::getOutputForAttr(const audio_attributes_t *attr,
                                                    audio_io_handle_t *output,
                                                    audio_session_t session,
                                                    audio_stream_type_t *stream,
                                                    uid_t uid,
                                                    const audio_config_t *config,
                                                    audio_output_flags_t *flags,
                                                    audio_port_handle_t *selectedDeviceId,
                                                    audio_port_handle_t *portId)
{
    audio_offload_info_t tOffloadInfo = AUDIO_INFO_INITIALIZER;
    audio_config_t tConfig;

    uint32_t bitWidth = (audio_bytes_per_sample(config->format) * 8);

    memcpy(&tConfig, config, sizeof(audio_config_t));
    if ((*flags == AUDIO_OUTPUT_FLAG_DIRECT || tryForDirectPCM(*flags)) &&
        (!memcmp(&config->offload_info, &tOffloadInfo, sizeof(audio_offload_info_t)))) {
        tConfig.offload_info.sample_rate  = config->sample_rate;
        tConfig.offload_info.channel_mask = config->channel_mask;
        tConfig.offload_info.format = config->format;
        tConfig.offload_info.stream_type = *stream;
        tConfig.offload_info.bit_width = bitWidth;
        if (attr != NULL) {
            ALOGV("found attribute .. setting usage %d ", attr->usage);
            tConfig.offload_info.usage = attr->usage;
        } else {
            ALOGI("%s:: attribute is NULL .. no usage set", __func__);
        }
    }

    return AudioPolicyManager::getOutputForAttr(attr, output, session, stream,
                                                (uid_t)uid, &tConfig,
                                                flags,
                                                (audio_port_handle_t*)selectedDeviceId,
                                                portId);
}

audio_io_handle_t AudioPolicyManagerCustom::getOutputForDevice(
        audio_devices_t device,
        audio_session_t session,
        audio_stream_type_t stream,
        const audio_config_t *config,
        audio_output_flags_t *flags)
{
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    status_t status;

    if (stream < AUDIO_STREAM_MIN || stream >= AUDIO_STREAM_CNT) {
        ALOGE("%s: invalid stream %d", __func__, stream);
        return AUDIO_IO_HANDLE_NONE;
    }

    if (((*flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) &&
            (stream != AUDIO_STREAM_MUSIC)) {
        // compress should not be used for non-music streams
        ALOGE("Offloading only allowed with music stream");
        return 0;
       }

    if (mApmConfigs->isCompressVOIPEnabled()) {
        if (stream == AUDIO_STREAM_VOICE_CALL &&
            audio_is_linear_pcm(config->format)) {
            // let voice stream to go with primary output by default
            // in case direct voip is bypassed
            bool use_primary_out = true;

            if ((config->channel_mask == 1) &&
                    (config->sample_rate == 8000 || config->sample_rate == 16000 ||
                    config->sample_rate == 32000 || config->sample_rate == 48000)) {
                // Allow Voip direct output only if:
                // audio mode is MODE_IN_COMMUNCATION; AND
                // voip output is not opened already; AND
                // requested sample rate matches with that of voip input stream (if opened already)
                int value = 0;
                uint32_t voipOutCount = 1, voipSampleRate = 1;

                String8 valueStr = mpClientInterface->getParameters((audio_io_handle_t)0,
                                                  String8("voip_out_stream_count"));
                AudioParameter result = AudioParameter(valueStr);
                if (result.getInt(String8("voip_out_stream_count"), value) == NO_ERROR) {
                    voipOutCount = value;
                }

                valueStr = mpClientInterface->getParameters((audio_io_handle_t)0,
                                                  String8("voip_sample_rate"));
                result = AudioParameter(valueStr);
                if (result.getInt(String8("voip_sample_rate"), value) == NO_ERROR) {
                    voipSampleRate = value;
                }

                if ((voipOutCount == 0) &&
                    ((voipSampleRate == 0) || (voipSampleRate == config->sample_rate))) {
                    if (mApmConfigs->useVoicePathForPCMVOIP()
                            && (config->format == AUDIO_FORMAT_PCM_16_BIT)) {
                        *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_VOIP_RX |
                                                     AUDIO_OUTPUT_FLAG_DIRECT);
                        ALOGD("Set VoIP and Direct output flags for PCM format");
                        use_primary_out = false;
                    }
                }
            }

            if (use_primary_out) {
                *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_FAST|AUDIO_OUTPUT_FLAG_PRIMARY);
            }
        }
    } else {
        if (stream == AUDIO_STREAM_VOICE_CALL &&
            audio_is_linear_pcm(config->format)) {
            //check if VoIP output is not opened already
            bool voip_pcm_already_in_use = false;
            for (size_t i = 0; i < mOutputs.size(); i++) {
                 sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
                 if (desc->mFlags == (AUDIO_OUTPUT_FLAG_VOIP_RX | AUDIO_OUTPUT_FLAG_DIRECT)) {
                     //close voip output if currently open by the same client with different device
                     if (desc->mDirectClientSession == session &&
                         desc->device() != device) {
                         closeOutput(desc->mIoHandle);
                     } else {
                         voip_pcm_already_in_use = true;
                         ALOGD("VoIP PCM already in use");
                     }
                     break;
                 }
            }

            if (!voip_pcm_already_in_use) {
                *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_VOIP_RX |
                                               AUDIO_OUTPUT_FLAG_DIRECT);
                ALOGV("Set VoIP and Direct output flags for PCM format");
            }
        }
    } /* compress_voip_enabled */

    //IF VOIP is going to be started at the same time as when
    //vr is enabled, get VOIP to fallback to low latency
    String8 vr_value;
    String8 value_Str;
    bool is_vr_mode_on = false;
    AudioParameter ret;

    value_Str =  mpClientInterface->getParameters((audio_io_handle_t)0,
                                          String8("vr_audio_mode_on"));
    ret = AudioParameter(value_Str);
    if (ret.get(String8("vr_audio_mode_on"), vr_value) == NO_ERROR) {
        is_vr_mode_on = vr_value.contains("true");
        ALOGI("VR mode is %d, switch to primary output if request is for fast|raw",
            is_vr_mode_on);
    }

    if (is_vr_mode_on) {
         //check the flags being requested for, and clear FAST|RAW
        *flags = (audio_output_flags_t)(*flags &
            (~(AUDIO_OUTPUT_FLAG_FAST|AUDIO_OUTPUT_FLAG_RAW)));

    }

    if (mApmConfigs->isVoiceConcEnabled()) {
        bool prop_play_enabled = false, prop_voip_enabled = false;
        prop_play_enabled = mApmConfigs->isVoicePlayConcDisabled();
        prop_voip_enabled = mApmConfigs->isVoiceVOIPConcDisabled();

        bool isDeepBufferFallBackNeeded =
            ((AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT_PCM) & *flags);
        bool isFastFallBackNeeded =
            ((AUDIO_OUTPUT_FLAG_DEEP_BUFFER | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT_PCM) & *flags);

        if (prop_play_enabled && mvoice_call_state) {
            //check if voice call is active  / running in background
            if ((AUDIO_MODE_IN_CALL == mEngine->getPhoneState()) ||
                 ((AUDIO_MODE_IN_CALL == mPrevPhoneState)
                    && (AUDIO_MODE_IN_COMMUNICATION == mEngine->getPhoneState())))
            {
                if (AUDIO_OUTPUT_FLAG_VOIP_RX  & *flags) {
                    if (prop_voip_enabled) {
                       ALOGD("voice_conc:getoutput:IN call mode return no o/p for VoIP %x",
                            *flags );
                       return 0;
                    }
                }
                else {
                    if (isFastFallBackNeeded &&
                        (AUDIO_OUTPUT_FLAG_FAST == mFallBackflag)) {
                        ALOGD("voice_conc:IN call mode adding ULL flags .. flags: %x ", *flags );
                        *flags = AUDIO_OUTPUT_FLAG_FAST;
                    } else if (isDeepBufferFallBackNeeded &&
                               (AUDIO_OUTPUT_FLAG_DEEP_BUFFER == mFallBackflag)) {
                        if (AUDIO_STREAM_MUSIC == stream) {
                            *flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
                            ALOGD("voice_conc:IN call mode adding deep-buffer flags %x ", *flags );
                        }
                        else {
                            *flags = AUDIO_OUTPUT_FLAG_FAST;
                            ALOGD("voice_conc:IN call mode adding fast flags %x ", *flags );
                        }
                    }
                }
            }
        } else if (prop_voip_enabled && mvoice_call_state) {
            //check if voice call is active  / running in background
            //some of VoIP apps(like SIP2SIP call) supports resume of VoIP call when call in progress
            //return only ULL ouput
            if ((AUDIO_MODE_IN_CALL == mEngine->getPhoneState()) ||
                 ((AUDIO_MODE_IN_CALL == mPrevPhoneState)
                    && (AUDIO_MODE_IN_COMMUNICATION == mEngine->getPhoneState())))
            {
                if (AUDIO_OUTPUT_FLAG_VOIP_RX  & *flags) {
                        ALOGD("voice_conc:getoutput:IN call mode return no o/p for VoIP %x",
                            *flags );
                   return 0;
                }
            }
        }
    }
    if (mApmConfigs->isRecPlayConcEnabled()) {
        bool prop_rec_play_enabled = mApmConfigs->isRecPlayConcDisabled();
        if ((prop_rec_play_enabled) &&
                ((true == mIsInputRequestOnProgress) || (mInputs.activeInputsCountOnDevices() > 0))) {
            if (AUDIO_MODE_IN_COMMUNICATION == mEngine->getPhoneState()) {
                if (AUDIO_OUTPUT_FLAG_VOIP_RX & *flags) {
                    // allow VoIP using voice path
                    // Do nothing
                } else if ((*flags & AUDIO_OUTPUT_FLAG_FAST) == 0) {
                    ALOGD("voice_conc:MODE_IN_COMM is setforcing deep buffer output for non ULL... flags: %x", *flags);
                    // use deep buffer path for all non ULL outputs
                    *flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
                }
            } else if ((*flags & AUDIO_OUTPUT_FLAG_FAST) == 0) {
                ALOGD("voice_conc:Record mode is on forcing deep buffer output for non ULL... flags: %x ", *flags);
                // use deep buffer path for all non ULL outputs
                *flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
            }
        }
        if (prop_rec_play_enabled &&
                (stream == AUDIO_STREAM_ENFORCED_AUDIBLE)) {
               ALOGD("Record conc is on forcing ULL output for ENFORCED_AUDIBLE");
               *flags = AUDIO_OUTPUT_FLAG_FAST;
        }
    }

    /*
    * WFD audio routes back to target speaker when starting a ringtone playback.
    * This is because primary output is reused for ringtone, so output device is
    * updated based on SONIFICATION strategy for both ringtone and music playback.
    * The same issue is not seen on remoted_submix HAL based WFD audio because
    * primary output is not reused and a new output is created for ringtone playback.
    * Issue is fixed by updating output flag to AUDIO_OUTPUT_FLAG_FAST when there is
    * a non-music stream playback on WFD, so primary output is not reused for ringtone.
    */
    if (mApmConfigs->isAFEProxyEnabled()) {
        audio_devices_t availableOutputDeviceTypes = mAvailableOutputDevices.types();
        if ((availableOutputDeviceTypes & AUDIO_DEVICE_OUT_PROXY)
              && (stream != AUDIO_STREAM_MUSIC)) {
            ALOGD("WFD audio: use OUTPUT_FLAG_FAST for non music stream. flags:%x", *flags );
            //For voip paths
            if (*flags & AUDIO_OUTPUT_FLAG_DIRECT)
                *flags = AUDIO_OUTPUT_FLAG_DIRECT;
            else //route every thing else to ULL path
                *flags = AUDIO_OUTPUT_FLAG_FAST;
        }
    }

    // open a direct output if required by specified parameters
    // force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((*flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    if ((*flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) {
        *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }

    // Do internal direct magic here
    bool offload_disabled = property_get_bool("audio.offload.disable", false);
    if ((*flags == AUDIO_OUTPUT_FLAG_NONE) &&
        (stream == AUDIO_STREAM_MUSIC) &&
        ( !offload_disabled) &&
        ((config->offload_info.usage == AUDIO_USAGE_MEDIA) ||
        (config->offload_info.usage == AUDIO_USAGE_GAME))) {
        audio_output_flags_t old_flags = *flags;
        *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_DIRECT);
        ALOGD("Force Direct Flag .. old flags(0x%x)", old_flags);
    } else if (*flags == AUDIO_OUTPUT_FLAG_DIRECT &&
                (offload_disabled || stream != AUDIO_STREAM_MUSIC)) {
        ALOGD("Offloading is disabled or Stream is not music --> Force Remove Direct Flag");
        *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_NONE);
    }

    // check if direct output for pcm/track offload already exits
    bool direct_pcm_already_in_use = false;
    if (*flags == AUDIO_OUTPUT_FLAG_DIRECT) {
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc->mFlags == AUDIO_OUTPUT_FLAG_DIRECT) {
                direct_pcm_already_in_use = true;
                ALOGD("Direct PCM already in use");
                break;
            }
        }
        // prevent direct pcm for non-music stream blindly if direct pcm already in use
        // for other music stream concurrency is handled after checking direct ouput usage
        // and checking client
        if (direct_pcm_already_in_use == true && stream != AUDIO_STREAM_MUSIC) {
            ALOGD("disabling offload for non music stream as direct pcm is already in use");
            *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_NONE);
        }
    }

    bool forced_deep = false;
    // only allow deep buffering for music stream type
    if (stream != AUDIO_STREAM_MUSIC) {
        *flags = (audio_output_flags_t)(*flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
    } else if (/* stream == AUDIO_STREAM_MUSIC && */
            (*flags == AUDIO_OUTPUT_FLAG_NONE || *flags == AUDIO_OUTPUT_FLAG_DIRECT) &&
            mApmConfigs->isAudioDeepbufferMediaEnabled() && !isInCall()) {
            forced_deep = true;
    }

    if (stream == AUDIO_STREAM_TTS) {
        *flags = AUDIO_OUTPUT_FLAG_TTS;
    } else if (stream == AUDIO_STREAM_VOICE_CALL &&
               audio_is_linear_pcm(config->format)) {
        *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_VOIP_RX |
                                       AUDIO_OUTPUT_FLAG_DIRECT);
        ALOGV("Set VoIP and Direct output flags for PCM format");
    } else if (device == AUDIO_DEVICE_OUT_TELEPHONY_TX &&
        stream == AUDIO_STREAM_MUSIC &&
        audio_is_linear_pcm(config->format) &&
        isInCall()) {
        *flags = (audio_output_flags_t)AUDIO_OUTPUT_FLAG_INCALL_MUSIC;
    }

    sp<IOProfile> profile;

    // skip direct output selection if the request can obviously be attached to a mixed output
    // and not explicitly requested
    if (((*flags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) &&
            audio_is_linear_pcm(config->format) && config->sample_rate <= SAMPLE_RATE_HZ_MAX &&
            audio_channel_count_from_out_mask(config->channel_mask) <= 2) {
        goto non_direct_output;
    }

    // Do not allow offloading if one non offloadable effect is enabled or MasterMono is enabled.
    // This prevents creating an offloaded track and tearing it down immediately after start
    // when audioflinger detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    //
    // Supplementary annotation:
    // For sake of track offload introduced, we need a rollback for both compress offload
    // and track offload use cases.
    if ((*flags & (AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD|AUDIO_OUTPUT_FLAG_DIRECT)) &&
                (mEffects.isNonOffloadableEffectEnabled() || mMasterMono)) {
        ALOGD("non offloadable effect is enabled, try with non direct output");
        goto non_direct_output;
    }

    profile = getProfileForOutput(device,
                                        config->sample_rate,
                                        config->format,
                                        config->channel_mask,
                                        (audio_output_flags_t)*flags,
                                        true /* directOnly */);

    if (profile != 0) {

        if (!(*flags & AUDIO_OUTPUT_FLAG_DIRECT) &&
             (profile->getFlags() & AUDIO_OUTPUT_FLAG_DIRECT)) {
            ALOGI("got Direct without requesting ... reject ");
            profile = NULL;
            goto non_direct_output;
        }

        if ((*flags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) == 0 || output != AUDIO_IO_HANDLE_NONE) {
            sp<SwAudioOutputDescriptor> outputDesc = NULL;
            // if multiple concurrent offload decode is supported
            // do no check for reuse and also don't close previous output if its offload
            // previous output will be closed during track destruction
            if (!mApmConfigs->isAudioMultipleOffloadEnable() &&
                    ((*flags & AUDIO_OUTPUT_FLAG_DIRECT) != 0)) {
                for (size_t i = 0; i < mOutputs.size(); i++) {
                    sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
                    if (!desc->isDuplicated() && (profile == desc->mProfile)) {
                        outputDesc = desc;
                        // reuse direct output if currently open by the same client
                        // and configured with same parameters
                        if ((config->sample_rate == desc->mSamplingRate) &&
                            audio_formats_match(config->format, desc->mFormat) &&
                            (config->channel_mask == desc->mChannelMask) &&
                            (session == desc->mDirectClientSession)) {
                            desc->mDirectOpenCount++;
                            ALOGV("getOutputForDevice() reusing direct output %d for session %d",
                                mOutputs.keyAt(i), session);
                            return mOutputs.keyAt(i);
                        }
                    }
                    if (outputDesc != NULL) {
                        if (*flags == AUDIO_OUTPUT_FLAG_DIRECT &&
                             direct_pcm_already_in_use == true &&
                             session != outputDesc->mDirectClientSession) {
                             ALOGV("getOutput() do not reuse direct pcm output because current client (%d) "
                                   "is not the same as requesting client (%d) for different output conf",
                             outputDesc->mDirectClientSession, session);
                             goto non_direct_output;
                        }
                        closeOutput(outputDesc->mIoHandle);
                    }
                }
                if (!profile->canOpenNewIo()) {
                    goto non_direct_output;
                }

                outputDesc =
                        new SwAudioOutputDescriptor(profile, mpClientInterface);
                DeviceVector outputDevices = mAvailableOutputDevices.getDevicesFromTypeMask(device);
                String8 address = outputDevices.size() > 0 ? outputDevices.itemAt(0)->address()
                        : String8("");
                status = outputDesc->open(config, device, address, stream, *flags, &output);

                // only accept an output with the requested parameters
                if (status != NO_ERROR ||
                    (config->sample_rate != 0 && config->sample_rate != outputDesc->mSamplingRate) ||
                    (config->format != AUDIO_FORMAT_DEFAULT &&
                             !audio_formats_match(config->format, outputDesc->mFormat)) ||
                    (config->channel_mask != 0 && config->channel_mask != outputDesc->mChannelMask)) {
                    ALOGV("getOutputForDevice() failed opening direct output: output %d sample rate %d %d,"
                            "format %d %d, channel mask %04x %04x", output, config->sample_rate,
                            outputDesc->mSamplingRate, config->format, outputDesc->mFormat,
                            config->channel_mask, outputDesc->mChannelMask);
                    if (output != AUDIO_IO_HANDLE_NONE) {
                        outputDesc->close();
                    }
                    // fall back to mixer output if possible when the direct output could not be open
                    if (audio_is_linear_pcm(config->format) && config->sample_rate <= SAMPLE_RATE_HZ_MAX) {
                        goto non_direct_output;
                    }
                    return AUDIO_IO_HANDLE_NONE;
                }
                for (const std::pair<sp<TrackClientDescriptor>, size_t>& client_pair : mPrimaryOutput->getActiveClients()) {
                    if (client_pair.first->stream() == stream) {
                        outputDesc->changeStreamActiveCount(client_pair.first, -outputDesc->streamActiveCount(stream));
                        break;
                    }
                }
                outputDesc->mStopTime[stream] = 0;
                outputDesc->mDirectOpenCount = 1;
                outputDesc->mDirectClientSession = session;

                addOutput(output, outputDesc);
                mPreviousOutputs = mOutputs;
                ALOGV("getOutputForDevice() returns new direct output %d", output);
                mpClientInterface->onAudioPortListUpdate();
                return output;
            }
        }
    }

non_direct_output:

    // A request for HW A/V sync cannot fallback to a mixed output because time
    // stamps are embedded in audio data
    if ((*flags & (AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ)) != 0) {
        return AUDIO_IO_HANDLE_NONE;
    }

    // ignoring channel mask due to downmix capability in mixer

    // open a non direct output

    // for non direct outputs, only PCM is supported
    if (audio_is_linear_pcm(config->format)) {
        // get which output is suitable for the specified stream. The actual
        // routing change will happen when startOutput() will be called
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(device, mOutputs);

        // at this stage we should ignore the DIRECT flag as no direct output could be found earlier
        *flags = (audio_output_flags_t)(*flags & ~AUDIO_OUTPUT_FLAG_DIRECT);

        if (forced_deep) {
            *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
            ALOGI("setting force DEEP buffer now ");
        } else if (*flags == AUDIO_OUTPUT_FLAG_NONE) {
            // no deep buffer playback is requested hence fallback to primary
            *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_PRIMARY);
            ALOGI("FLAG None hence request for a primary output");
        }

        output = selectOutput(outputs, *flags, config->format);
    }

    ALOGW_IF((output == 0), "getOutputForDevice() could not find output for stream %d, "
            "sampling rate %d, format %#x, channels %#x, flags %#x",
            stream, config->sample_rate, config->format, config->channel_mask, *flags);

    ALOGV("getOutputForDevice() returns output %d", output);

    return output;
}

status_t AudioPolicyManagerCustom::getInputForAttr(const audio_attributes_t *attr,
                                         audio_io_handle_t *input,
                                         audio_session_t session,
                                         uid_t uid,
                                         const audio_config_base_t *config,
                                         audio_input_flags_t flags,
                                         audio_port_handle_t *selectedDeviceId,
                                         input_type_t *inputType,
                                         audio_port_handle_t *portId)
{
    audio_source_t inputSource;
    inputSource = attr->source;

    if (mApmConfigs->isVoiceConcEnabled()) {
        bool prop_rec_enabled = false, prop_voip_enabled = false;
        prop_rec_enabled = mApmConfigs->isVoiceRecConcDisabled();
        prop_voip_enabled = mApmConfigs->isVoiceVOIPConcDisabled();

        if (prop_rec_enabled && mvoice_call_state) {
             //check if voice call is active  / running in background
             //some of VoIP apps(like SIP2SIP call) supports resume of VoIP call when call in progress
             //Need to block input request
            if ((AUDIO_MODE_IN_CALL == mEngine->getPhoneState()) ||
               ((AUDIO_MODE_IN_CALL == mPrevPhoneState) &&
                 (AUDIO_MODE_IN_COMMUNICATION == mEngine->getPhoneState())))
            {
                switch(inputSource) {
                    case AUDIO_SOURCE_VOICE_UPLINK:
                    case AUDIO_SOURCE_VOICE_DOWNLINK:
                    case AUDIO_SOURCE_VOICE_CALL:
                        ALOGD("voice_conc:Creating input during incall mode for inputSource: %d",
                            inputSource);
                    break;

                    case AUDIO_SOURCE_VOICE_COMMUNICATION:
                        if (prop_voip_enabled) {
                           ALOGD("voice_conc:BLOCK VoIP requst incall mode for inputSource: %d",
                            inputSource);
                           return NO_INIT;
                        }
                    break;
                    default:
                        ALOGD("voice_conc:BLOCK VoIP requst incall mode for inputSource: %d",
                            inputSource);
                    return NO_INIT;
                }
            }
        }//check for VoIP flag
        else if (prop_voip_enabled && mvoice_call_state) {
             //check if voice call is active  / running in background
             //some of VoIP apps(like SIP2SIP call) supports resume of VoIP call when call in progress
             //Need to block input request
            if ((AUDIO_MODE_IN_CALL == mEngine->getPhoneState()) ||
               ((AUDIO_MODE_IN_CALL == mPrevPhoneState) &&
                 (AUDIO_MODE_IN_COMMUNICATION == mEngine->getPhoneState())))
            {
                if (inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                    ALOGD("BLOCKING VoIP request during incall mode for inputSource: %d ",inputSource);
                    return NO_INIT;
                }
            }
        }
    }


    return AudioPolicyManager::getInputForAttr(attr,
                                               input,
                                               session,
                                               uid,
                                               config,
                                               flags,
                                               selectedDeviceId,
                                               inputType,
                                               portId);
}

uint32_t AudioPolicyManagerCustom::activeNonSoundTriggerInputsCountOnDevices(audio_devices_t devices) const
{
    uint32_t count = 0;
    for (size_t i = 0; i < mInputs.size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = mInputs.valueAt(i);
        if (inputDescriptor->isActive() && !inputDescriptor->isSoundTrigger() &&
                ((devices == AUDIO_DEVICE_IN_DEFAULT) ||
                 ((inputDescriptor->mDevice & devices & ~AUDIO_DEVICE_BIT_IN) != 0))) {
            count++;
        }
    }
    return count;
}

status_t AudioPolicyManagerCustom::startInput(audio_port_handle_t portId)
{

    ALOGV("startInput(portId:%d)", portId);
    sp<AudioInputDescriptor> inputDesc = mInputs.getInputForClient(portId);
    if (inputDesc == 0) {
        ALOGW("%s no input for client %d", __FUNCTION__, portId);
        return BAD_VALUE;
    }

    audio_io_handle_t input = inputDesc->mIoHandle;

    sp<RecordClientDescriptor> client = inputDesc->getClient(portId);
    if (client->active()) {
        ALOGW("%s input %d client %d already started", __FUNCTION__, input, client->portId());
        return INVALID_OPERATION;
    }

    audio_session_t session = client->session();

    ALOGV("%s input:%d, session:%d)", __FUNCTION__, input, session);


// FIXME: disable concurrent capture until UI is ready
#if 0
    if (!isConcurentCaptureAllowed(inputDesc, audioSession)) {
        ALOGW("startInput(%d) failed: other input already started", input);
        return INVALID_OPERATION;
    }

    if (isInCall()) {
        *concurrency |= API_INPUT_CONCURRENCY_CALL;
    }

    if (mInputs.activeInputsCountOnDevices() != 0) {
        *concurrency |= API_INPUT_CONCURRENCY_CAPTURE;
    }
#endif

    if (mApmConfigs->isRecPlayConcEnabled()) {
        mIsInputRequestOnProgress = true;

        if (mApmConfigs->isRecPlayConcDisabled() && (mInputs.activeInputsCountOnDevices() == 0)) {
            // send update to HAL on record playback concurrency
            AudioParameter param = AudioParameter();
            param.add(String8("rec_play_conc_on"), String8("true"));
            ALOGD("startInput() setParameters rec_play_conc is setting to ON ");
            mpClientInterface->setParameters(0, param.toString());

            // Call invalidate to reset all opened non ULL audio tracks
            // Move tracks associated to this strategy from previous output to new output
            for (int i = AUDIO_STREAM_SYSTEM; i < AUDIO_STREAM_FOR_POLICY_CNT; i++) {
                // Do not call invalidate for ENFORCED_AUDIBLE (otherwise pops are seen for camcorder)
                if (i != AUDIO_STREAM_ENFORCED_AUDIBLE) {
                   ALOGD("Invalidate on releaseInput for stream :: %d ", i);
                   //FIXME see fixme on name change
                   mpClientInterface->invalidateStream((audio_stream_type_t)i);
                }
            }
            // close compress tracks
            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
                if ((outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
                   ALOGD("ouput desc / profile is NULL");
                   continue;
                }
                if (outputDesc->mProfile->getFlags()
                                & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                    // close compress  sessions
                    ALOGD("calling closeOutput on record conc for COMPRESS output");
                    closeOutput(mOutputs.keyAt(i));
                }
            }
        }
    }
    status_t status = inputDesc->start();
    if (status != NO_ERROR) {
        inputDesc->setClientActive(client, false);
        return status;
    }

    // increment activity count before calling getNewInputDevice() below as only active sessions
    // are considered for device selection
    inputDesc->setClientActive(client, true);

    // indicate active capture to sound trigger service if starting capture from a mic on
    // primary HW module
    audio_devices_t device = getNewInputDevice(inputDesc);
    setInputDevice(input, device, true /* force */);

    if (inputDesc->activeCount()  == 1) {
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((inputDesc->mPolicyMix != NULL)
                && ((inputDesc->mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0)) {
            mpClientInterface->onDynamicPolicyMixStateUpdate(inputDesc->mPolicyMix->mDeviceAddress,
                    MIX_STATE_MIXING);
        }

        audio_devices_t primaryInputDevices = availablePrimaryInputDevices();
        if ((device & primaryInputDevices & ~AUDIO_DEVICE_BIT_IN) != 0) {
            if (mApmConfigs->isVAConcEnabled()) {
                if (activeNonSoundTriggerInputsCountOnDevices(primaryInputDevices) == 1)
                    SoundTrigger::setCaptureState(true);
            } else if (mInputs.activeInputsCountOnDevices(primaryInputDevices) == 1)
                SoundTrigger::setCaptureState(true);
        }

        // automatically enable the remote submix output when input is started if not
        // used by a policy mix of type MIX_TYPE_RECORDERS
        // For remote submix (a virtual device), we open only one input per capture request.
        if (audio_is_remote_submix_device(inputDesc->mDevice)) {
            String8 address = String8("");
            if (inputDesc->mPolicyMix == NULL) {
                address = String8("0");
            } else if (inputDesc->mPolicyMix->mMixType == MIX_TYPE_PLAYERS) {
                address = inputDesc->mPolicyMix->mDeviceAddress;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        address, "remote-submix");
            }
        }
    }

    ALOGV("%s input %d source = %d exit", __FUNCTION__, input, client->source());

    if (mApmConfigs->isRecPlayConcEnabled())
        mIsInputRequestOnProgress = false;
    return NO_ERROR;
}

status_t AudioPolicyManagerCustom::stopInput(audio_port_handle_t portId)
{
    status_t status;
    status = AudioPolicyManager::stopInput(portId);
    sp<AudioInputDescriptor> inputDesc = mInputs.getInputForClient(portId);
    if (inputDesc == 0) {
        ALOGW("stopInput() no input for client %d", portId);
        return BAD_VALUE;
    }
    sp<RecordClientDescriptor> client = inputDesc->getClient(portId);
    audio_io_handle_t input = inputDesc->mIoHandle;

    ALOGV("stopInput() input %d", input);
    if (mApmConfigs->isVAConcEnabled()) {
        sp<AudioInputDescriptor> inputDesc = mInputs.getInputForClient(portId);
        audio_devices_t device = inputDesc->mDevice;
        audio_devices_t primaryInputDevices = availablePrimaryInputDevices();
        if (((device & primaryInputDevices & ~AUDIO_DEVICE_BIT_IN) != 0) &&
                activeNonSoundTriggerInputsCountOnDevices(primaryInputDevices) == 0) {
                SoundTrigger::setCaptureState(false);
        }
    }
    if (mApmConfigs->isRecPlayConcEnabled()) {
        if (mApmConfigs->isRecPlayConcDisabled() &&
                (mInputs.activeInputsCountOnDevices() == 0)) {
            //send update to HAL on record playback concurrency
            AudioParameter param = AudioParameter();
            param.add(String8("rec_play_conc_on"), String8("false"));
            ALOGD("stopInput() setParameters rec_play_conc is setting to OFF ");
            mpClientInterface->setParameters(0, param.toString());

            //call invalidate tracks so that any open streams can fall back to deep buffer/compress path from ULL
            for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
                //Do not call invalidate for ENFORCED_AUDIBLE (otherwise pops are seen for camcorder stop tone)
                if ((i != AUDIO_STREAM_ENFORCED_AUDIBLE) && (i != AUDIO_STREAM_PATCH)) {
                   ALOGD(" Invalidate on stopInput for stream :: %d ", i);
                   //FIXME see fixme on name change
                   mpClientInterface->invalidateStream((audio_stream_type_t)i);
                }
            }
        }
    }
    return status;
}

AudioPolicyManagerCustom::AudioPolicyManagerCustom(AudioPolicyClientInterface *clientInterface)
    : AudioPolicyManager(clientInterface),
      mFallBackflag(AUDIO_OUTPUT_FLAG_NONE),
      mHdmiAudioDisabled(false),
      mHdmiAudioEvent(false),
      mPrevPhoneState(0),
      mIsInputRequestOnProgress(false),
      mPrevFMVolumeDb(0.0f),
      mFMIsActive(false)
{
    if (mApmConfigs->useXMLAudioPolicyConf())
        ALOGD("USE_XML_AUDIO_POLICY_CONF is TRUE");
    else
        ALOGD("USE_XML_AUDIO_POLICY_CONF is FALSE");

    if (mApmConfigs->isVoiceConcEnabled())
        mFallBackflag = getFallBackPath();
}

}
